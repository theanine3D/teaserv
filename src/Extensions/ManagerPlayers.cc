// Player identity, per-player extension data, the Episode 3 Meseta ledger,
// wall-clock schedules and the Episode 3 battle-result observation. Everything
// here belongs to the Teaserv extension Manager; upstream code calls only
// Manager::ep3_battle_ended (ReceiveCommands.cc, on_CA_Ep3).

#include "Manager.hh"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <phosg/JSON.hh>
#include <phosg/Time.hh>
#include <random>

#include "../Client.hh"
#include "../Episode3/Server.hh"
#include "../Episode3/monocypher/monocypher.h"
#include "../GameServer.hh"
#include "../Lobby.hh"
#include "../Loggers.hh"
#include "../SendCommands.hh"
#include "../ServerState.hh"

#ifdef _WIN32
#include <windows.h>
#endif

namespace teaserv::extensions {
namespace {

constexpr size_t MAX_PLAYER_FILE_BYTES = 96 * 1024 * 1024;
constexpr size_t MAX_PENDING_PER_PLAYER = 32;
constexpr size_t MAX_KNOWN_PLAYERS = 500000;
const char* const STATE_DIR = "system/extensions-state";

bool valid_package_id(const std::string& id) {
  return !id.empty() && id.size() <= 64 && std::all_of(id.begin(), id.end(), [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
  });
}

// File names contain a dot before "players", which package IDs cannot, so they
// never collide with a package's <id>.json store.
std::filesystem::path host_file() {
  return std::filesystem::path(STATE_DIR) / "host.players.json";
}

std::filesystem::path package_file(const std::string& id) {
  if (!valid_package_id(id)) throw std::runtime_error("invalid extension ID");
  return std::filesystem::path(STATE_DIR) / (id + ".players.json");
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error("cannot read " + path.string());
  std::string result((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  if (file.bad() || result.size() > MAX_PLAYER_FILE_BYTES) throw std::runtime_error("file is unreadable or too large: " + path.string());
  return result;
}

void write_file_atomic(const std::filesystem::path& path, const std::string& data) {
  std::filesystem::create_directories(STATE_DIR);
  auto temporary = path;
  temporary += ".tmp";
  {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file) throw std::runtime_error("cannot create " + temporary.string());
    file << data;
    file.close();
    if (!file) throw std::runtime_error("cannot write " + temporary.string());
  }
#ifdef _WIN32
  if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    throw std::runtime_error("cannot replace " + path.string());
  }
#else
  std::filesystem::rename(temporary, path);
#endif
}

std::string to_hex(const uint8_t* data, size_t size) {
  static const char digits[] = "0123456789abcdef";
  std::string result;
  for (size_t z = 0; z < size; z++) {
    result += digits[data[z] >> 4];
    result += digits[data[z] & 15];
  }
  return result;
}

std::string from_hex(const std::string& text) {
  if (text.size() % 2) throw std::runtime_error("invalid hexadecimal text");
  auto digit = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    throw std::runtime_error("invalid hexadecimal text");
  };
  std::string result;
  for (size_t z = 0; z < text.size(); z += 2) result += static_cast<char>(digit(text[z]) * 16 + digit(text[z + 1]));
  return result;
}

phosg::JSON entry_json(const BoardEntry& entry) {
  return phosg::JSON::dict({{"player_key", entry.player_key}, {"name", entry.name}, {"score", entry.score},
      {"reached", static_cast<int64_t>(entry.reached)}});
}

BoardEntry entry_from_json(const phosg::JSON& json) {
  auto reached = json.get_int("reached", 0);
  return {json.at("player_key").as_string(), json.get_string("name", ""), json.at("score").as_int(),
      static_cast<uint64_t>(reached < 0 ? 0 : reached)};
}

std::string serialize_player_data(const PlayerData& data) {
  auto players = phosg::JSON::dict();
  for (const auto& [key, player] : data.players) {
    auto values = phosg::JSON::dict();
    for (const auto& [name, value] : player.values) values.emplace(name, phosg::JSON(value));
    auto grants = phosg::JSON::list();
    for (const auto& id : player.grant_ids) grants.emplace_back(phosg::JSON(id));
    players.emplace(key, phosg::JSON::dict({{"values", std::move(values)}, {"grants", std::move(grants)}}));
  }
  auto boards = phosg::JSON::dict();
  for (const auto& [name, board] : data.boards) {
    auto entries = phosg::JSON::list();
    for (const auto& [key, entry] : board.entries) {
      (void)key;
      entries.emplace_back(entry_json(entry));
    }
    auto previous = phosg::JSON::list();
    for (const auto& entry : board.previous) previous.emplace_back(entry_json(entry));
    boards.emplace(name, phosg::JSON::dict({{"next", static_cast<int64_t>(board.next_reached)},
        {"entries", std::move(entries)}, {"previous", std::move(previous)}}));
  }
  return phosg::JSON::dict({{"schema_version", 1}, {"players", std::move(players)}, {"boards", std::move(boards)}}).serialize();
}

PlayerData parse_player_data(const std::string& text) {
  auto json = phosg::JSON::parse(text);
  if (json.at("schema_version").as_int() != 1) throw std::runtime_error("unsupported player data schema");
  PlayerData data;
  for (const auto& [key, player_json] : json.at("players").as_dict()) {
    PlayerData::Player player;
    for (const auto& [name, value] : player_json->at("values").as_dict()) player.values.emplace(name, value->as_string());
    for (const auto& id : player_json->at("grants").as_list()) player.grant_ids.emplace_back(id->as_string());
    data.players.emplace(key, std::move(player));
  }
  for (const auto& [name, board_json] : json.at("boards").as_dict()) {
    PlayerData::Board board;
    auto next = board_json->get_int("next", 1);
    board.next_reached = next < 1 ? 1 : static_cast<uint64_t>(next);
    for (const auto& entry_json_item : board_json->at("entries").as_list()) {
      auto entry = entry_from_json(*entry_json_item);
      auto key = entry.player_key;
      board.entries.emplace(std::move(key), std::move(entry));
    }
    for (const auto& entry_json_item : board_json->at("previous").as_list()) {
      board.previous.emplace_back(entry_from_json(*entry_json_item));
    }
    data.boards.emplace(name, std::move(board));
  }
  data.validate();
  return data;
}

int32_t local_offset_seconds(int64_t utc) {
  std::time_t t = static_cast<std::time_t>(utc);
  std::tm local{};
#ifdef _WIN32
  if (localtime_s(&local, &t)) return 0;
#else
  if (!localtime_r(&t, &local)) return 0;
#endif
  int64_t as_utc = civil_to_seconds(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);
  int64_t offset = as_utc - utc;
  return (offset < -86400 || offset > 86400) ? 0 : static_cast<int32_t>(offset);
}

} // namespace

void Manager::read_player_settings() {
  auto s = this->server.lock();
  if (!s) return;
  auto config = s->data->config_json->get("Extensions", phosg::JSON::dict());
  auto zone = config.get_string("TimeZone", "local");
  if (zone != "local" && zone != "UTC") throw std::runtime_error("Extensions.TimeZone must be \"local\" or \"UTC\"");
  auto economy = config.get("Economy", phosg::JSON::dict());
  auto max_grant = economy.get_int("MaxGrant", 100000);
  auto daily_limit = economy.get_int("DailyLimitPerPackage", 10000000);
  auto balance_cap = economy.get_int("BalanceCap", 9999999);
  if (max_grant < 1 || max_grant > 1000000000 || daily_limit < 1 || daily_limit > 1000000000000LL ||
      balance_cap < 1 || balance_cap > 0xFFFFFFFFLL) {
    throw std::runtime_error("Extensions.Economy limits are out of range");
  }
  this->clock_utc = zone == "UTC";
  this->economy_max_grant = max_grant;
  this->economy_daily_limit = daily_limit;
  this->economy_balance_cap = balance_cap;
}

ClockTime Manager::now() const {
  int64_t utc = static_cast<int64_t>(std::time(nullptr));
  return clock_time(utc, this->clock_utc ? 0 : local_offset_seconds(utc));
}

void Manager::load_players() {
  if (this->players_loaded) return;
  auto path = host_file();
  if (std::filesystem::exists(path)) {
    auto json = phosg::JSON::parse(read_file(path));
    if (json.at("schema_version").as_int() != 1) throw std::runtime_error("unsupported host player file schema");
    auto secret = from_hex(json.at("secret").as_string());
    if (secret.size() != 32) throw std::runtime_error("invalid player key secret");
    std::map<std::string, KnownPlayer> known;
    std::map<uint32_t, std::string> by_account;
    for (const auto& [key, player] : json.at("players").as_dict()) {
      auto account_id = player->at("account_id").as_int();
      if (!valid_player_key(key) || account_id < 0 || account_id > 0xFFFFFFFFLL) throw std::runtime_error("invalid known player entry");
      known.emplace(key, KnownPlayer{static_cast<uint32_t>(account_id), player->get_string("name", "")});
      by_account.emplace(static_cast<uint32_t>(account_id), key);
    }
    std::map<std::string, std::vector<PendingGrant>> pending;
    for (const auto& [key, list] : json.at("pending").as_dict()) {
      if (!known.count(key)) throw std::runtime_error("queued grant for an unknown player");
      auto& grants = pending[key];
      for (const auto& grant : list->as_list()) {
        auto amount = grant->at("amount").as_int();
        if (amount < 1 || amount > 1000000000) throw std::runtime_error("invalid queued grant amount");
        grants.emplace_back(PendingGrant{grant->at("package").as_string(), amount, grant->get_string("reason", "")});
      }
    }
    std::map<std::string, DailyTotal> daily;
    for (const auto& [owner, total] : json.get("daily", phosg::JSON::dict()).as_dict()) {
      daily.emplace(owner, DailyTotal{total->at("day").as_int(), total->at("total").as_int()});
    }
    this->player_key_secret = std::move(secret);
    this->known_players = std::move(known);
    this->key_for_account = std::move(by_account);
    this->pending_grants = std::move(pending);
    this->daily_totals = std::move(daily);
  } else {
    std::random_device device;
    std::string secret;
    for (size_t z = 0; z < 32; z++) secret += static_cast<char>(device() & 0xFF);
    this->player_key_secret = std::move(secret);
    this->players_dirty = true;
  }
  this->players_loaded = true;
}

void Manager::flush_players() {
  std::string failures;
  if (this->players_loaded && this->players_dirty) {
    try {
      auto players = phosg::JSON::dict();
      for (const auto& [key, player] : this->known_players) {
        players.emplace(key, phosg::JSON::dict({{"account_id", static_cast<int64_t>(player.account_id)}, {"name", player.name}}));
      }
      auto pending = phosg::JSON::dict();
      for (const auto& [key, grants] : this->pending_grants) {
        auto list = phosg::JSON::list();
        for (const auto& grant : grants) {
          list.emplace_back(phosg::JSON::dict({{"package", grant.owner}, {"amount", grant.amount}, {"reason", grant.reason}}));
        }
        pending.emplace(key, std::move(list));
      }
      auto daily = phosg::JSON::dict();
      for (const auto& [owner, total] : this->daily_totals) {
        daily.emplace(owner, phosg::JSON::dict({{"day", total.day}, {"total", total.total}}));
      }
      auto json = phosg::JSON::dict({{"schema_version", 1},
          {"secret", to_hex(reinterpret_cast<const uint8_t*>(this->player_key_secret.data()), this->player_key_secret.size())},
          {"players", std::move(players)}, {"pending", std::move(pending)}, {"daily", std::move(daily)}});
      write_file_atomic(host_file(), json.serialize());
      this->players_dirty = false;
    } catch (const std::exception& e) {
      failures = std::string("host: ") + e.what();
    }
  }
  for (auto& [owner, data] : this->package_players) {
    if (!data.dirty) continue;
    try {
      write_file_atomic(package_file(owner), serialize_player_data(data));
      data.dirty = false;
    } catch (const std::exception& e) {
      if (!failures.empty()) failures += "; ";
      failures += owner + ": " + e.what();
    }
  }
  if (!failures.empty()) throw std::runtime_error(failures);
}

PlayerData* Manager::player_data(const std::string& owner) {
  auto it = this->package_players.find(owner);
  if (it != this->package_players.end()) return &it->second;
  if (!valid_package_id(owner)) return nullptr;
  // Host-owned and independent of package staging: a reload keeps this data, and
  // a dry check or rejected candidate cannot change it (staging has no context).
  auto path = package_file(owner);
  PlayerData data;
  if (std::filesystem::exists(path)) data = parse_player_data(read_file(path));
  return &this->package_players.emplace(owner, std::move(data)).first->second;
}

std::string Manager::player_key_for(std::shared_ptr<Client> client) {
  if (!client || !client->login || !client->login->account || client->login->account->is_temporary) return "";
  uint32_t account_id = client->login->account->account_id;
  auto cached = this->key_for_account.find(account_id);
  if (cached != this->key_for_account.end()) return cached->second;
  try {
    this->load_players();
  } catch (const std::exception& e) {
    this->log("players", e.what());
    return "";
  }
  uint8_t message[12] = {'t', 's', 'v', 'p', 'k', 'e', 'y', '1'};
  for (size_t z = 0; z < 4; z++) message[8 + z] = static_cast<uint8_t>(account_id >> (8 * z));
  uint8_t hash[12];
  crypto_blake2b_keyed(hash, sizeof(hash), reinterpret_cast<const uint8_t*>(this->player_key_secret.data()),
      this->player_key_secret.size(), message, sizeof(message));
  // Not recorded as a known player here: only an Episode 3 login does that.
  return "p" + to_hex(hash, sizeof(hash));
}

void Manager::player_logged_in(std::shared_ptr<Client> client) {
  // Only Episode 3 sessions carry the server-held Meseta balance.
  if (!is_ep3(client->version())) return;
  auto key = this->player_key_for(client);
  if (key.empty()) return;
  auto account = client->login->account;
  auto known = this->known_players.find(key);
  if (known == this->known_players.end()) {
    if (this->known_players.size() >= MAX_KNOWN_PLAYERS) return;
    this->known_players.emplace(key, KnownPlayer{account->account_id, account->last_player_name});
    this->key_for_account[account->account_id] = key;
    this->players_dirty = true;
  } else if (known->second.name != account->last_player_name && !account->last_player_name.empty()) {
    known->second.name = account->last_player_name;
    this->players_dirty = true;
  }
  auto pending = this->pending_grants.find(key);
  if (pending == this->pending_grants.end()) return;
  auto s = this->server.lock();
  if (!s || s->data->ep3_infinite_meseta) return; // Keep the queue until balances are real again.
  auto& notices = this->login_notices[client->id];
  for (const auto& grant : pending->second) {
    int64_t room = this->economy_balance_cap - static_cast<int64_t>(account->ep3_current_meseta);
    int64_t added = std::max<int64_t>(0, std::min(grant.amount, room));
    account->ep3_current_meseta += static_cast<uint32_t>(added);
    account->ep3_total_meseta_earned = static_cast<uint32_t>(
        std::min<int64_t>(0xFFFFFFFFLL, static_cast<int64_t>(account->ep3_total_meseta_earned) + added));
    this->log(grant.owner, std::format("Meseta: delivered queued {} (added {}) to {} at login; balance {}{}",
        grant.amount, added, key, account->ep3_current_meseta, grant.reason.empty() ? "" : "; reason: " + grant.reason));
    if (notices.size() < 4) {
      notices.emplace_back(std::format("You received {} Meseta{}", added, grant.reason.empty() ? "." : ": " + grant.reason));
    }
  }
  account->save();
  this->pending_grants.erase(pending);
  this->players_dirty = true;
  // Upstream sends the login B7 right after this hook, with the new balance.
}

void Manager::deliver_login_notices(std::shared_ptr<Client> client, std::shared_ptr<Lobby> lobby) {
  auto it = this->login_notices.find(client->id);
  if (it == this->login_notices.end() || !lobby || lobby->is_game()) return;
  auto notices = std::move(it->second);
  this->login_notices.erase(it);
  auto context = this->context_for(client, lobby);
  for (const auto& text : notices) this->reply(context, text);
}

MesetaResult Manager::meseta_grant(const std::string& owner, const std::string& player_key, int64_t amount,
    const std::string& grant_id, const std::string& reason) {
  auto s = this->server.lock();
  auto package = this->packages.find(owner);
  if (!s || package == this->packages.end() || !package->second->enabled() ||
      !package->second->grants().count("economy.meseta") || !valid_player_key(player_key)) return {};
  this->load_players();
  auto* data = this->player_data(owner);
  if (!data) return {};
  if (grant_id.empty() || grant_id.size() > PlayerData::MAX_GRANT_ID_BYTES || amount < 1) return {false, "invalid"};
  if (s->data->ep3_infinite_meseta) return {false, "infinite_meseta"};
  auto known = this->known_players.find(player_key);
  if (known == this->known_players.end()) return {false, "not_ep3"};
  if (data->grant_id_used(player_key, grant_id)) return {true, "duplicate"};
  if (amount > this->economy_max_grant) return {false, "limit"};
  const int64_t today = this->now().day_number;
  auto& daily = this->daily_totals[owner];
  if (daily.day != today) daily = DailyTotal{today, 0};
  if (daily.total + amount > this->economy_daily_limit) return {false, "limit"};

  std::shared_ptr<Client> online;
  if (s->game_server) {
    for (const auto& c : s->game_server->all_clients()) {
      if (c && c->login && c->login->account && c->login->account->account_id == known->second.account_id &&
          is_ep3(c->version()) && !c->proxy_session && c->channel && c->channel->connected() &&
          c->server_behavior == ServerBehavior::GAME_SERVER) {
        online = c;
        break;
      }
    }
  }
  std::string clean_reason = reason.substr(0, 100);
  for (auto& ch : clean_reason) {
    if (static_cast<unsigned char>(ch) < 0x20 || ch == 0x7F) ch = ' ';
  }
  if (online) {
    auto account = online->login->account;
    if (account->is_temporary) return {false, "not_ep3"};
    int64_t room = this->economy_balance_cap - static_cast<int64_t>(account->ep3_current_meseta);
    if (room <= 0) return {false, "cap"};
    int64_t added = std::min(amount, room);
    data->remember_grant_id(player_key, grant_id); // Before the money moves: a retry can never pay twice.
    account->ep3_current_meseta += static_cast<uint32_t>(added);
    account->ep3_total_meseta_earned = static_cast<uint32_t>(
        std::min<int64_t>(0xFFFFFFFFLL, static_cast<int64_t>(account->ep3_total_meseta_earned) + added));
    account->save();
    send_ep3_rank_update(online); // The same B7 update upstream sends after a tournament reward.
    daily.total += added;
    this->players_dirty = true;
    this->log(owner, std::format("Meseta: granted {} (added {}) to {}; balance {}; id {}{}", amount, added, player_key,
        account->ep3_current_meseta, grant_id, clean_reason.empty() ? "" : "; reason: " + clean_reason));
    return {true, "granted", static_cast<int64_t>(account->ep3_current_meseta)};
  }
  auto& queue = this->pending_grants[player_key];
  if (queue.size() >= MAX_PENDING_PER_PLAYER) return {false, "limit"};
  data->remember_grant_id(player_key, grant_id);
  queue.emplace_back(PendingGrant{owner, amount, clean_reason});
  daily.total += amount;
  this->players_dirty = true;
  this->log(owner, std::format("Meseta: queued {} for {} until the next login; id {}{}", amount, player_key, grant_id,
      clean_reason.empty() ? "" : "; reason: " + clean_reason));
  return {true, "queued"};
}

std::deque<std::string> Manager::economy_info() const {
  size_t queued = 0;
  for (const auto& [key, grants] : this->pending_grants) {
    (void)key;
    queued += grants.size();
  }
  size_t schedules = 0;
  for (const auto& [id, runtime] : this->packages) {
    (void)id;
    schedules += runtime->schedule_count();
  }
  return {std::format("Players: {} known Episode 3 accounts, {} queued Meseta grants; limits: {} per grant, {} per package per day, balance cap {}",
              this->known_players.size(), queued, this->economy_max_grant, this->economy_daily_limit, this->economy_balance_cap),
      std::format("Schedules: {} registered; clock: {}", schedules, this->clock_utc ? "UTC" : "server local time")};
}

void Manager::run_schedules() {
  const ClockTime time = this->now();
  // Strictly increasing: a clock set back (or the repeated autumn hour) never
  // fires the same rule twice. Minutes skipped by a forward jump are not replayed.
  if (time.minute_number <= this->last_schedule_minute) return;
  bool first = this->last_schedule_minute == 0;
  this->last_schedule_minute = time.minute_number;
  if (first && time.second > 5) return; // Started mid-minute: wait for the next boundary.
  for (const auto& id : this->package_order) {
    auto runtime = this->packages.find(id);
    if (runtime == this->packages.end() || !runtime->second->schedule_count()) continue;
    try {
      runtime->second->run_schedules(time);
      if (!runtime->second->enabled()) this->cleanup_scene_owner(id);
    } catch (const std::exception& e) {
      this->log(id, e.what());
    }
  }
}

void Manager::ep3_battle_ended(std::shared_ptr<Lobby> lobby) {
  try {
    auto s = this->server.lock();
    if (!s || !lobby || !lobby->ep3_server || !lobby->is_game() || lobby->battle_player ||
        lobby->check_flag(Lobby::Flag::IS_SPECTATOR_TEAM) || lobby->check_flag(Lobby::Flag::IS_CLIENT_CUSTOMIZATION)) return;
    auto server = lobby->ep3_server;
    if (server->setup_phase != Episode3::SetupPhase::BATTLE_ENDED) return;
    for (auto it = this->reported_battles.begin(); it != this->reported_battles.end();) {
      auto reported = it->lock();
      if (reported == server) return; // One observation per battle server object.
      it = reported ? std::next(it) : this->reported_battles.erase(it);
    }
    this->reported_battles.emplace_back(server);
    bool observed = false;
    for (const auto& [id, runtime] : this->packages) {
      (void)id;
      observed |= runtime->enabled() && runtime->grants().count("events");
    }
    if (!observed) return;

    int8_t winner = -1;
    try {
      winner = server->get_winner_team_id();
    } catch (const std::exception&) {
      return; // Undecided or inconsistent result (for example an empty team): not a win.
    }
    if (winner != 0 && winner != 1) return;

    std::vector<InfoTable> teams(2);
    std::vector<std::vector<InfoTable>> members(2);
    for (size_t z = 0; z < 4; z++) {
      auto ps = server->player_states[z];
      if (!ps) continue;
      uint8_t team = ps->get_team_id();
      if (team > 1) continue;
      InfoTable player;
      bool is_com = server->presence_entries[z].is_cpu_player;
      auto c = z < lobby->clients.size() ? lobby->clients[z] : nullptr;
      bool human = !is_com && c && c->login;
      player.add("slot", InfoValue{static_cast<int64_t>(z)});
      player.add("is_com", InfoValue{!human});
      if (human) {
        std::string name = c->login->account->last_player_name.substr(0, 64);
        player.add("name", InfoValue{std::move(name)});
        auto key = this->player_key_for(c);
        if (!key.empty()) player.add("player_key", InfoValue{std::move(key)});
        player.add("client_id", InfoValue{std::to_string(c->id)});
      } else {
        player.add("name", InfoValue{std::string(is_com ? "COM" : "")});
        if (!is_com) player.add("disconnected", InfoValue{true});
      }
      members[team].emplace_back(std::move(player));
    }
    for (size_t team = 0; team < 2; team++) {
      teams[team].add("team", InfoValue{static_cast<int64_t>(team)});
      teams[team].add("won", InfoValue{static_cast<int64_t>(team) == winner});
      teams[team].add("players", InfoValue{std::move(members[team])});
    }
    InfoTable info;
    info.add("event", InfoValue{std::string("ep3_battle_ended")});
    info.add("lobby_id", InfoValue{std::to_string(lobby->lobby_id)});
    info.add("name", InfoValue{lobby->name.substr(0, 256)});
    info.add("winner_team", InfoValue{static_cast<int64_t>(winner)});
    info.add("tournament", InfoValue{static_cast<bool>(lobby->tournament_match)});
    info.add("rounds", InfoValue{static_cast<int64_t>(server->get_round_num())});
    uint64_t now_usecs = phosg::now();
    info.add("duration_s", InfoValue{static_cast<int64_t>(
        server->battle_start_usecs && now_usecs > server->battle_start_usecs ? (now_usecs - server->battle_start_usecs) / 1000000 : 0)});
    info.add("rules_modded", InfoValue{static_cast<bool>(lobby->ep3_cards && lobby->ep3_cards != s->data->ep3_card_index)});
    info.add("teams", InfoValue{std::move(teams)});

    for (const auto& id : this->package_order) {
      auto runtime = this->packages.find(id);
      if (runtime == this->packages.end()) continue;
      try {
        runtime->second->emit_info("ep3_battle_ended", info);
        if (!runtime->second->enabled()) this->cleanup_scene_owner(id);
      } catch (const std::exception& e) {
        this->log(id, e.what());
      }
    }
  } catch (const std::exception& e) {
    this->log("battles", e.what());
  }
}

} // namespace teaserv::extensions
