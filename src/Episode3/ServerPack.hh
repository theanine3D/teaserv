#pragma once
// Server modpacks (CSPK1): encrypted packs that only the servers pinned inside them can unlock. Design and wire
// format: pso_ep3_server_modpacks.md. Shared byte-for-byte with Teaserv (src/Episode3/ServerPack.hh), like
// mod_protocol.hpp / Modpacks.hh. Needs monocypher.c compiled into the program.
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "monocypher/monocypher.h"

#include <cstdio>
#ifdef _WIN32
// bcrypt.dll's system random numbers, declared here so the header does not pull in windows.h (link bcrypt)
extern "C" long __stdcall BCryptGenRandom(void* algorithm, unsigned char* buffer, unsigned long size, unsigned long flags);
#else
#include <sys/random.h>
#endif

namespace caelum::serverpack {

constexpr const char* HEADER_FILE = "caelum_server_pack.txt";
constexpr const char* INDEX_FILE = "caelum_server_pack.idx";
constexpr size_t MAX_PACKS = 32;   // as the mod list
constexpr size_t SEAL_OVERHEAD = 24 + 16;

using Key = std::array<uint8_t, 32>;
using PackId = std::array<uint8_t, 16>;

inline void random_bytes(void* out, size_t size) {
#ifdef _WIN32
  if (BCryptGenRandom(nullptr, static_cast<unsigned char*>(out), static_cast<unsigned long>(size), 2 /* BCRYPT_USE_SYSTEM_PREFERRED_RNG */) < 0)
    throw std::runtime_error("No system random numbers");
#else
  for (size_t done = 0; done < size;) {
    const ssize_t n = getrandom(static_cast<uint8_t*>(out) + done, size - done, 0);
    if (n < 0) throw std::runtime_error("No system random numbers");
    done += static_cast<size_t>(n);
  }
#endif
}
template <typename T> inline T random_array() { T out; random_bytes(out.data(), out.size()); return out; }

inline std::string hex(const uint8_t* data, size_t size) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  for (size_t z = 0; z < size; z++) { out += digits[data[z] >> 4]; out += digits[data[z] & 15]; }
  return out;
}
template <typename T> inline std::string hex(const T& a) { return hex(a.data(), a.size()); }
template <typename T> inline T unhex(const std::string& text, const char* what) {
  T out;
  if (text.size() != out.size() * 2) throw std::runtime_error(std::string("Invalid ") + what);
  auto nibble = [&](char c) -> uint8_t {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    throw std::runtime_error(std::string("Invalid ") + what);
  };
  for (size_t z = 0; z < out.size(); z++) out[z] = (nibble(text[z * 2]) << 4) | nibble(text[z * 2 + 1]);
  return out;
}

// ---- pack header (plain entry) -----------------------------------------------------------------------------------
struct Header {
  PackId id{};
  std::vector<Key> servers;   // identity public keys allowed to unlock the pack
  bool allows(const Key& server) const {
    for (const auto& k : servers) if (k == server) return true;
    return false;
  }
};
inline std::string write_header(const Header& h) {
  std::string out = "CSPK1\nid=" + hex(h.id) + "\nservers=";
  for (size_t z = 0; z < h.servers.size(); z++) out += (z ? "," : "") + hex(h.servers[z]);
  return out + "\n";
}
inline Header parse_header(std::string text) {
  if (text.size() > 8192) throw std::runtime_error("Invalid server modpack header");
  std::istringstream lines(text);
  std::string line;
  Header h;
  bool magic = false, id = false;
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!magic) { if (line != "CSPK1") throw std::runtime_error("Unsupported server modpack format"); magic = true; continue; }
    if (line.starts_with("id=")) { h.id = unhex<PackId>(line.substr(3), "server modpack id"); id = true; }
    else if (line.starts_with("servers=")) {
      std::istringstream keys(line.substr(8));
      std::string key;
      while (std::getline(keys, key, ',')) if (!key.empty()) h.servers.push_back(unhex<Key>(key, "server key"));
    }
  }
  if (!magic || !id || h.servers.empty() || h.servers.size() > 16) throw std::runtime_error("Invalid server modpack header");
  return h;
}

// ---- sealed blobs: nonce(24) | mac(16) | ciphertext, ad = "CSPK1" | pack id | label --------------------------------
inline std::string seal_ad(const PackId& id, const std::string& label) {
  return std::string("CSPK1") + std::string(reinterpret_cast<const char*>(id.data()), id.size()) + label;
}
inline std::string seal(const Key& key, const PackId& id, const std::string& label, const std::string& plain) {
  std::string out(SEAL_OVERHEAD + plain.size(), '\0');
  auto* p = reinterpret_cast<uint8_t*>(out.data());
  random_bytes(p, 24);
  const std::string ad = seal_ad(id, label);
  crypto_aead_lock(p + SEAL_OVERHEAD, p + 24, key.data(), p, reinterpret_cast<const uint8_t*>(ad.data()), ad.size(),
                   reinterpret_cast<const uint8_t*>(plain.data()), plain.size());
  return out;
}
inline std::string open(const Key& key, const PackId& id, const std::string& label, const uint8_t* blob, size_t size) {
  if (size < SEAL_OVERHEAD) throw std::runtime_error("Damaged server modpack file");
  std::string out(size - SEAL_OVERHEAD, '\0');
  const std::string ad = seal_ad(id, label);
  if (crypto_aead_unlock(reinterpret_cast<uint8_t*>(out.data()), blob + 24, key.data(), blob,
                         reinterpret_cast<const uint8_t*>(ad.data()), ad.size(), blob + SEAL_OVERHEAD, out.size()) != 0)
    throw std::runtime_error("Server modpack file does not match its key");
  return out;
}

// ---- index ----------------------------------------------------------------------------------------------------------
struct IndexEntry { uint32_t number = 0; uint8_t method = 0; uint32_t size = 0; std::string path; };
inline std::string entry_name(uint32_t number) {
  char name[32];
  std::snprintf(name, sizeof(name), "data/%06u.bin", number);
  return name;
}
inline std::string write_index(const std::vector<IndexEntry>& entries) {
  std::string out;
  for (const auto& e : entries)
    out += std::to_string(e.number) + "\t" + std::to_string(e.method) + "\t" + std::to_string(e.size) + "\t" + e.path + "\n";
  return out;
}
inline std::vector<IndexEntry> parse_index(const std::string& text) {
  std::istringstream lines(text);
  std::string line;
  std::vector<IndexEntry> out;
  while (std::getline(lines, line)) {
    if (line.empty()) continue;
    const auto a = line.find('\t'), b = line.find('\t', a + 1), c = line.find('\t', b + 1);
    if (a == std::string::npos || b == std::string::npos || c == std::string::npos) throw std::runtime_error("Invalid server modpack index");
    IndexEntry e;
    e.number = static_cast<uint32_t>(std::stoul(line.substr(0, a)));
    const unsigned long method = std::stoul(line.substr(a + 1, b - a - 1));
    e.size = static_cast<uint32_t>(std::stoul(line.substr(b + 1, c - b - 1)));
    e.path = line.substr(c + 1);
    if ((method != 0 && method != 8) || e.path.empty() || e.number != out.size()) throw std::runtime_error("Invalid server modpack index");
    e.method = static_cast<uint8_t>(method);
    out.push_back(std::move(e));
  }
  return out;
}

// ---- server identity (EdDSA, Monocypher crypto_eddsa) -----------------------------------------------------------
struct Identity {
  std::array<uint8_t, 64> secret{};
  Key public_key{};
  static Identity from_seed(Key seed) {   // copy: crypto_eddsa_key_pair wipes its seed
    Identity id;
    crypto_eddsa_key_pair(id.secret.data(), id.public_key.data(), seed.data());
    return id;
  }
  ~Identity() { crypto_wipe(secret.data(), secret.size()); }
};

// ---- key exchange: F2 flag 4 (client) and F2 flag 5 (server) -----------------------------------------------------
struct KeyRequest { Key client_public{}; std::vector<PackId> packs; };
inline std::string write_key_request(const KeyRequest& r) {
  if (r.packs.empty() || r.packs.size() > MAX_PACKS) throw std::runtime_error("Invalid server modpack key request");
  std::string out = "CSK4" + std::string(reinterpret_cast<const char*>(r.client_public.data()), 32);
  out += static_cast<char>(r.packs.size());
  for (const auto& id : r.packs) out.append(reinterpret_cast<const char*>(id.data()), id.size());
  return out;
}
// PSO commands are padded with zeros: bytes after the declared content must be zero.
inline void require_zero_tail(const std::string& data, size_t used) {
  for (size_t z = used; z < data.size(); z++) if (data[z]) throw std::runtime_error("Invalid server modpack key message");
}
inline KeyRequest parse_key_request(const std::string& data) {
  if (data.size() < 37 || data.compare(0, 4, "CSK4") != 0) throw std::runtime_error("Invalid server modpack key request");
  KeyRequest r;
  std::memcpy(r.client_public.data(), data.data() + 4, 32);
  const size_t count = static_cast<uint8_t>(data[36]);
  if (!count || count > MAX_PACKS || data.size() < 37 + count * 16) throw std::runtime_error("Invalid server modpack key request");
  for (size_t z = 0; z < count; z++) {
    PackId id;
    std::memcpy(id.data(), data.data() + 37 + z * 16, 16);
    r.packs.push_back(id);
  }
  require_zero_tail(data, 37 + count * 16);
  return r;
}
inline Key wrapping_key(const Key& shared, const Key& client_public, const Key& server_public) {
  std::string m = "CSPK1 kek";
  for (const Key* k : {&shared, &client_public, &server_public}) m.append(reinterpret_cast<const char*>(k->data()), 32);
  Key out;
  crypto_blake2b(out.data(), 32, reinterpret_cast<const uint8_t*>(m.data()), m.size());
  crypto_wipe(m.data(), m.size());
  return out;
}
inline std::string reply_signed_message(const Key& client_public, const std::string& body) {
  return "CSPK1 key reply" + std::string(reinterpret_cast<const char*>(client_public.data()), 32) + body;
}
// Server side: keys for the requested packs this server knows (others are left out).
inline std::string make_key_reply(const Identity& identity, const KeyRequest& request, const std::map<PackId, Key>& keys) {
  Key secret = random_array<Key>(), server_public, shared;
  crypto_x25519_public_key(server_public.data(), secret.data());
  crypto_x25519(shared.data(), secret.data(), request.client_public.data());
  Key kek = wrapping_key(shared, request.client_public, server_public);
  crypto_wipe(secret.data(), 32);
  crypto_wipe(shared.data(), 32);
  std::string body = "CSK5" + std::string(reinterpret_cast<const char*>(server_public.data()), 32) +
      std::string(reinterpret_cast<const char*>(identity.public_key.data()), 32);
  std::string entries;
  size_t count = 0;
  for (const auto& id : request.packs) {
    auto it = keys.find(id);
    if (it == keys.end()) continue;
    uint8_t nonce[24], mac[16], wrapped[32];
    random_bytes(nonce, 24);
    crypto_aead_lock(wrapped, mac, kek.data(), nonce, id.data(), id.size(), it->second.data(), 32);
    entries.append(reinterpret_cast<const char*>(id.data()), 16);
    entries.append(reinterpret_cast<const char*>(nonce), 24);
    entries.append(reinterpret_cast<const char*>(mac), 16);
    entries.append(reinterpret_cast<const char*>(wrapped), 32);
    count++;
  }
  crypto_wipe(kek.data(), 32);
  body += static_cast<char>(count);
  body += entries;
  const std::string message = reply_signed_message(request.client_public, body);
  uint8_t signature[64];
  crypto_eddsa_sign(signature, identity.secret.data(), reinterpret_cast<const uint8_t*>(message.data()), message.size());
  return body + std::string(reinterpret_cast<const char*>(signature), 64);
}
struct KeyReply { Key server_identity{}; std::map<PackId, Key> keys; };
// Client side: checks the signature and unwraps the keys. The caller then accepts a key only for a pack whose header
// pins reply.server_identity.
inline KeyReply open_key_reply(const std::string& data, const Key& client_secret, const Key& client_public) {
  if (data.size() < 69 || data.compare(0, 4, "CSK5") != 0) throw std::runtime_error("Invalid server modpack key reply");
  const size_t count = static_cast<uint8_t>(data[68]);
  const size_t body_size = 69 + count * 88;
  if (count > MAX_PACKS || data.size() < body_size + 64) throw std::runtime_error("Invalid server modpack key reply");
  require_zero_tail(data, body_size + 64);
  KeyReply reply;
  Key server_public;
  std::memcpy(server_public.data(), data.data() + 4, 32);
  std::memcpy(reply.server_identity.data(), data.data() + 36, 32);
  const std::string message = reply_signed_message(client_public, data.substr(0, body_size));
  if (crypto_eddsa_check(reinterpret_cast<const uint8_t*>(data.data() + body_size), reply.server_identity.data(),
                         reinterpret_cast<const uint8_t*>(message.data()), message.size()) != 0)
    throw std::runtime_error("The server's modpack keys are not signed correctly");
  Key shared;
  crypto_x25519(shared.data(), client_secret.data(), server_public.data());
  Key kek = wrapping_key(shared, client_public, server_public);
  crypto_wipe(shared.data(), 32);
  for (size_t z = 0; z < count; z++) {
    const auto* e = reinterpret_cast<const uint8_t*>(data.data() + 69 + z * 88);
    PackId id;
    std::memcpy(id.data(), e, 16);
    Key key;
    if (crypto_aead_unlock(key.data(), e + 40, kek.data(), e + 16, id.data(), 16, e + 56, 32) != 0) {
      crypto_wipe(kek.data(), 32);
      throw std::runtime_error("The server's modpack keys are damaged");
    }
    reply.keys[id] = key;
  }
  crypto_wipe(kek.data(), 32);
  return reply;
}

} // namespace caelum::serverpack
