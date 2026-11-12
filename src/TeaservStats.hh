#pragma once

#include <memory>

class ServerState;

// Activity statistics for server admins (docs/TEASERV_ADMIN_GUI.md). The server appends one JSON line per interval to
// system/stats/YYYY-MM.jsonl and answers the `stats` shell command with the current numbers. Everything runs on the
// server's event thread. Nothing here changes what clients see.
namespace teaserv_stats {

// Server mode only (never in replays): registers the `stats` shell command and starts the sampler. The interval comes
// from the optional config key TeaservStatsInterval (seconds; default 60; 0 keeps the shell command but writes no
// files).
void start(std::shared_ptr<ServerState> state);
// Writes a last line after the event loop has ended, so the counts since the previous sample are not lost.
void stop();

// Counters. Each is called from exactly one place in the upstream code (docs/EXTENSIONS-INTEGRATION.md).
void note_login();
void note_game_created();
void note_battle_started();

} // namespace teaserv_stats

namespace teaserv_admin {

// Saves the game data of every connected Blue Burst player. Call once, in server mode, after the event loop has
// ended. Upstream saves this data every 60 seconds and in ~Client, but ServerState and GameServer hold each other,
// so no Client is destroyed when the server shuts down; without this call a normal `exit` can lose up to a minute
// of a player's progress. Other versions keep their characters on the client.
void save_connected_players(const std::shared_ptr<ServerState>& state);

} // namespace teaserv_admin
