#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "Modpacks.hh"
#include "ServerPack.hh"

// Caelum card mods and cross-play (docs/CAELUM_MODPACKS.md, "Card mods and stock clients").
//
// A modpack in the server's mod list "adds cards" when its card table (PsoCardDataTbl_Base.prs, matched by file name
// in any folder of the ZIP, as the client's basename overrides do) holds a card ID that the server's stock card list
// (system/ep3/card-definitions.mnr) does not. While any listed pack adds cards, Caelum clients (which hold the pack)
// and stock clients must not share a game, a battle table, a spectator team, a tournament or a card trade: a stock
// game cannot show or store a card it does not have. Packs that only change files (card art, models, text, stats of
// existing cards) leave cross-play open.
//
// Teaserv reads the packs from a local folder (system/caelum-modpacks/, or CAELUM_MODPACK_DIR), matched by SHA-256.
// A listed pack with no local copy, or one that cannot be read, counts as adding cards: the safe side.
namespace caelum::cardmods {

constexpr const char* CARD_TABLE_FILE = "psocarddatatbl_base.prs"; // lower case; compared case-insensitively

// Raw DEFLATE (RFC 1951) decoder for ZIP entries; throws on bad data or output over max_size.
std::string inflate_raw(const std::string& data, size_t max_size);
// The bytes of every entry of a ZIP whose file name (after the last '/' or '\') equals basename_lower, ignoring case.
// Stored and deflated entries; throws on a malformed archive.
std::vector<std::string> zip_entries_named(const std::string& zip, const std::string& basename_lower);
// The bytes of the entry with exactly this path; throws when it is missing.
std::string zip_entry(const std::string& zip, const std::string& path);
// Card IDs of a decompressed card table REL (footer root -> {array offset, count}, 0x128-byte records). The end
// record (type byte +0x44 negative) is skipped. Throws on a malformed table.
std::vector<uint32_t> card_table_ids(const std::string& rel);

struct PackCheck {
  enum class Result { NO_NEW_CARDS, NEW_CARDS, NOT_FOUND, UNREADABLE };
  std::string sha256, name;
  Result result = Result::NOT_FOUND;
  std::vector<uint32_t> new_card_ids;
  std::string detail; // why NOT_FOUND / UNREADABLE
  bool blocks_cross_play() const { return result != Result::NO_NEW_CARDS; }
};
// Classifies one pack ZIP's bytes against the stock card IDs (empty stock set: any card table counts as new cards).
// A server modpack (sealed, ServerPack.hh) is read with its key from keys (by pack id); without one it is UNREADABLE.
using PackKeys = std::map<caelum::serverpack::PackId, caelum::serverpack::Key>;
PackCheck check_pack_bytes(const std::string& zip, const std::set<uint32_t>& stock_ids, const PackKeys& keys = {});
// Classifies every pack of a parsed mod list, finding each by SHA-256 among the *.zip files of pack_dir. Results for a
// SHA-256 are cached; folder files are hashed again only when their size or time changes.
std::vector<PackCheck> check_manifest(
    const std::vector<caelum::mods::Requirement>& packs, const std::filesystem::path& pack_dir,
    const std::set<uint32_t>& stock_ids, const PackKeys& keys = {});
std::filesystem::path pack_dir();

// Caelum client (finished the modpack handshake) and stock client may share play unless a listed pack adds cards.
constexpr bool separated(bool card_mods_active, bool a_is_caelum, bool b_is_caelum) {
  return card_mods_active && (a_is_caelum != b_is_caelum);
}

} // namespace caelum::cardmods
