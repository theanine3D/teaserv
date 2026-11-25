#pragma once

#include <asio.hpp>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>

#include "Runtime.hh"
#include "SessionLifecycle.hh"

class Client;
class ServerState;
struct Lobby;
namespace Episode3 {
class Server;
}

namespace teaserv::extensions {

// All entry points run on ServerState's executor. Only this adapter knows about
// Newserv objects; Runtime is independently testable and contains no packet API.
class Manager : public Host, public std::enable_shared_from_this<Manager> {
public:
  static std::shared_ptr<Manager> create(std::shared_ptr<ServerState> server);
  ~Manager();
  void reload();
  void disable(const std::string& id);
  std::deque<std::string> describe() const;
  std::deque<std::string> check();
  std::deque<std::string> info(const std::string& id) const;
  std::deque<std::string> scene_info(const std::string& id) const;
  bool dispatch(std::shared_ptr<Client> client, const std::string& name, const std::string& args);
  void lobby_event(const std::string& event, std::shared_ptr<Client> client, std::shared_ptr<Lobby> lobby);
  void game_closed(std::shared_ptr<Lobby> lobby);
  void quest_assigned(std::shared_ptr<Lobby> lobby);
  void quest_map_loaded(std::shared_ptr<Lobby> lobby);
  void quest_load_acknowledged(std::shared_ptr<Client> client, std::shared_ptr<Lobby> lobby);
  void quest_join_acknowledged(std::shared_ptr<Client> client, std::shared_ptr<Lobby> lobby, bool bb_ready);
  void game_join_ready(std::shared_ptr<Client> client, std::shared_ptr<Lobby> lobby,
      bool was_loading, bool initial_ready);
  void disconnected(uint64_t client_id);
  void client_logged_in(std::shared_ptr<Client> client);
  void proxy_started(uint64_t client_id);
  void send_login_media(std::shared_ptr<Client> client);
  void prepare_scene(std::shared_ptr<Client> client, std::shared_ptr<Lobby> lobby);
  // Episode 3 battle result observation (ManagerPlayers.cc). Call after the battle
  // server handled a command; reports each battle server object at most once.
  void ep3_battle_ended(std::shared_ptr<Lobby> lobby);
  std::deque<std::string> economy_info() const;
  // Caelum extension channel (ManagerCaelum.cc): a client's F2 flag 0x20-0x2F.
  void caelum_ext_message(std::shared_ptr<Client> client, uint8_t flag, const std::string& data);

private:
  explicit Manager(std::shared_ptr<ServerState> server);
  std::weak_ptr<ServerState> server;
  std::shared_ptr<asio::io_context> io_context;
  asio::steady_timer timer;
  std::map<std::string, std::unique_ptr<Runtime>> packages;
  std::map<std::string, std::set<std::string>> dependencies;
  std::vector<std::string> package_order;
  const std::set<std::string>* staging_names = nullptr;
  size_t ticks = 0;
  AnnouncementGate announcement_gate;
  std::string last_reload_error;
  std::string last_check_error;
  SceneService scenes;
  SessionLifecycle sessions;
  GameLifecycle games;
  std::vector<MediaUpdate> baseline_media;
  bool scenes_enabled = false;
  bool experimental_models = false;
  std::set<int64_t> dirty_scene_lobbies;
  std::chrono::steady_clock::time_point last_scene_flush{};

  // Player identity, per-player data, Meseta ledger and schedules (ManagerPlayers.cc).
  struct PendingGrant {
    std::string owner;
    int64_t amount = 0;
    std::string reason;
  };
  struct KnownPlayer {
    uint32_t account_id = 0;
    std::string name;
  };
  struct DailyTotal {
    int64_t day = 0;
    int64_t total = 0;
  };
  bool players_loaded = false;
  bool players_dirty = false;
  std::string player_key_secret; // 32 random bytes
  std::map<std::string, KnownPlayer> known_players; // By player key
  std::map<uint32_t, std::string> key_for_account;
  std::map<std::string, std::vector<PendingGrant>> pending_grants; // By player key
  std::map<std::string, DailyTotal> daily_totals; // By package
  std::map<std::string, PlayerData> package_players; // By package
  std::map<uint64_t, std::vector<std::string>> login_notices; // By session ID, shown at the first lobby
  std::vector<std::weak_ptr<Episode3::Server>> reported_battles;
  int64_t last_schedule_minute = 0;
  bool clock_utc = false;
  int64_t economy_max_grant = 100000;
  int64_t economy_daily_limit = 10000000;
  int64_t economy_balance_cap = 9999999;
  void load_players();
  void flush_players();
  void read_player_settings();
  std::string player_key_for(std::shared_ptr<Client> client);
  void player_logged_in(std::shared_ptr<Client> client);
  void deliver_login_notices(std::shared_ptr<Client> client, std::shared_ptr<Lobby> lobby);
  void run_schedules();
  // Caelum extension channel and lobby objects (ManagerCaelum.cc).
  struct CaelumPeer {
    uint32_t caps = 0;
    uint32_t max_objects = 16; // HELLO "objects=" (CEXT_LEGACY_OBJECTS without it)
    uint32_t events = 0; // Client reports accepted this session (bounded)
  };
  CaelumScenes caelum;
  std::map<uint64_t, CaelumPeer> caelum_peers; // By session ID; only clients that sent HELLO
  void caelum_send_snapshot(std::shared_ptr<Client> client, std::shared_ptr<Lobby> lobby);
  void caelum_detach(uint64_t session); // Removes the objects attached to a player who left the lobby
  void caelum_sync_lobby(uint8_t number);
  void caelum_cleanup_owner(const std::string& owner);
  std::string caelum_info() const;

  Context context_for(std::shared_ptr<Client> client, std::shared_ptr<Lobby> lobby) const;
  std::shared_ptr<Client> live_client(const Context& context) const;
  void schedule_tick();
  void flush_storage();
  void refresh_baseline();
  SceneClient scene_client(std::shared_ptr<Client> client, std::shared_ptr<Lobby> lobby) const;
  void sync_lobby(int64_t lobby_id);
  void sync_all_scenes();
  void expire_scenes();
  void cleanup_scene_owner(const std::string& owner);
  void flush_scene_updates(bool force = false);
  void emit_session_event(const std::string& event, const Context& context);
  void game_opened(std::shared_ptr<Client> client, std::shared_ptr<Lobby> lobby);
  void emit_game_event(const std::string& event, const GameSnapshot& snapshot);
  void quest_event(std::shared_ptr<Lobby> lobby, bool map_loaded);
  void quest_acknowledged(std::shared_ptr<Client> client, std::shared_ptr<Lobby> lobby, QuestAcknowledgementKind kind);
  std::deque<std::string> stage_config(bool activate, const std::string& reason = "reload");
  void reload_with_reason(const std::string& reason);
  void notify_lifecycle(Runtime& runtime, bool loading, const std::string& reason);

  bool authorized(const Context& context) const override;
  void reply(const Context& context, const std::string& text) override;
  void broadcast(const Context& context, const std::string& text) override;
  AnnouncementResult announce(const std::string& owner, const std::string& text) override;
  bool whisper(const Context& context, uint64_t target, const std::string& text) override;
  void log(const std::string& extension_id, const std::string& text) override;
  bool command_reserved(const std::string& name) const override;
  std::optional<LobbyView> lobby_view(const Context& context) const override;
  std::optional<std::vector<CatalogCommand>> command_catalog(const Context& context) const override;
  SceneCapabilities scene_capabilities(const Context& context) const override;
  SceneAssetSupport scene_asset_support(const Context& context, const std::string& owner, const std::string& asset) const override;
  SceneResult scene_spawn(const Context& context, const std::string& owner, const std::string& key,
      const std::string& asset, const std::string& slot, uint32_t lifetime_ms) override;
  SceneResult scene_remove(const Context& context, const std::string& owner, const std::string& key) override;
  SceneResult scene_event(const Context& context, const std::string& owner, int event) override;
  ClockTime now() const override;
  PlayerData* player_data(const std::string& owner) override;
  MesetaResult meseta_grant(const std::string& owner, const std::string& player_key, int64_t amount,
      const std::string& grant_id, const std::string& reason) override;
  CaelumResult caelum_object(const std::string& owner, uint8_t lobby, const std::string& key, const CaelumObjectSpec& spec) override;
  bool caelum_remove(const std::string& owner, uint8_t lobby, const std::string& key) override;
  CaelumResult caelum_music(const std::string& owner, uint8_t lobby, const CaelumMusicSpec* spec) override;
  bool caelum_clear(const std::string& owner, uint8_t lobby) override;
  int caelum_listeners(uint8_t lobby) const override;
  uint8_t caelum_lobby_named(const std::string& name) const override;
  std::optional<std::string> caelum_lobby_name(uint8_t lobby) const override;
  std::optional<Position> player_position(const Context& context) const override;
  std::shared_ptr<Lobby> public_lobby(uint8_t number) const;
};

} // namespace teaserv::extensions
