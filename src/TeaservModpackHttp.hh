#pragma once

#include <memory>
#include <optional>
#include <string>

class ServerState;

// Optional download server for Caelum modpacks (docs/CAELUM_MODPACKS.md, "Built-in download server"). It serves only
// the ZIP files directly inside the local modpack folder (system/caelum-modpacks, or CAELUM_MODPACK_DIR) as
// http://<address>:<port>/modpacks/<file name>, for admins without another web host. Off unless config.json sets
// TeaservModpackHTTPEnabled to true; TeaservModpackHTTPPort chooses the port (default 80). Teaserv Admin writes the
// matching download addresses into system/caelum-modpacks.txt. Runs on the server's event thread; GET and HEAD only,
// one request per connection, files streamed in small blocks. Nothing else in the server depends on it.
namespace teaserv_modpack_http {

// Server mode only (called from teaserv_stats::start, so upstream code has no call site of its own).
void start(std::shared_ptr<ServerState> state);
// Closes the listening socket; called from teaserv_stats::stop after the event loop has ended.
void stop();

// The pack file name a request target names: "/modpacks/<name>" with an optional query string, percent-decoded. Only
// a plain file name ending in .zip (any case) qualifies: no folder separators, drive or stream colons, control
// characters, leading dot, or more than 255 bytes. Anything else gives nullopt. (Exposed for the static tests.)
std::optional<std::string> filename_for_target(const std::string& target);

} // namespace teaserv_modpack_http
