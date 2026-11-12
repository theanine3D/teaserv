#include "TeaservStats.hh"

#include <stdio.h>
#include <time.h>

#include <chrono>
#include <deque>
#include <filesystem>
#include <string>

#include <phosg/Filesystem.hh>
#include <phosg/JSON.hh>
#include <phosg/Strings.hh>
#include <phosg/Time.hh>

#include "AsyncUtils.hh"
#include "Client.hh"
#include "GameServer.hh"
#include "Lobby.hh"
#include "Loggers.hh"
#include "ServerState.hh"
#include "ShellCommands.hh"
#include "Version.hh"

namespace teaserv_stats {
namespace {

struct Counters {
  uint64_t logins = 0;
  uint64_t games_created = 0;
  uint64_t battles_started = 0;
};

struct Snapshot {
  size_t connections = 0; // every open client connection, logged in or not
  size_t players = 0; // logged-in clients
  size_t in_lobbies = 0;
  size_t in_games = 0;
  size_t ep3_players = 0;
  size_t caelum_players = 0;
  size_t games = 0;
  size_t battles = 0;
};

Counters totals;
Counters written;
std::weak_ptr<ServerState> weak_state;
uint64_t start_usecs = 0;
uint64_t interval_secs = 60;
bool started = false;
bool write_failed = false;

Snapshot take_snapshot(const ServerState& s) {
  Snapshot ret;
  if (s.game_server) {
    for (const auto& c : s.game_server->all_clients()) {
      ret.connections++;
      if (!c->login) {
        continue;
      }
      ret.players++;
      if (is_ep3(c->version())) {
        ret.ep3_players++;
      }
      if (c->ep3_extended_media || c->caelum_mod_capable) {
        ret.caelum_players++;
      }
      auto l = c->lobby.lock();
      if (l) {
        if (l->is_game()) {
          ret.in_games++;
        } else {
          ret.in_lobbies++;
        }
      }
    }
  }
  for (const auto& [_, l] : s.id_to_lobby) {
    if (l->is_game() && !l->check_flag(Lobby::Flag::IS_SPECTATOR_TEAM)) {
      ret.games++;
      if (l->check_flag(Lobby::Flag::BATTLE_IN_PROGRESS)) {
        ret.battles++;
      }
    }
  }
  return ret;
}

phosg::JSON snapshot_json(const Snapshot& snap, const char* event) {
  uint64_t now = phosg::now();
  return phosg::JSON::dict({
      {"t", now / 1000000},
      {"ev", event},
      {"up", (now - start_usecs) / 1000000},
      {"conn", snap.connections},
      {"players", snap.players},
      {"lobby", snap.in_lobbies},
      {"game", snap.in_games},
      {"ep3", snap.ep3_players},
      {"caelum", snap.caelum_players},
      {"games", snap.games},
      {"battles", snap.battles},
  });
}

void write_line(const ServerState& s, const char* event) {
  auto json = snapshot_json(take_snapshot(s), event);
  // Counts since the previous line
  json.emplace("logins", totals.logins - written.logins);
  json.emplace("new_games", totals.games_created - written.games_created);
  json.emplace("new_battles", totals.battles_started - written.battles_started);

  try {
    std::filesystem::create_directories("system/stats");
    time_t now_secs = time(nullptr);
    struct tm now_tm;
#ifdef PHOSG_WINDOWS
    gmtime_s(&now_tm, &now_secs);
#else
    gmtime_r(&now_secs, &now_tm);
#endif
    std::string filename = std::format("system/stats/{:04}-{:02}.jsonl", now_tm.tm_year + 1900, now_tm.tm_mon + 1);
    auto f = phosg::fopen_unique(filename, "ab");
    std::string line = json.serialize() + "\n";
    phosg::fwritex(f.get(), line);
    written = totals;
    write_failed = false;
  } catch (const std::exception& e) {
    // Keep the counts for the next attempt; report the problem once, not every interval
    if (!write_failed) {
      config_log.warning_f("Cannot write statistics: {}", e.what());
      write_failed = true;
    }
  }
}

asio::awaitable<void> sampler_task() {
  for (;;) {
    co_await async_sleep(std::chrono::seconds(interval_secs));
    auto s = weak_state.lock();
    if (!s) {
      co_return;
    }
    write_line(*s, "sample");
  }
}

asio::awaitable<std::deque<std::string>> stats_command(ShellCommand::Args& args) {
  const auto& s = *args.s;
  auto json = snapshot_json(take_snapshot(s), "now");
  json.emplace("total_logins", totals.logins);
  json.emplace("total_games", totals.games_created);
  json.emplace("total_battles", totals.battles_started);
  json.emplace("interval", interval_secs);

  auto lobbies_json = phosg::JSON::list();
  auto games_json = phosg::JSON::list();
  for (const auto& [_, l] : s.id_to_lobby) {
    size_t count = l->count_clients();
    if (l->is_game()) {
      games_json.emplace_back(phosg::JSON::dict({
          {"id", l->lobby_id},
          {"name", l->name},
          {"players", count},
          {"ep3", l->episode == Episode::EP3},
          {"battle", l->check_flag(Lobby::Flag::BATTLE_IN_PROGRESS)},
          {"spectators", l->check_flag(Lobby::Flag::IS_SPECTATOR_TEAM)},
      }));
    } else if (count) {
      lobbies_json.emplace_back(phosg::JSON::dict({
          {"id", l->lobby_id},
          {"block", l->block},
          {"name", l->name},
          {"players", count},
      }));
    }
  }
  json.emplace("lobbies", std::move(lobbies_json));
  json.emplace("game_list", std::move(games_json));

  auto clients_json = phosg::JSON::list();
  if (s.game_server) {
    for (const auto& c : s.game_server->all_clients()) {
      if (clients_json.size() >= 200) {
        break;
      }
      std::string name;
      try {
        auto p = c->character_file(false, false);
        if (p) {
          name = p->disp.visual.name.decode(c->language());
        }
      } catch (const std::exception&) {
      }
      auto l = c->lobby.lock();
      clients_json.emplace_back(phosg::JSON::dict({
          {"id", c->id},
          {"name", std::move(name)},
          {"version", phosg::name_for_enum(c->version())},
          {"account", c->login ? c->login->account->account_id : phosg::JSON(nullptr)},
          {"lobby", l ? l->lobby_id : phosg::JSON(nullptr)},
          {"in_game", l ? l->is_game() : false},
          {"caelum", c->ep3_extended_media || c->caelum_mod_capable},
      }));
    }
  }
  json.emplace("clients", std::move(clients_json));

  // One line with a fixed prefix, so a program that drives the shell can pick it out of the output
  co_return std::deque<std::string>{"TEASERV-STATS " + json.serialize()};
}

} // namespace

void start(std::shared_ptr<ServerState> state) {
  if (started) {
    return;
  }
  started = true;
  weak_state = state;
  start_usecs = phosg::now();
  interval_secs = 60;
  try {
    int64_t configured = state->data->config_json->get_int("TeaservStatsInterval", 60);
    interval_secs = (configured < 0) ? 0 : static_cast<uint64_t>(configured);
  } catch (const std::exception& e) {
    config_log.warning_f("TeaservStatsInterval is not valid ({}); using 60 seconds", e.what());
  }

  // Registered here rather than as a global object: the command registry is a global in another file, and the order
  // in which globals in different files are constructed is not defined. The object lives until the process exits.
  new ShellCommand(
      "stats", "stats\n\
    Show the current player, lobby, game and battle counts as one line of JSON\n\
    (used by the Teaserv admin GUI).",
      stats_command);

  if (interval_secs) {
    config_log.info_f("Recording statistics every {} seconds in system/stats", interval_secs);
    write_line(*state, "start");
    asio::co_spawn(*state->io_context, sampler_task(), asio::detached);
  } else {
    config_log.info_f("Statistics recording is off (TeaservStatsInterval is 0)");
  }
}

void stop() {
  if (!started || !interval_secs) {
    return;
  }
  auto s = weak_state.lock();
  if (s) {
    write_line(*s, "stop");
  }
}

void note_login() {
  totals.logins++;
}

void note_game_created() {
  totals.games_created++;
}

void note_battle_started() {
  totals.battles_started++;
}

} // namespace teaserv_stats

namespace teaserv_admin {

void save_connected_players(const std::shared_ptr<ServerState>& state) {
  if (!state || !state->game_server) {
    return;
  }
  size_t saved = 0;
  for (const auto& c : state->game_server->all_clients()) {
    try {
      // The same condition and call as the 60-second timer in Client::reschedule_save_game_data_timer
      if ((c->version() == Version::BB_V4) && c->login && c->character_file(false)) {
        c->save_all();
        saved++;
      }
    } catch (const std::exception& e) {
      config_log.warning_f("Could not save player data of client {} at shutdown: {}", c->id, e.what());
    }
  }
  if (saved) {
    config_log.info_f("Saved the data of {} connected Blue Burst player(s) before exit", saved);
  }
}

} // namespace teaserv_admin
