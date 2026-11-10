#include "CommandArguments.hh"

#include <algorithm>
#include <charconv>
#include <set>
#include <stdexcept>

namespace teaserv::extensions {
namespace {
bool separator(char c) { return c == ' ' || c == '\t'; }
} // namespace

CommandArguments::CommandArguments(std::vector<CommandArgument> fields) : fields(std::move(fields)) {
  if (this->fields.size() > 8) {
    throw std::runtime_error("command schemas allow at most eight arguments");
  }
  bool optional = false;
  std::set<std::string> names;
  for (size_t index = 0; index < this->fields.size(); ++index) {
    const auto& field = this->fields[index];
    if (field.name.empty() || field.name.size() > 32 || !std::all_of(field.name.begin(), field.name.end(), [](unsigned char c) {
          return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        }) ||
        !names.emplace(field.name).second) {
      throw std::runtime_error("schema argument names must be unique lowercase ASCII names (1..32 bytes)");
    }
    if ((optional && !field.optional) || (field.type == CommandArgument::Type::TEXT && index + 1 != this->fields.size()) ||
        field.max_bytes == 0 || field.max_bytes > 1024 ||
        (field.type == CommandArgument::Type::CLIENT_ID && field.max_bytes > 20) || field.minimum > field.maximum) {
      throw std::runtime_error("invalid argument schema: trailing optional arguments, final text, and valid bounds required");
    }
    optional |= field.optional;
  }
}

std::optional<ParsedCommandArguments> CommandArguments::parse(const std::string& input) const {
  if (input.size() > 1024 || std::any_of(input.begin(), input.end(), [](unsigned char c) { return (c < 0x20 && c != '\t') || c == 0x7F; })) {
    return std::nullopt;
  }
  ParsedCommandArguments result;
  size_t position = 0;
  for (const auto& field : this->fields) {
    while (position < input.size() && separator(input[position]))
      ++position;
    if (position == input.size()) {
      if (field.optional)
        continue;
      return std::nullopt;
    }
    size_t end = position;
    if (field.type == CommandArgument::Type::TEXT) {
      end = input.size();
    } else {
      while (end < input.size() && !separator(input[end]))
        ++end;
    }
    auto token = input.substr(position, end - position);
    if (token.size() > field.max_bytes)
      return std::nullopt;
    if (field.type == CommandArgument::Type::INTEGER) {
      int64_t value = 0;
      auto [ptr, error] = std::from_chars(token.data(), token.data() + token.size(), value);
      if (error != std::errc() || ptr != token.data() + token.size() || value < field.minimum || value > field.maximum)
        return std::nullopt;
      result.emplace_back(field.name, value);
    } else {
      if (field.type == CommandArgument::Type::CLIENT_ID) {
        uint64_t value = 0;
        auto [ptr, error] = std::from_chars(token.data(), token.data() + token.size(), value);
        if (error != std::errc() || ptr != token.data() + token.size() || !value)
          return std::nullopt;
      }
      result.emplace_back(field.name, std::move(token));
    }
    position = end;
  }
  while (position < input.size() && separator(input[position]))
    ++position;
  if (position != input.size())
    return std::nullopt;
  return result;
}

std::string CommandArguments::usage(const std::string& command) const {
  std::string result = "Usage: " + command;
  for (const auto& field : this->fields) {
    result += field.optional ? " [" : " <";
    result += field.name;
    result += field.optional ? "]" : ">";
  }
  return result;
}

} // namespace teaserv::extensions
