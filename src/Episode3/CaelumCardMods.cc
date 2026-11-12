#include "CaelumCardMods.hh"

#include <phosg/Filesystem.hh>
#include <phosg/Hash.hh>
#include <phosg/Strings.hh>

#include <array>
#include <cstdlib>
#include <map>
#include <mutex>
#include <stdexcept>

#include "../CommonFileFormats.hh"
#include "../Compression.hh"
#include "DataIndexes.hh"

namespace caelum::cardmods {

namespace {

// ---- raw DEFLATE (RFC 1951), after Mark Adler's puff -----------------------------------------------------------
struct Huffman {
  std::array<uint16_t, 16> count{};
  std::array<uint16_t, 288> symbol{};
};

class Inflater {
public:
  Inflater(const std::string& data, size_t max_size) : in(data), max_size(max_size) {}

  std::string run() {
    bool last;
    do {
      last = this->bits(1);
      switch (this->bits(2)) {
        case 0:
          this->stored();
          break;
        case 1:
          this->fixed();
          break;
        case 2:
          this->dynamic();
          break;
        default:
          throw std::runtime_error("invalid deflate block type");
      }
    } while (!last);
    return std::move(this->out);
  }

private:
  const std::string& in;
  size_t max_size;
  size_t pos = 0;
  uint32_t bitbuf = 0;
  int bitcnt = 0;
  std::string out;

  uint32_t bits(int need) {
    uint32_t value = this->bitbuf;
    while (this->bitcnt < need) {
      if (this->pos >= this->in.size()) {
        throw std::runtime_error("truncated deflate data");
      }
      value |= static_cast<uint32_t>(static_cast<uint8_t>(this->in[this->pos++])) << this->bitcnt;
      this->bitcnt += 8;
    }
    this->bitbuf = value >> need;
    this->bitcnt -= need;
    return value & ((1u << need) - 1);
  }

  void put(char ch) {
    if (this->out.size() >= this->max_size) {
      throw std::runtime_error("deflate output too large");
    }
    this->out.push_back(ch);
  }

  void stored() {
    this->bitbuf = 0;
    this->bitcnt = 0;
    if (this->pos + 4 > this->in.size()) {
      throw std::runtime_error("truncated stored block");
    }
    const auto* p = reinterpret_cast<const uint8_t*>(this->in.data() + this->pos);
    const uint32_t len = p[0] | (p[1] << 8), nlen = p[2] | (p[3] << 8);
    if (len != (~nlen & 0xFFFF)) {
      throw std::runtime_error("invalid stored block length");
    }
    this->pos += 4;
    if (this->pos + len > this->in.size()) {
      throw std::runtime_error("truncated stored block");
    }
    for (uint32_t z = 0; z < len; z++) {
      this->put(this->in[this->pos++]);
    }
  }

  static void construct(Huffman& h, const uint8_t* length, size_t n) {
    h.count.fill(0);
    for (size_t z = 0; z < n; z++) {
      h.count[length[z]]++;
    }
    if (h.count[0] == n) {
      return; // no codes: decode fails if it is used
    }
    int left = 1;
    for (size_t len = 1; len < 16; len++) {
      left <<= 1;
      left -= h.count[len];
      if (left < 0) {
        throw std::runtime_error("over-subscribed deflate code");
      }
    }
    std::array<uint16_t, 16> offs{};
    for (size_t len = 1; len < 15; len++) {
      offs[len + 1] = offs[len] + h.count[len];
    }
    for (size_t z = 0; z < n; z++) {
      if (length[z]) {
        h.symbol[offs[length[z]]++] = z;
      }
    }
  }

  int decode(const Huffman& h) {
    int code = 0, first = 0, index = 0;
    for (size_t len = 1; len < 16; len++) {
      code |= this->bits(1);
      const int count = h.count[len];
      if (code - count < first) {
        return h.symbol[index + (code - first)];
      }
      index += count;
      first += count;
      first <<= 1;
      code <<= 1;
    }
    throw std::runtime_error("invalid deflate code");
  }

  void codes(const Huffman& lencode, const Huffman& distcode) {
    static const uint16_t lbase[29] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const uint8_t lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const uint16_t dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
        1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const uint8_t dext[30] = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    for (;;) {
      int symbol = this->decode(lencode);
      if (symbol < 256) {
        this->put(static_cast<char>(symbol));
      } else if (symbol == 256) {
        return;
      } else {
        symbol -= 257;
        if (symbol >= 29) {
          throw std::runtime_error("invalid deflate length symbol");
        }
        const size_t len = lbase[symbol] + this->bits(lext[symbol]);
        const int dsym = this->decode(distcode);
        if (dsym >= 30) {
          throw std::runtime_error("invalid deflate distance symbol");
        }
        const size_t dist = dbase[dsym] + this->bits(dext[dsym]);
        if (dist > this->out.size()) {
          throw std::runtime_error("deflate distance too far back");
        }
        for (size_t z = 0; z < len; z++) {
          this->put(this->out[this->out.size() - dist]);
        }
      }
    }
  }

  void fixed() {
    static Huffman lencode, distcode;
    static std::once_flag once;
    std::call_once(once, [] {
      uint8_t lengths[288];
      size_t z = 0;
      for (; z < 144; z++) {
        lengths[z] = 8;
      }
      for (; z < 256; z++) {
        lengths[z] = 9;
      }
      for (; z < 280; z++) {
        lengths[z] = 7;
      }
      for (; z < 288; z++) {
        lengths[z] = 8;
      }
      construct(lencode, lengths, 288);
      for (z = 0; z < 30; z++) {
        lengths[z] = 5;
      }
      construct(distcode, lengths, 30);
    });
    this->codes(lencode, distcode);
  }

  void dynamic() {
    static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    const size_t nlen = this->bits(5) + 257, ndist = this->bits(5) + 1, ncode = this->bits(4) + 4;
    if (nlen > 286 || ndist > 30) {
      throw std::runtime_error("invalid deflate code counts");
    }
    uint8_t lengths[320] = {};
    for (size_t z = 0; z < ncode; z++) {
      lengths[order[z]] = this->bits(3);
    }
    Huffman lencode, distcode;
    construct(lencode, lengths, 19);
    size_t index = 0;
    while (index < nlen + ndist) {
      int symbol = this->decode(lencode);
      if (symbol < 16) {
        lengths[index++] = symbol;
        continue;
      }
      uint8_t len = 0;
      size_t repeat;
      if (symbol == 16) {
        if (index == 0) {
          throw std::runtime_error("deflate repeat with no first length");
        }
        len = lengths[index - 1];
        repeat = 3 + this->bits(2);
      } else if (symbol == 17) {
        repeat = 3 + this->bits(3);
      } else {
        repeat = 11 + this->bits(7);
      }
      if (index + repeat > nlen + ndist) {
        throw std::runtime_error("too many deflate lengths");
      }
      while (repeat--) {
        lengths[index++] = len;
      }
    }
    if (lengths[256] == 0) {
      throw std::runtime_error("deflate code has no end-of-block");
    }
    construct(lencode, lengths, nlen);
    construct(distcode, lengths + nlen, ndist);
    this->codes(lencode, distcode);
  }
};

uint32_t u16l(const std::string& s, size_t offset) {
  if (offset + 2 > s.size()) {
    throw std::runtime_error("truncated ZIP");
  }
  return static_cast<uint8_t>(s[offset]) | (static_cast<uint8_t>(s[offset + 1]) << 8);
}
uint32_t u32l(const std::string& s, size_t offset) {
  return u16l(s, offset) | (u16l(s, offset + 2) << 16);
}
uint32_t u32b(const std::string& s, size_t offset) {
  if (offset + 4 > s.size()) {
    throw std::runtime_error("truncated card table");
  }
  return (static_cast<uint8_t>(s[offset]) << 24) | (static_cast<uint8_t>(s[offset + 1]) << 16) |
      (static_cast<uint8_t>(s[offset + 2]) << 8) | static_cast<uint8_t>(s[offset + 3]);
}

std::string lower(std::string s) {
  for (char& ch : s) {
    if (ch >= 'A' && ch <= 'Z') {
      ch += 32;
    }
  }
  return s;
}

constexpr size_t MAX_ENTRY_SIZE = 64 * 1024 * 1024; // the card table is ~200 KB; grown tables stay far below this

} // namespace

std::string inflate_raw(const std::string& data, size_t max_size) {
  return Inflater(data, max_size).run();
}

namespace {
// Every entry whose name passes wanted, as {name, bytes}.
template <typename Wanted>
std::vector<std::pair<std::string, std::string>> zip_find(const std::string& zip, Wanted wanted);
}

std::vector<std::string> zip_entries_named(const std::string& zip, const std::string& basename_lower) {
  std::vector<std::string> result;
  for (auto& [name, data] : zip_find(zip, [&](const std::string& name) {
         const size_t slash = name.find_last_of("/\\");
         return lower(slash == std::string::npos ? name : name.substr(slash + 1)) == basename_lower;
       })) {
    result.emplace_back(std::move(data));
  }
  return result;
}

std::string zip_entry(const std::string& zip, const std::string& path) {
  auto found = zip_find(zip, [&](const std::string& name) { return name == path; });
  if (found.size() != 1) {
    throw std::runtime_error("ZIP entry missing: " + path);
  }
  return std::move(found[0].second);
}

namespace {
template <typename Wanted>
std::vector<std::pair<std::string, std::string>> zip_find(const std::string& zip, Wanted wanted) {
  // End of central directory: the last "PK\5\6" within the final 64 KB + 22 bytes
  if (zip.size() < 22) {
    throw std::runtime_error("not a ZIP file");
  }
  size_t eocd = std::string::npos;
  const size_t stop = zip.size() > 0x10000 + 22 ? zip.size() - 0x10000 - 22 : 0;
  for (size_t z = zip.size() - 22;; z--) {
    if (u32l(zip, z) == 0x06054B50) {
      eocd = z;
      break;
    }
    if (z == stop) {
      break;
    }
  }
  if (eocd == std::string::npos) {
    throw std::runtime_error("not a ZIP file");
  }
  const size_t count = u16l(zip, eocd + 10);
  size_t offset = u32l(zip, eocd + 16);
  std::vector<std::pair<std::string, std::string>> result;
  for (size_t z = 0; z < count; z++) {
    if (u32l(zip, offset) != 0x02014B50) {
      throw std::runtime_error("invalid ZIP central directory");
    }
    const uint32_t method = u16l(zip, offset + 10), compressed = u32l(zip, offset + 20), size = u32l(zip, offset + 24);
    const size_t name_len = u16l(zip, offset + 28), extra_len = u16l(zip, offset + 30), comment_len = u16l(zip, offset + 32);
    const size_t local = u32l(zip, offset + 42);
    if (offset + 46 + name_len > zip.size()) {
      throw std::runtime_error("truncated ZIP central directory");
    }
    std::string name = zip.substr(offset + 46, name_len);
    offset += 46 + name_len + extra_len + comment_len;
    if (!wanted(name)) {
      continue;
    }
    if (compressed == 0xFFFFFFFF || size == 0xFFFFFFFF || size > MAX_ENTRY_SIZE) {
      throw std::runtime_error("ZIP entry too large: " + name);
    }
    if (u32l(zip, local) != 0x04034B50) {
      throw std::runtime_error("invalid ZIP local header: " + name);
    }
    const size_t data_offset = local + 30 + u16l(zip, local + 26) + u16l(zip, local + 28);
    if (data_offset + compressed > zip.size()) {
      throw std::runtime_error("truncated ZIP entry: " + name);
    }
    std::string data = zip.substr(data_offset, compressed);
    if (method == 8) {
      data = inflate_raw(data, size);
    } else if (method != 0) {
      throw std::runtime_error("unsupported ZIP compression method in " + name);
    }
    if (data.size() != size) {
      throw std::runtime_error("ZIP entry size mismatch: " + name);
    }
    result.emplace_back(std::move(name), std::move(data));
  }
  return result;
}
} // namespace

std::vector<uint32_t> card_table_ids(const std::string& rel) {
  if (rel.size() < sizeof(RELFileFooterBE)) {
    throw std::runtime_error("card table too small");
  }
  phosg::StringReader r(rel);
  const uint32_t root = r.pget<RELFileFooterBE>(rel.size() - sizeof(RELFileFooterBE)).root_offset;
  const uint32_t array = u32b(rel, root), count = u32b(rel, root + 4);
  constexpr size_t record = sizeof(Episode3::CardDefinition);
  static_assert(record == 0x128, "card record size");
  if (count > 0x10000 || array > rel.size() || count * record > rel.size() - array) {
    throw std::runtime_error("card table array out of range");
  }
  std::vector<uint32_t> ids;
  for (size_t z = 0; z < count; z++) {
    const size_t at = array + z * record;
    if (static_cast<uint8_t>(rel[at + 0x44]) & 0x80) {
      continue; // end record
    }
    ids.emplace_back(u32b(rel, at));
  }
  return ids;
}

// The card tables of a pack: plain entries, or the sealed files of a server modpack opened with its key.
static std::vector<std::string> card_tables(const std::string& zip, const PackKeys& keys) {
  namespace sp = caelum::serverpack;
  auto header_entries = zip_find(zip, [](const std::string& name) { return name == sp::HEADER_FILE; });
  if (header_entries.empty()) {
    return zip_entries_named(zip, CARD_TABLE_FILE);
  }
  const auto header = sp::parse_header(header_entries[0].second);
  auto key = keys.find(header.id);
  if (key == keys.end()) {
    throw std::runtime_error("server modpack " + sp::hex(header.id) + " has no key in caelum-server-keys.txt");
  }
  auto open = [&](const std::string& label, const std::string& blob) {
    return sp::open(key->second, header.id, label, reinterpret_cast<const uint8_t*>(blob.data()), blob.size());
  };
  std::vector<std::string> tables;
  for (const auto& e : sp::parse_index(open("index", zip_entry(zip, sp::INDEX_FILE)))) {
    const size_t slash = e.path.find_last_of("/\\");
    if (lower(slash == std::string::npos ? e.path : e.path.substr(slash + 1)) != CARD_TABLE_FILE) {
      continue;
    }
    std::string data = open(std::to_string(e.number), zip_entry(zip, sp::entry_name(e.number)));
    if (e.method == 8) {
      data = inflate_raw(data, e.size);
    }
    if (data.size() != e.size || e.size > MAX_ENTRY_SIZE) {
      throw std::runtime_error("server modpack file size mismatch");
    }
    tables.emplace_back(std::move(data));
  }
  return tables;
}

PackCheck check_pack_bytes(const std::string& zip, const std::set<uint32_t>& stock_ids, const PackKeys& keys) {
  PackCheck check;
  try {
    check.result = PackCheck::Result::NO_NEW_CARDS;
    for (const auto& compressed : card_tables(zip, keys)) {
      for (uint32_t id : card_table_ids(prs_decompress(compressed, MAX_ENTRY_SIZE))) {
        if (!stock_ids.count(id)) {
          check.new_card_ids.emplace_back(id);
        }
      }
    }
    if (!check.new_card_ids.empty()) {
      check.result = PackCheck::Result::NEW_CARDS;
    }
  } catch (const std::exception& e) {
    check.result = PackCheck::Result::UNREADABLE;
    check.new_card_ids.clear();
    check.detail = e.what();
  }
  return check;
}

std::filesystem::path pack_dir() {
  const char* env = std::getenv("CAELUM_MODPACK_DIR");
  return (env && *env) ? std::filesystem::path(env) : std::filesystem::path("system/caelum-modpacks");
}

std::vector<PackCheck> check_manifest(
    const std::vector<caelum::mods::Requirement>& packs, const std::filesystem::path& dir,
    const std::set<uint32_t>& stock_ids, const PackKeys& keys) {
  struct FileHash {
    uintmax_t size;
    std::filesystem::file_time_type time;
    std::string sha256;
  };
  static std::mutex mutex;
  static std::map<std::filesystem::path, FileHash> file_hashes;
  static std::map<std::string, PackCheck> results; // by SHA-256; a hash names fixed bytes
  std::lock_guard lock(mutex);

  std::map<std::string, std::filesystem::path> by_hash;
  std::error_code ec;
  if (std::filesystem::is_directory(dir, ec)) {
    for (const auto& item : std::filesystem::directory_iterator(dir, ec)) {
      if (!item.is_regular_file(ec) || lower(item.path().extension().string()) != ".zip") {
        continue;
      }
      const auto size = item.file_size(ec);
      const auto time = item.last_write_time(ec);
      auto it = file_hashes.find(item.path());
      if (it == file_hashes.end() || it->second.size != size || it->second.time != time) {
        try {
          FileHash hash{size, time, lower(phosg::SHA256(phosg::load_file(item.path().string())).hex())};
          it = file_hashes.insert_or_assign(item.path(), std::move(hash)).first;
        } catch (const std::exception&) {
          continue;
        }
      }
      by_hash.emplace(it->second.sha256, item.path());
    }
  }

  std::vector<PackCheck> checks;
  for (const auto& pack : packs) {
    PackCheck check;
    auto cached = results.find(pack.sha256);
    if (cached != results.end()) {
      check = cached->second;
    } else if (auto file = by_hash.find(pack.sha256); file != by_hash.end()) {
      try {
        check = check_pack_bytes(phosg::load_file(file->second.string()), stock_ids, keys);
      } catch (const std::exception& e) {
        check.result = PackCheck::Result::UNREADABLE;
        check.detail = e.what();
      }
      if (check.result != PackCheck::Result::UNREADABLE) {
        results.emplace(pack.sha256, check);
      }
    } else {
      check.result = PackCheck::Result::NOT_FOUND;
      check.detail = "no ZIP with this SHA-256 in " + dir.string();
    }
    check.sha256 = pack.sha256;
    check.name = pack.name;
    checks.emplace_back(std::move(check));
  }
  return checks;
}

} // namespace caelum::cardmods
