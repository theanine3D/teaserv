#include "TeaservModpackHttp.hh"

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <asio.hpp>
#include <phosg/JSON.hh>
#include <phosg/Strings.hh>

#include "Episode3/CaelumCardMods.hh"
#include "Loggers.hh"
#include "ServerState.hh"

namespace teaserv_modpack_http {
namespace {

using asio::ip::tcp;

phosg::PrefixedLogger http_log("[ModpackHTTP] ", phosg::LogLevel::L_USE_DEFAULT);

constexpr size_t MAX_REQUEST_BYTES = 8192; // request line and headers
constexpr size_t MAX_CONNECTIONS = 32; // further connections are closed at once
constexpr size_t BLOCK_BYTES = 64 * 1024; // file bytes per socket write
constexpr auto REQUEST_TIMEOUT = std::chrono::seconds(20); // to receive the whole request
constexpr auto WRITE_TIMEOUT = std::chrono::seconds(60); // per block; a stalled download is dropped

std::shared_ptr<tcp::acceptor> acceptor;
size_t open_connections = 0;

const char* reason_for(int code) {
  switch (code) {
    case 200:
      return "OK";
    case 400:
      return "Bad Request";
    case 404:
      return "Not Found";
    case 405:
      return "Method Not Allowed";
    case 505:
      return "HTTP Version Not Supported";
    default:
      return "Internal Server Error";
  }
}

// Headers for a response; `body_size` is announced even for HEAD, whose body is then left out.
std::string headers(int code, const char* type, uint64_t body_size, const char* extra = "") {
  return std::format(
      "HTTP/1.1 {} {}\r\nContent-Type: {}\r\nContent-Length: {}\r\n{}Cache-Control: no-cache\r\n"
      "Connection: close\r\nServer: Teaserv\r\n\r\n",
      code, reason_for(code), type, body_size, extra);
}

std::string text_response(int code, const std::string& body, bool head, const char* extra = "") {
  auto ret = headers(code, "text/plain; charset=utf-8", body.size(), extra);
  if (!head) {
    ret += body;
  }
  return ret;
}

int hex_digit(char ch) {
  if (ch >= '0' && ch <= '9') {
    return ch - '0';
  }
  if (ch >= 'a' && ch <= 'f') {
    return ch - 'a' + 10;
  }
  if (ch >= 'A' && ch <= 'F') {
    return ch - 'A' + 10;
  }
  return -1;
}

asio::awaitable<void> write_all(tcp::socket& sock, const void* data, size_t size) {
  co_await asio::async_write(sock, asio::buffer(data, size), asio::cancel_after(WRITE_TIMEOUT, asio::use_awaitable));
}

asio::awaitable<void> serve(tcp::socket sock) {
  struct Count {
    Count() {
      open_connections++;
    }
    ~Count() {
      open_connections--;
    }
  } count;
  std::string peer = "?";
  {
    std::error_code ec;
    auto endpoint = sock.remote_endpoint(ec);
    if (!ec) {
      peer = endpoint.address().to_string();
    }
  }

  try {
    asio::streambuf request(MAX_REQUEST_BYTES);
    std::error_code read_error;
    co_await asio::async_read_until(sock, request, "\r\n\r\n",
        asio::cancel_after(REQUEST_TIMEOUT, asio::redirect_error(asio::use_awaitable, read_error)));
    if (read_error) {
      if (read_error == asio::error::not_found) { // longer than MAX_REQUEST_BYTES
        auto response = text_response(400, "Request too large.\n", false);
        co_await write_all(sock, response.data(), response.size());
      }
      co_return;
    }
    std::string text(asio::buffers_begin(request.data()), asio::buffers_end(request.data()));
    std::string line = text.substr(0, text.find("\r\n"));
    size_t first_space = line.find(' ');
    size_t last_space = line.rfind(' ');
    if (first_space == std::string::npos || last_space == first_space) {
      auto response = text_response(400, "Bad request.\n", false);
      co_await write_all(sock, response.data(), response.size());
      co_return;
    }
    std::string method = line.substr(0, first_space);
    std::string target = line.substr(first_space + 1, last_space - first_space - 1);
    std::string version = line.substr(last_space + 1);
    bool head = (method == "HEAD");

    std::string response;
    if (!version.starts_with("HTTP/1.")) {
      response = text_response(505, "Only HTTP/1.x is supported.\n", head);
    } else if (method != "GET" && !head) {
      response = text_response(405, "Only GET and HEAD are supported.\n", false, "Allow: GET, HEAD\r\n");
    } else if (target == "/") {
      response = text_response(200, "Teaserv Caelum modpack downloads.\n", head);
    }
    if (!response.empty()) {
      co_await write_all(sock, response.data(), response.size());
      co_return;
    }

    auto name = filename_for_target(target);
    std::filesystem::path path;
    std::error_code ec;
    uint64_t size = 0;
    bool found = false;
    if (name) {
      path = caelum::cardmods::pack_dir() /
          std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(name->data()), name->size()));
      auto status = std::filesystem::symlink_status(path, ec);
      if (!ec && std::filesystem::is_regular_file(status)) {
        size = std::filesystem::file_size(path, ec);
        found = !ec;
      }
    }
    std::ifstream file;
    if (found) {
      file.open(path, std::ios::binary);
      found = file.is_open();
    }
    if (!found) {
      response = text_response(404, "No such modpack.\n", head);
      co_await write_all(sock, response.data(), response.size());
      http_log.info_f("{} {} from {}: not found", method, target, peer);
      co_return;
    }

    response = headers(200, "application/zip", size);
    co_await write_all(sock, response.data(), response.size());
    if (head) {
      co_return;
    }
    std::vector<char> block(BLOCK_BYTES);
    for (uint64_t remaining = size; remaining > 0;) {
      size_t bytes = static_cast<size_t>(std::min<uint64_t>(remaining, block.size()));
      file.read(block.data(), static_cast<std::streamsize>(bytes));
      if (static_cast<size_t>(file.gcount()) != bytes) {
        throw std::runtime_error("the file became shorter while it was being sent");
      }
      co_await write_all(sock, block.data(), bytes);
      remaining -= bytes;
    }
    http_log.info_f("Sent {} ({} bytes) to {}", *name, size, peer);
  } catch (const std::exception& e) {
    http_log.info_f("Download to {} ended early: {}", peer, e.what());
  }
  std::error_code ignored;
  sock.shutdown(tcp::socket::shutdown_both, ignored);
  sock.close(ignored);
}

asio::awaitable<void> accept_loop(std::shared_ptr<tcp::acceptor> listener) {
  for (;;) {
    std::error_code ec;
    tcp::socket sock = co_await listener->async_accept(asio::redirect_error(asio::use_awaitable, ec));
    if (ec) {
      if (ec == asio::error::operation_aborted || !listener->is_open()) {
        co_return;
      }
      continue; // for example a connection reset before it was accepted
    }
    if (open_connections >= MAX_CONNECTIONS) {
      std::error_code ignored;
      sock.close(ignored);
      continue;
    }
    asio::co_spawn(listener->get_executor(), serve(std::move(sock)), asio::detached);
  }
}

} // namespace

std::optional<std::string> filename_for_target(const std::string& target) {
  static const std::string prefix = "/modpacks/";
  if (!target.starts_with(prefix)) {
    return std::nullopt;
  }
  size_t query = target.find('?');
  std::string encoded = target.substr(prefix.size(), query == std::string::npos ? std::string::npos : query - prefix.size());
  std::string name;
  for (size_t z = 0; z < encoded.size(); z++) {
    char ch = encoded[z];
    if (ch == '%') {
      if (z + 2 >= encoded.size()) {
        return std::nullopt;
      }
      int high = hex_digit(encoded[z + 1]);
      int low = hex_digit(encoded[z + 2]);
      if (high < 0 || low < 0) {
        return std::nullopt;
      }
      name.push_back(static_cast<char>(high * 16 + low));
      z += 2;
    } else {
      name.push_back(ch);
    }
  }
  if (name.empty() || name.size() > 255 || name[0] == '.') {
    return std::nullopt;
  }
  for (unsigned char ch : name) {
    if (ch < 0x20 || ch == 0x7F || ch == '/' || ch == '\\' || ch == ':') {
      return std::nullopt;
    }
  }
  if (name.size() < 5 || phosg::tolower(name.substr(name.size() - 4)) != ".zip") {
    return std::nullopt;
  }
  return name;
}

void start(std::shared_ptr<ServerState> state) {
  if (acceptor) {
    return;
  }
  bool enabled = false;
  int64_t port = 80;
  try {
    enabled = state->data->config_json->get_bool("TeaservModpackHTTPEnabled", false);
    port = state->data->config_json->get_int("TeaservModpackHTTPPort", 80);
  } catch (const std::exception& e) {
    http_log.warning_f("The modpack download server settings are not valid ({}); the server stays off", e.what());
    return;
  }
  if (!enabled) {
    return;
  }
  if (port < 1 || port > 65535) {
    http_log.warning_f("TeaservModpackHTTPPort must be between 1 and 65535 (it is {}); the download server stays off",
        port);
    return;
  }
  try {
    auto listener = std::make_shared<tcp::acceptor>(*state->io_context);
    tcp::endpoint endpoint(tcp::v4(), static_cast<uint16_t>(port));
    listener->open(endpoint.protocol());
    listener->set_option(tcp::acceptor::reuse_address(true));
    listener->bind(endpoint);
    listener->listen();
    acceptor = listener;
    asio::co_spawn(*state->io_context, accept_loop(listener), asio::detached);
    http_log.info_f("Serving the ZIP files in {} on TCP port {} (/modpacks/<file name>)",
        caelum::cardmods::pack_dir().string(), port);
  } catch (const std::exception& e) {
    http_log.error_f("Cannot start the modpack download server on TCP port {}: {}. Is another program using that "
                     "port? Choose another port in the settings, or turn the download server off.",
        port, e.what());
  }
}

void stop() {
  if (acceptor) {
    std::error_code ignored;
    acceptor->close(ignored);
    acceptor.reset();
  }
}

} // namespace teaserv_modpack_http
