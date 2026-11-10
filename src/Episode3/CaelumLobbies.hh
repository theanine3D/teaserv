#pragma once
#include <cstdint>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "../Version.hh"

// Extra Episode 3 lobbies for Caelum (the PC port). The client side reads caelum_lobbies.txt from a modpack
// ("<lobby number> <scene stem> <based-on map> <menu name>") and adds lobbies 20 and up to its teleporter. The server
// side is system/caelum-lobbies.txt (or CAELUM_LOBBIES): one "<lobby number> <name>" per line, '#' comments. The
// pack with the scenes belongs in the server's mod list (system/caelum-modpacks.txt), so every client that is let in
// holds it.
//
// Wire contract (Caelum runtime/lobby_ext.h): the lobby number is both the 0x67 lobby number (the client's area-15
// map number) and the lobby's index in the 0x83 lobby list. Stock clients have room for 21 list entries, so the extra
// lobbies are listed, and can be joined, only by clients that finished the Caelum modpack handshake (E3/02 + F2).
namespace caelum::lobbies {

constexpr uint8_t FIRST_NUMBER = 20;   // lobbies 1-20 are the stock lobby numbers 0-19
constexpr uint8_t LAST_NUMBER = 63;    // Caelum keeps list entries up to 63
// Extra lobbies get fixed lobby IDs (base + lobby number) outside the sequential range, so creating them does not
// shift the IDs of the games created later
constexpr uint32_t LOBBY_ID_BASE = 0xCAE00000;

struct ExtraLobby {
  uint8_t number;
  std::string name;
};

// Lobby numbers must run 20, 21, ... without gaps: the client finds a lobby by its list index.
inline std::vector<ExtraLobby> parse(const std::string& text) {
  auto require = [](bool ok, const std::string& error) {
    if (!ok) {
      throw std::runtime_error("caelum-lobbies: " + error);
    }
  };
  require(text.size() <= 16384, "file exceeds 16 KB");
  std::istringstream lines(text);
  std::string line;
  std::vector<ExtraLobby> result;
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    const size_t start = line.find_first_not_of(" \t");
    if (start == std::string::npos || line[start] == '#') {
      continue;
    }
    for (unsigned char ch : line) {
      require(ch >= 0x20 || ch == '\t', "invalid character");
    }
    size_t end = line.find_first_of(" \t", start);
    const std::string number_text = line.substr(start, end == std::string::npos ? std::string::npos : end - start);
    require(!number_text.empty() && number_text.size() <= 2 &&
            number_text.find_first_not_of("0123456789") == std::string::npos,
        "invalid lobby number \"" + number_text + "\"");
    const unsigned number = std::stoul(number_text);
    require(number == FIRST_NUMBER + result.size(),
        "lobby numbers must run from 20 without gaps (expected " + std::to_string(FIRST_NUMBER + result.size()) +
            ", got " + number_text + ")");
    require(number <= LAST_NUMBER, "lobby number above 63");
    std::string name;
    if (end != std::string::npos) {
      const size_t name_start = line.find_first_not_of(" \t", end);
      if (name_start != std::string::npos) {
        name = line.substr(name_start);
        name.erase(name.find_last_not_of(" \t") + 1);
      }
    }
    require(name.size() <= 32, "lobby name longer than 32 characters");
    result.push_back({static_cast<uint8_t>(number), name.empty() ? "LOBBY" + std::to_string(number + 1) : name});
  }
  return result;
}

// Who sees and may join the extra lobbies: native Episode 3 sessions that completed the modpack handshake.
constexpr bool visible_to(Version version, bool caelum_mod_ready) {
  return version == Version::GC_EP3 && caelum_mod_ready;
}

} // namespace caelum::lobbies
