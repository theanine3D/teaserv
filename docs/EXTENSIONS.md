# Teaserv server extensions (API 1)

Teaserv adds an optional Lua 5.4 extension host to Newserv. It supports server chat commands, lobby membership, local-session, primary-game and quest observations, package lifecycle callbacks, player-bound timers, and versioned persistent string stores. It uses existing Newserv text-message helpers and introduces no new packet types for extensions. (Separately from the extension host, Teaserv implements the Caelum modpack handshake (`F2`) and extra card lobbies; see [Caelum modpacks](CAELUM_MODPACKS.md).) Existing built-in commands retain priority and their existing behavior. Proxy sessions and log-replay servers do not execute extensions.

The second implementation milestone adds a shared native scene registry, retail Episode 3 lobby media, and controlled built-in lobby events; see [native scene setup and API](EXTENSIONS-SCENES.md). Arbitrary scene transforms, character mutation such as level-up, capability negotiation and new protocol messages for Lua extensions, and client patches remain later milestones. Lobby membership events are not scene-rendering readiness notifications. Extension chat dispatch is disabled for DC NTE and the November 2000 prototype, which do not support the overlay-message helper used here.

## Build and enable

Extensions are disabled at build time by default. A normal upstream-style build does not require Lua. To enable them, install a Lua **5.4** development library, then build with the usual Newserv dependencies:

```sh
cmake -S . -B build -DNEWSERV_ENABLE_EXTENSIONS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

For example, Ubuntu provides `liblua5.4-dev`; macOS can use `brew install lua@5.4` and `-DCMAKE_PREFIX_PATH="$(brew --prefix lua@5.4)"`. Windows requires Lua headers/library matching the compiler and architecture used for Newserv; set `LUA_INCLUDE_DIR` and `LUA_LIBRARY` if CMake cannot discover them. Lua is an external build dependency and is not fetched automatically or bundled in source control. A Lua 5.5 installation is not interchangeable with this API's 5.4 runtime requirement.

Run Newserv from its repository/data directory, as usual. Merge the `Extensions` object from `system/extensions/config.example.json` into `system/config.json`. Do not replace your complete Newserv configuration with that small example. The example opts in to the included `chat-tools` package. Without `Extensions.Enabled: true`, no packages execute.

The compiled executable is `teaserv.exe` on Windows and `teaserv` on other platforms, whether extensions are enabled or disabled. CMake keeps the upstream target name `newserv`; tests and installation resolve its renamed output automatically.

To use `/shout` instead of `$shout`, set Newserv's existing `ChatCommandSentinel` option to `/`. The prefix is global, including built-in commands. Teaserv preserves upstream prefix escaping and the November 2000 prototype's default `@` prefix.

## Packages and grants

Each package lives at `system/extensions/<id>/`, with an `extension.json` manifest and an entry script:

```json
{
  "id": "greeting",
  "version": "0.1.0",
  "api_version": 1,
  "entry": "main.lua",
  "permissions": ["commands", "chat.reply"],
  "dependencies": []
}
```

The server must explicitly enable it and grant its requested permissions:

```json
{
  "Extensions": {
    "Enabled": true,
    "Packages": [
      {"id": "greeting", "grants": ["commands", "chat.reply"]}
    ]
  }
}
```

IDs and command names are 1..64 lowercase ASCII letters, digits, hyphens, or underscores. IDs must match the manifest and directory. Entry paths must resolve inside the package directory and end in `.lua`. There are at most 16 enabled packages, 16 KiB per manifest, and 256 KiB per entry script. Initialization accepts Lua source only, not bytecode.

Supported grants are `commands`, `commands.read`, `events`, `timers`, `storage`, `chat.reply`, `chat.broadcast`, `chat.whisper`, `chat.announce`, `scene.media`, `scene.event`, and `lobby.read`. The script receives only permissions both requested in its manifest and granted by the server. Unknown grants and missing requested grants reject activation. Dependencies are package IDs, must appear earlier in the configured `Packages` list, and must load successfully. API compatibility currently requires `api_version: 1` exactly; package version strings are informational, with no semantic-version dependency solver yet.

Initialization registers commands, event handlers, and optionally a storage migration. Initialization may read its validated settings. Ordinary initialization cannot send messages, access storage, log, or schedule timers. A required migration subsequently runs with access to the staged package's string store and read-only settings, as described below. Registration conflicts with built-in commands, other enabled packages, or duplicate commands reject activation. Missing/invalid packages reject the entire candidate configuration; a failed reload preserves the previous active set.

## Per-package settings

Declare settings in the package's `extension.json`; administrators override them in the corresponding `Extensions.Packages` entry in the server configuration. Scripts can read only their own declared settings through `extensions.setting(name)`, including during initialization, migration, and callbacks. No grant or player context is needed. Reads return scalar copies and share the 32-call API budget for initialization, each migration, or each callback. Cache frequently used values in local variables.

For example, the shipped chat-tools manifest declares:

```json
"settings": {
  "shout_cooldown_ms": {"type": "integer", "default": 10000, "min": 0, "max": 3600000}
}
```

The administrator can use this package entry:

```json
{
  "id": "chat-tools",
  "grants": ["commands", "events", "timers", "storage", "chat.reply", "chat.broadcast", "chat.whisper"],
  "Settings": {"shout_cooldown_ms": 5000}
}
```

The script reads the value while registering its command:

```lua
local delay = extensions.setting("shout_cooldown_ms")
extensions.command("shout", {cooldown_ms = delay}, function(ctx, text)
  extensions.broadcast(ctx.player_name .. ": " .. text)
end)
```

Types are strict `boolean`, `integer` (exact signed 64-bit), and `string`. There is no coercion of strings, floats, or numbers to another type; JSON null, arrays, and objects are not setting values. Optional `default` must match the type and bounds even when overridden. `required: true` requires either an override or a default. An omitted optional value without a default returns nil; an undeclared name is a script error. False, zero, and empty strings remain valid explicit values.

Only integer definitions accept `min`/`max`, inclusive, defaulting to the full signed 64-bit range. Only string definitions accept `max_bytes`, 1..1024, default 1024. Empty strings are allowed; embedded NUL is refused. Unknown schema options, inapplicable constraints, invalid defaults, reversed bounds, or undeclared overrides reject staging. Names use the same 1..64 lowercase ASCII letter/digit/hyphen/underscore rules as package IDs. At most 64 settings are declared per package, with a combined 16 KiB allowance for names and resolved values (booleans/integers count as eight bytes each); the existing 16 KiB manifest limit also applies.

After editing server configuration, run `reload config`, then `extensions check`, then `extensions reload` when ready to activate. Configuration reload updates Newserv's config snapshot; existing extension instances keep their prior values until package activation. Check stages the settings and script together without changing active instances or saving state. Invalid settings preserve the previous active set on package reload. Omitting `settings`/`Settings` retains existing package behavior. Changing settings alone does not increment persistent data versions or trigger an equal-version migration.

`extensions info ID` lists setting names, types, and set/unset status; it omits values. Host validation errors also omit values. Scripts can deliberately log or send setting values if their other APIs permit it. Settings are not written into extension storage automatically and scripts have no configuration-write API.

## Script API

```lua
extensions.command("hello", {permission = "everyone", cooldown_ms = 1000},
  function(ctx, args)
    extensions.reply("Hello, " .. ctx.player_name .. ". You said: " .. args)
  end)
```

Names omit the configured prefix. A handler receives `ctx`, the unchanged raw argument string (up to 1024 bytes), and an optional parsed-arguments table. Commands without an argument schema retain their existing raw-string behavior and receive nil as the third argument. `ctx` contains `client_id`, `lobby_id`, `player_name`, `client_version`, `is_game`, and `can_act`. IDs are decimal strings to avoid number precision loss. They refer to session/lobby identities, not account or Guild Card numbers. `client_version` is the server's base version name, for example `GC_EP3`, and does not identify a modified client. `is_game` describes the supplied lobby snapshot; it is false outside a lobby/game. `can_act` describes whether this callback's scope permits player actions; true still requires live authorization and the relevant grants. Modifying this Lua table never changes the host's authorization context.

### Optional argument schemas

Declare `arguments` in command options to have the host validate player input before calling Lua:

```lua
extensions.command("repeat", {cooldown_ms = 1000, arguments = {
  {name = "count", type = "integer", min = 1, max = 3},
  {name = "message", type = "text", max_bytes = 100}
}}, function(ctx, raw, args)
  for i = 1, args.count do extensions.reply(args.message) end
end)
```

Invalid input receives generated usage such as `Usage: repeat <count> <message>`. It does not call the handler, count as a script failure, mutate storage, schedule work, or consume the command cooldown. Normal authorization is checked first. A successfully parsed command still runs under all existing permissions, grants, and budgets. A schema describes syntax; handlers must still validate game state, target availability, and the meaning of their actions.

| Argument type | Parsed Lua value | Validation |
| --- | --- | --- |
| `word` | String | One token; default maximum 256 bytes. |
| `text` | String | Remainder of input after separating whitespace; default maximum 256 bytes. Must be the last field. Internal/trailing spaces and tabs are preserved. |
| `integer` | Exact signed Lua integer | Decimal integer, optional minus sign, default range -2147483648..2147483647. Optional integer `min`/`max` support the signed 64-bit range. Fractions, exponents, plus signs, trailing junk, and overflow are rejected. |
| `client_id` | Decimal string | Nonzero unsigned 64-bit session ID, maximum/default 20 bytes; never converted to a Lua number. Leading zeroes are preserved within the byte limit. The target is resolved and authorized by the actual host action. |

`max_bytes` may restrict a token/remainder to 1..1024 bytes (`client_id` remains at most 20). Limits measure bytes, not Unicode characters; oversized values are rejected, never truncated by the parser. Each field requires a unique `name` of 1..32 lowercase ASCII letters, digits, or underscores, and a supported `type`. Set `optional = true` only on trailing fields; omitted values are absent/nil in the parsed table. There are no implicit defaults. Generated usage, including the command name, must fit 200 bytes; shorten names if registration fails.

Schemas are dense arrays of at most eight definitions. Unknown field options, duplicate names, invalid bounds/types, holes, misplaced optional fields, or a non-final `text` reject package staging and preserve the previous active set on reload. `arguments = {}` explicitly accepts no arguments beyond spaces/tabs; omitting `arguments` leaves validation to the script. `min`/`max` apply only to `integer` fields.

Tokens are separated by ASCII spaces and tabs. There is no quoting, escaping, pattern matching, or option/flag parser: quotes are literal characters. A final `text` consumes all remaining text; other schemas reject extra tokens. Schema commands reject NUL, other ASCII controls, and DEL in input. The original raw string passed as the second handler argument remains unchanged. The shipped `shout` and `whisper` examples demonstrate schema use.

### Permissions and actions

Command permissions are `everyone` (default), `announce`, `debug`, `moderator`, `root`, or `change-event`. These map to Newserv account flags; composite permissions require the complete flag mask. The command never inherits the shell's permission-bypass mode. Extension grants and the invoking player's permission are separate requirements. Muted users and clients without chat-command access cannot invoke messaging actions. Cooldowns are per command/session, default to 1000 ms, and accept 0..3600000 ms. Unloading/reloading clears cooldowns; disabling should be an administrator action.

Command options may also declare `help = "A short description"`. Help is optional, at most 160 bytes, and must be a string without NUL, ASCII controls, or DEL. It appears with permission, cooldown, and generated usage in the administrator's `extensions info ID` output. It does not change dispatch, permissions, or player-visible argument-error replies. The shipped shout/whisper examples include descriptions.

| Function | Behavior |
| --- | --- |
| `extensions.lobby()` | Return a fresh snapshot of the caller's lobby/game and its connected local players, or nil if unavailable. Requires `lobby.read` and a live authorized callback; unavailable in initialization/migration. |
| `extensions.commands()` | Return a fresh sorted array of active extension commands the caller may use, or nil if unavailable. Requires `commands.read`; no arguments. See command discovery below. |
| `extensions.setting(name)` | Read a declared, validated package setting as a boolean, integer, string, or nil. Available during initialization and callbacks without a grant. |
| `extensions.command(name, options, handler)` | Register a command during initialization. Up to 32 commands per package. |
| `extensions.on(event, handler)` | Register a supported event. Player/session/join-readiness handlers receive `(ctx, event_name)`, with nil parsed arguments; package lifecycle, primary-game and quest observation handlers receive one metadata table as described below. Requires `events`; up to 32 handlers total per package. |
| `extensions.reply(text)` | Send plain text to the invoking player, using `chat.reply`. |
| `extensions.broadcast(text)` | Send plain text to connected clients in that player's current lobby/game, using `chat.broadcast`. |
| `extensions.announce(text)` | Attempt a server-wide plain-text announcement; returns `ok, reason`. Requires `chat.announce`; event and player authority rules and rate limits are described below. |
| `extensions.whisper(client_id, text)` | Send plain text to a connected session in the same lobby/game, using `chat.whisper`. Returns false for an invalid/missing target; a departing recipient is a normal condition, not a script fault. |
| `extensions.after(milliseconds, handler)` | Schedule a one-shot callback; returns an integer timer ID, or nil plus an error string when the package's pending-timer quota is full. Requires `timers`; delay is 50..3600000 ms. Handler receives `(ctx, "")`. |
| `extensions.every(milliseconds, handler)` | Schedule a repeating callback with a fixed delay after each successful invocation. Requires `timers`; interval is an integer 50..3600000 ms. Returns a stable timer ID or nil plus a quota error. Handler receives `(ctx, "")`; see repeating timers below. |
| `extensions.cancel(timer_id)` | Cancel a timer owned by this package; returns a boolean. Requires `timers`. |
| `extensions.storage_get(key)` | Read a stored string or return nil; requires `storage`. |
| `extensions.storage_set(key, value)` | Set a stored string, or delete with nil; requires `storage`. |
| `extensions.migrate(handler)` | Register one initialization-time storage migration; requires `storage`. Runs as `(old_version, target_version)` only when upgrading package data. |
| `extensions.log(text)` | Write a bounded, single-line message to the server log under the package ID. |

Player-context messaging and timer APIs use the host's captured context, never script-provided player objects. Before a delayed callback runs, the host rechecks that the original session is connected, remains in the original lobby, and still has the command permission. Lobby transitions and disconnects cancel that session's timers. A reload/unload cancels all timers owned by the replaced package. Timers are not persisted across restarts.

Membership hooks run after successful joins and on departures through `ServerState`'s membership methods, not on every join-notification refresh. On a successful move, `lobby_leave` carries the old lobby snapshot and `lobby_join` the new one. Leave callbacks may use logging/storage or separately granted server announcements: the player may already have moved or disconnected, so messaging/timer actions may be rejected. There is no cancellable join/leave hook. The baseline call-site audit found all external lobby membership mutations routed through these methods; upstream integration must preserve that routing.

### Server-wide announcements

`extensions.announce(text)` uses Newserv's native on-screen announcement presentation, including Blue Burst's scrolling top bar. It broadcasts across eligible local game-server sessions in different games/lobbies, without requiring a current lobby. Authenticated, connected sessions on supported versions are eligible; proxies, patch/unknown versions, DC NTE/November 2000, and BB connections before game-server phase 4 are excluded. Recipient account permissions and chat mute status do not suppress announcements. This is the current server instance, not a federation-wide broadcast.

The package must request `chat.announce` in its manifest and the administrator must explicitly grant it. Existing `chat.broadcast` grants do not confer server-wide authority. Commands and their player-bound timers additionally recheck both the captured context's normal authorization and the invoking account's `ANNOUNCE` flag at the time of the call. Register announcement commands with `permission = "announce"`; an `everyone` command cannot bypass the extra check, and editing Lua context fields cannot change authority.

Registered lobby, local-session, join-readiness, primary-game, quest, and `package_load` callbacks may announce under the granted extension authority, even when `can_act` is false or no player remains connected. This is an explicit exception to the following observation sections' player-messaging restrictions. It does not unlock reply/broadcast/whisper, timers, scene actions, or live queries in those scopes. Timers never inherit event authority. Initialization, storage migrations, dry checks/rejected candidates, `package_unload`, and faulted/disabled packages cannot announce. At initial package load, native server services may not exist yet: the call returns `false, "unavailable"` without queuing a message. A subsequent reload's load callback can announce normally.

Accepted attempts are limited to **one per package every 30 seconds** and **one across all extensions every 5 seconds**, using monotonic time. The manager retains cooldowns through reload, disable/re-enable, and configuration changes; a new server process starts fresh. Rejected attempts do not extend cooldowns. There is no queue, retry, or deferred burst. These limits affect extension announcements only; built-in `$ann` and shell `announce` keep their existing behavior.

| Result | Meaning |
| --- | --- |
| `true, nil` | Accepted for an immediate fanout attempt. No recipients, a recipient send failure, or absent client rendering does not turn this into a delivery acknowledgement. |
| `false, "not_authorized"` | A command/timer caller lacks current authority, including the `ANNOUNCE` flag. |
| `false, "empty_message"` | Input becomes empty or only spaces/newlines after control filtering and delivery truncation. |
| `false, "package_rate_limited"` | This package has an unexpired 30-second cooldown. |
| `false, "server_rate_limited"` | Another accepted extension announcement is within the 5-second global interval. |
| `false, "unavailable"` | Native services or the active granted package are unavailable. |

The call accepts exactly one string, at most 512 bytes, without NUL. Delivery uses the existing extension plain-text rules: ASCII controls removed except newline, formatting metacharacters escaped, and at most 200 UTF-8 bytes without splitting a valid character. Calls count against the existing 32-call and 4096-byte callback budgets, including normal refusals. Wrong arguments, missing grants, or prohibited initialization/migration/unload calls are script errors; ordinary rate/authorization refusals do not fault the package. Individual recipient send failures are contained, with one aggregate log entry; already attempted sends are not rolled back.

For example:

```lua
extensions.on("game_opened", function(info)
  local ok, reason = extensions.announce("A new game is available.")
  if not ok then extensions.log("Announcement skipped: " .. reason) end
end)
```

The optional `server-announcements` package demonstrates this event and adds `$extannounce <message>` for accounts with `ANNOUNCE`. Merge the package entry from `system/extensions/announcements.config.example.json` into your existing `Extensions.Packages`; do not replace the full server configuration. Set its `game_opened_message` setting to an empty string to disable the automatic message. No shipped default configuration enables this package. Run `reload config`, `extensions check`, then `extensions reload`. For a live test, verify the same manual message across two different lobbies/games, denied non-admin commands, rate refusals, and native GC/Episode 3 versus BB presentation. Automated headless acceptance does not establish client display.

### Repeating timers

```lua
local id
local count = 0
id = extensions.every(1000, function(ctx)
  count = count + 1
  extensions.reply("Reminder " .. count)
  if count >= 3 then extensions.cancel(id) end
end)
```

Declare the ID before assigning it so the Lua closure captures that local. `every` uses the same `timers` grant, player context, 50..3600000 ms integer bounds, and shared 32-timer quota as `after`. The first invocation is due one interval after scheduling. On successful completion, the same ID is scheduled one interval after completion; time spent in the callback is not subtracted. Missed intervals are skipped, with no catch-up burst. Scheduling and cancellation use the normal callback API budget.

Due timers are selected by earliest deadline, with timer ID breaking ties. At most one timer is processed per package tick, under the existing executor/fairness budget. This prevents an always-due low-ID repeating timer from continually taking priority over an older waiting one-shot. Actual intervals can be longer due to server load, callbacks, and scheduling; this is not a real-time clock or a background worker.

Repeating timers keep their quota slot while executing. `cancel(id)` can cancel the executing timer and returns true; cancellation is idempotent (a later attempt returns false). A callback may cancel itself and create replacement work without the old execution resurrecting the timer or releasing a reused registry reference. A failed repeating callback stops that timer after recording its normal callback error. Earlier actions are not rolled back, and the existing cumulative three-error package fault threshold still applies. A package fault, unload, or successful replacement cancels all its timers.

The original connected session, lobby, chat access, and command permission are checked before each invocation and before repeating rescheduling. Invalid or revoked contexts remove the timer without running Lua. Existing leave/disconnect/proxy cleanup applies to both timer kinds; a return/reconnect does not resume old work. Initialization, migration, and read-only observation callbacks cannot create repeating timers. Timers and IDs are runtime-local and are not persisted across reload/restart. Callback timing diagnostics count actual invocations, not queued intervals or skipped ticks.

The shipped chat-tools package adds `$repeatremind 3 5`: three reminders, five seconds apart, using the configured prefix. It accepts 1..5 messages and 1..60 seconds, stops itself after the requested count, and is cancelled on departure. No additional grant or client packet is needed beyond the package's existing timers/reply permissions.

### Live membership checks

Player-bound actions and live queries require a current local game-server membership, rechecked on use. The adapter resolves the session ID through the client registry, verifies the client's own ID, and requires authentication, a connected channel, no proxy, a supported game version, and normal BB game-server phase where applicable. The supplied lobby ID must resolve to the exact lobby object currently referenced by the client, and the client's own slot must contain that same client object. Retired/replaced lobbies, removed or inconsistent slots, wrong IDs, patch/login-server roles, unknown/patch versions, DC NTE/November 2000 prototypes, and BB data phases are unavailable. Other trial/prototype builds are not rejected merely for that classification; their existing version-specific restrictions remain. This check is synchronous evidence; no new player handle or authority is returned to Lua.

Replies (including permission-error replies), whisper recipients, broadcast recipients, lobby views, command discovery, scene capability/action adapters, and due Lua timers use this shared eligibility boundary. Broadcasts and lobby views also match each enumerated position to the client's own slot, so a duplicate entry in another slot is skipped. Membership eligibility is separate from the invoking command's account permission and extension grants: the sender must still be authorized, while recipients need not hold the sender's admin flag or have permission to issue chat commands themselves. A current client denied a command permission can still receive its normal error reply. Invalid or departed contexts do not receive replies.

Existing leave/disconnect/proxy hooks still cancel player timers. Revalidation additionally refuses a due timer when membership/role/phase has become invalid; it does not replace those cancellation hooks or establish a new persistent membership generation. Read-only observation scopes retain their existing no-action/no-live-query rules. Chat membership remains distinct from primary-game observation eligibility: ordinary spectator/chat behavior is not reclassified as a primary game. These guards do not change native membership ordering, built-in chat, packet formats, or scene recipient negotiation.

### Command discovery and player help

`extensions.commands()` requires the independent `commands.read` grant. It returns a fresh array sorted by command name across enabled packages, filtered by the current caller's account permissions. Each entry contains `name`, `package_id`, `permission`, `cooldown_ms`, `usage`, and `help` (an empty string when absent). Names and usage omit the configured prefix. This lists extension commands only; built-in command dispatch/help is unchanged. Faulted or unloaded packages are omitted, even though a faulted package's command names remain reserved until replacement/unload.

The query takes no arguments and rechecks the invoking callback's original session/lobby/permission before consulting the adapter. Each candidate command's permission is checked independently before any metadata reaches Lua. Results contain no callback references, source, grants, settings, storage, or player/account identities. Modifying returned tables or `ctx` never changes the registry or permissions. Every query sees the current activated set; dry checks and rejected reloads preserve it, and successful reload/unload changes subsequent results. This is a current discovery snapshot, not an execution handle: cooldowns, grants, live state, and permissions are still checked on invocation. Permission revocation also applies to delayed callbacks.

With the grant, read-only session/game/quest/package observation callbacks and stale/unauthorized player contexts receive nil without querying live command state. A live adapter with no available commands returns an empty array. Initialization and migrations cannot call the query. The normal 32-API-call, instruction/time, memory, and fault limits apply. Native validation bounds the catalog to 16 packages, 32 commands per package, 512 entries total; malformed/duplicate metadata rejects before table construction. Native snapshot ownership is cleared after protected callbacks, including allocation failures.

To enable the optional player help package, merge `system/extensions/command-help.config.example.json` into your complete configuration, preserving other package entries. It grants only `commands`, `commands.read`, and `chat.reply`:

```json
{"id": "command-help", "grants": ["commands", "commands.read", "chat.reply"]}
```

After `reload config` and `extensions reload`, use `$exthelp` or `$exthelp 2` to list six names per page, and `$extusage shout` for a visible command's usage/help. Substitute the configured prefix if different. The two help commands have independent one-second session cooldowns. Unknown and permission-denied names receive the same unavailable response. Out-of-range pages receive a bounded response; integer/schema failures do not execute the handler. Pagination keeps output within existing chat limits even with a full catalog. Listing does not imply that a gameplay action or rendering feature is supported on the caller's client.

### Current lobby and connected players

`extensions.lobby()` takes no parameters. With the `lobby.read` grant, a live authorized command, lobby membership callback, or player-bound timer can read its caller's current lobby/game. The adapter rechecks the callback's original command permission and exact client/lobby membership on every call. There is no server-wide player lookup or query for another lobby.

The result is a fresh Lua table:

| Field | Meaning |
| --- | --- |
| `lobby_id` | Decimal string, matching the callback's lobby ID. |
| `name` | Server's current lobby/game name. |
| `is_game` | True for a game, false for a lobby. |
| `event` | Current built-in lobby event number; reading does not change it. |
| `capacity` | Configured maximum clients, 1..12; not a count of rendering-ready players. |
| `players` | Dense array of at most 12 connected, authenticated local players, ordered by lobby slot. |

Each player has decimal-string `client_id`, integer `slot` (0..11), `player_name`, base `client_version` (such as `GC_EP3`), and boolean `is_self`/`is_leader`. The array index is not the slot. Session IDs preserve the full unsigned 64-bit range and can be passed directly to `extensions.whisper`; they are not Guild Card/account identifiers. Names use the same last-known authenticated player name as callback contexts and can be empty or out of date. Versions do not identify custom clients. Lobby/player names are at most 256 bytes; version names are at most 32. The host rejects malformed/out-of-scope snapshots before returning them to Lua.

Proxy, disconnected, unauthenticated, pending, and stale membership entries are excluded. No password, IP address, account ID, permission flags, character files, or other lobby's players are exposed. A player present in the snapshot may still be loading; the query does not establish scene or game readiness. The API sends no client packets.

Every call consumes one of the existing 32 callback API operations and returns copies. Editing or retaining the returned tables cannot change server state or grant authority. Retained snapshots do not update as players move; call again for fresh data. Subsequent actions independently revalidate their target and context. A delayed callback cannot use a snapshot to act on a departed/reconnected player or another lobby.

A missing/unauthorized live context returns nil without a script fault, including stale lobby-leave callbacks and read-only `client_login`/`client_disconnect`/`game_join_ready` observations. These read-only observations never query the adapter, even if Lua changes `ctx.can_act`. Initialization and migration attempts, or calls without the grant, are script errors. A grant authorizes reading within a valid scope; it does not bypass command permissions, mute restrictions, or proxy/replay exclusions.

The optional `lobby-tools` package implements `$who` (or `/who` with that configured prefix), listing connected session IDs for chat-tools' whisper command. Append this entry to your existing `Extensions.Packages`, then run the normal `reload config`, `extensions check`, `extensions reload` sequence:

```json
{"id": "lobby-tools", "grants": ["commands", "chat.reply", "lobby.read"]}
```

The package uses a five-second per-session cooldown, accepts no arguments, and sends its list only to the caller through existing plain-text helpers. It is independent of chat-tools and is not enabled automatically. Existing chat-tools grants and configuration remain sufficient. Its replies put each complete session ID before the player name so legacy text truncation does not cut the identifier.

### Local session observations

`client_login` and `client_disconnect` are metadata observations of an authenticated **local game-server session**, not authentication hooks, physical-connection counters, account logins, or rendering-ready events:

| Event | Timing and snapshot |
| --- | --- |
| `client_login` | First entry into the existing local game-server login procedure after authentication/version selection. Sent before its menu/initial media/join work. Lobby ID is `"0"` and `is_game` is false. The player name can be empty or a previously stored name; character data need not be loaded yet. Repeated procedure/menu setup is deduplicated while the local session remains tracked. |
| `client_disconnect` | At the beginning of disconnect cleanup, after timers and native recipient cache are cleared, before the usual lobby removal and `lobby_leave` notification. Carries the last observed lobby/game and name snapshot, which may no longer be current membership. Sent at most once for an admitted local session. A disconnect before admission produces no event. |

Both events always have `ctx.can_act == false`. Storage reads/writes require the `storage` grant; bounded `extensions.log` is available as in other callbacks. Player-context messaging, timer creation/cancellation, and scene mutation are refused even if the underlying connection is still usable or Lua changes `ctx.can_act`. Read-only scene capability queries return unavailable flags and zero byte budgets without faulting. Misusing an action API is a script error and follows the normal fault limit. Observation callbacks retain the existing instruction, time, memory, API, and output budgets.

Patch/data-server authentication, active proxy sessions, and replay servers do not produce these events. BB enters only in its game-server phase. A successful proxy handoff silently forgets local admission and cancels pending timers/cache; it does not emit a local disconnect after GameServer later clears the proxy pointer. Returning to the local server can admit another local session with the same client ID. Ship/block reconnects use their normal new client IDs. Failed proxy connections preserve local tracking.

Sessions are tracked independently of package reloads. Enabling or reloading a package does not replay historical logins; a newly enabled observer can receive a disconnect without having seen its login. Disabling/faulting a package, proxy handoffs, abrupt shutdown, and server replacement can also leave unmatched observations. The events are not a durable, exactly paired audit ledger or an online-player counting API. These session observations provide no veto, game/quest-start/end notification, or new client protocol. Package load/unload notifications are separate and described below.

The optional `system/extensions/session-audit` package demonstrates persistent observation counts and logs without sending client messages. To enable it, append this entry to `Extensions.Packages`, then use the normal configuration/package reload sequence:

```json
{"id": "session-audit", "grants": ["events", "storage"]}
```

It is not enabled by the shipped configuration example. Membership callbacks still describe both lobby and game membership; inspect `ctx.is_game` to distinguish them. Neither membership nor session observations prove that a scene is rendered or that a quest has begun.

### Individual game-join readiness

`game_join_ready` observes one authenticated local player's initial game-loading acknowledgement. Register it with the `events` grant; the callback uses the existing copied player context and event name, with nil parsed arguments:

```lua
extensions.on("game_join_ready", function(ctx, event)
  extensions.log("Client " .. ctx.client_id .. " acknowledged joining game " .. ctx.lobby_id)
end)
```

`ctx` contains decimal-string `client_id` and `lobby_id`, `player_name`, `client_version`, `is_game = true`, and `can_act = false`. It contains no account credentials, quest data, live objects, or whole-game readiness state. Each invocation gets a fresh table; altering it cannot affect the server or authorize actions.

The exact native boundary is the successful end of `on_6F`, after its normal item-ID/visibility/metadata/time handling and next-join scheduling. The incoming command must be the normal `0x006F`, and this client must have had the native `LOADING` flag set before that handler cleared it. The manager rechecks the current Client/Lobby registry objects, actual slot membership, connection/authentication/local game-server phase, and a previously tracked membership in an admitted primary game. Spectator, playback, client-customization, proxy, replay, and stale memberships are excluded. BB must be in its normal game-server phase.

A membership produces at most one observation. Repeated acknowledgements and duplicate join notifications do not rearm it. Leaving and rejoining starts a fresh membership; disconnect and successful proxy handoff discard tracking, and game removal retires all members. Package check/reload/disable does not reset tracking or replay past acknowledgements. Enabling an observer while a member is still loading can observe the later acknowledgement. Tracking is consumed before callbacks, so a failed observer cannot cause a retry or prevent healthy observers from receiving the same observation.

The callback may log, read validated settings, and read/write granted namespaced storage. Player-context messaging, timers, scene mutations, and late registration are refused even if Lua changes `ctx.can_act`. With their usual grants, lobby queries return nil and scene-capability queries report unavailable/zero budgets without querying live state. Normal callback quotas and fault isolation apply; ordinary commands and membership callbacks retain their existing authority.

This is an individual initial-join acknowledgement, not all players loaded, visible rendering, quest readiness/start/completion, or an Episode 3 battle transition. BB's `0x016F` quest-ready alias is excluded. An Episode 3 post-battle `6F` without initial loading does not produce this event. A joinable quest can still be downloading or loading after the initial join acknowledgement, including on BB where this handler starts that transfer. No additional packets or acknowledgement probes are sent. Native exceptions before the final boundary emit nothing; clearing a native flag alone does not promise an observation or a retry.

The optional `game-audit` package now also keeps a `joins-ready` count alongside game openings/removals. Real authenticated-client timing, reconnect/rejoin, BB joinable quests, and Episode 3 post-battle checks remain acceptance tests; synthetic tracker/runtime tests do not establish client rendering readiness.

### Primary game observations

Register `game_opened` and `game_closed` using the existing `events` grant. Each handler receives **one copied metadata table**, with no player context:

```lua
extensions.on("game_opened", function(info)
  extensions.log("Primary game " .. info.lobby_id .. " opened: " .. info.name)
end)

extensions.on("game_closed", function(info)
  extensions.log("Primary game " .. info.lobby_id .. " removed: " .. info.name)
end)
```

| Event | Meaning |
| --- | --- |
| `game_opened` | First eligible authenticated local player successfully joins a primary game. Sent after the existing membership callbacks, once for that server registry lifetime. `reason` is `first_join`. |
| `game_closed` | A previously observed game is explicitly removed from ServerState's lobby registry. The registry entry is already gone; its object still exists while the snapshot is copied. `reason` is `removed`. |

A primary game is a game lobby outside Episode 3 spectator/watcher teams, battle playback, and temporary client-customization/patch-enabler games. Proxy, patch/data-server, disconnected, unauthenticated, and stale membership paths are excluded. BB must be in a game-server phase. Admission checks both the current server registry entries and the player's actual lobby slot/pointer. Ordinary public lobbies produce no game event. A creation attempt that never reaches an eligible join produces neither event.

Metadata fields are:

| Field | Value |
| --- | --- |
| `event`, `reason` | Event name and the reason above. |
| `lobby_id` | Decimal string, matching the game's server lobby ID. IDs are local to the server lifetime, not persistent global identities. |
| `name` | Current game name, at most 256 bytes, without embedded NUL. |
| `episode` | Integer 1..4: Episodes I, II, III, IV. |
| `mode` | Integer 0 normal, 1 battle, 2 challenge, 3 solo. |
| `difficulty` | Integer 0 normal, 1 hard, 2 very hard, 3 ultimate. These are server metadata, not assertions about every client's gameplay mode. |
| `quest_number` | Currently assigned server quest number, or nil if none. Zero is a valid number. Assignment does not imply quest loading/completion. |
| `quest_joinable` | Assigned quest's joinability flag as a boolean; nil when there is no quest. False remains a valid explicit value. |

The close snapshot reads current metadata, including a renamed game or a subsequently assigned quest; it is not a retained copy of the opening snapshot. No account IDs, client IDs, passwords, connection addresses, player list, map objects, or live handles are exposed. Mutating/retaining the Lua table cannot change game state.

Both events permit bounded logging, read-only settings, and granted namespaced storage reads/writes, even after the last player has left. They carry no player-action authority: player-context messaging, timers, scene mutation, and late registration are refused. With their usual grants, lobby queries return nil and scene capability queries return unavailable/zero budgets without consulting live state. Forging fields such as `info.can_act` does not change this scope. The existing 32-handler registration quota, per-callback resource limits, aggregate 10 ms event time check, diagnostics, and three-error fault threshold apply. A bad observer cannot veto a join or removal; the host still owns resource cleanup.

Game tracking belongs to the manager and survives package reload/check/disable. Additional joins, an empty/refilled persistent game, and package replacement do not repeat opening; a persistent game closes only on actual registry removal. Enabling/reloading an observer does not replay old openings. Removing a registry entry consumes tracking before callbacks so repeated cleanup does not duplicate closure. There is no synthetic per-game close on manager shutdown, and disabling/faulting an observer can miss events. These are observations, not an exactly paired audit ledger.

Do not interpret opening as all players loaded, client rendering readiness, a quest starting, or an Episode 3 battle starting. Removal is not victory, quest completion, or a reason-specific end signal. Opening may happen before the game-join response is sent on creation paths. Relative ordering of final `lobby_leave` and `game_closed` follows upstream removal paths: disconnect removal emits leave first, while a successful move may unregister the old game before its membership callbacks. Do not depend on one universal ordering between those two events. No new client packet or gameplay mutation is introduced.

The optional `game-audit` package keeps independent opening/closure counts and logs game IDs/names. Enable it with:

```json
{"id": "game-audit", "grants": ["events", "storage"]}
```

Append it to `Extensions.Packages`, then run `reload config`, `extensions check`, and `extensions reload`. Actual authenticated-client event timing, persistent-game expiry, and proxy/spectator filtering remain live-client acceptance checks; independent tracker/runtime tests do not establish rendering readiness.

### Quest assignment and server map preparation

Register `quest_assigned` and `quest_map_loaded` with the existing `events` grant. Both use the same copied game metadata table and restricted authority as primary-game observations:

```lua
extensions.on("quest_assigned", function(info)
  extensions.log("Game " .. info.lobby_id .. " assigned quest " .. info.quest_number)
end)

extensions.on("quest_map_loaded", function(info)
  extensions.log("Game " .. info.lobby_id .. " prepared quest map " .. info.quest_number)
end)
```

| Event | Exact server boundary | `reason` |
| --- | --- | --- |
| `quest_assigned` | In `set_lobby_quest`, after game/quest compatibility checks, assignment, configured episode/difficulty/drop-mode updates, and item-creator preparation succeed; before per-client quest file sending begins. | `assigned` |
| `quest_map_loaded` | At the successful end of the existing `on_quest_loaded` routine, after replacing the server map, clearing floor items, and preparing the required overlays and associated state. | `map_loaded` |

`quest_number` is mandatory for both events and is the canonical server Quest metadata number, an exact integer 0..4294967295. It is not an arbitrary file name or proof of a per-client quest transfer. `quest_joinable` is a boolean copied from that quest's metadata. Other fields describe the current game: `lobby_id`, `name`, `episode`, `mode`, `difficulty`, `event`, and `reason`. Episode remains the game's actual episode, including an Episode 3 game using a substituted V3 quest. No quest scripts, file contents, player/account data, pointers, or live handles are exposed.

Only previously admitted primary games with the same current registry object can produce these events. Spectator/playback and client-customization games remain excluded. Tracker state belongs to the manager: one assignment and one matching map preparation per registered game lifetime, with assignment recorded before callbacks. Duplicate assignments, a different replacement quest number, map preparation before assignment, a mismatched quest, and notifications after removal are omitted. The upstream server currently rejects assigning a second quest to the same game. A new game registry lifetime gets fresh state. Package reload/check/disable or additional player joins do not reset it; enabling a late observer does not replay historical events.

A handler may log, read validated settings, and read/write its granted namespaced storage. It has no player-action authority: player-context messaging, timers, scene mutation, and late registration remain refused. With their normal grants, lobby queries return nil and scene capability queries return unavailable/zero budgets. Copied fields such as `info.can_act` cannot grant authority. Existing registration, instruction, memory, time, API/output budgets, cumulative diagnostics, and fault isolation apply. A failed assignment observer cannot veto native quest processing or prevent healthy observers from receiving a later map observation; earlier storage changes are not rolled back.

These are server preparation observations, **not client readiness, quest start/completion, or success notifications**. Pre-V3 clients do not provide the same loading confirmations as later clients, so the existing server may prepare its map immediately while downloads are still in progress. Later versions reach the map routine through existing loading/barrier logic. A late join to a running joinable quest synchronizes that client; it does not assign the group's quest or rebuild the group's map again. Existing Trial Edition, mixed-version, and disconnect/barrier behavior is preserved. A barrier release that does not execute `on_quest_loaded` does not invent a map-prepared event.

Assignment can be observed even if a later file send or map/overlay preparation fails; there is no rollback or synthetic failure/completion event. If native preparation fails before an observation boundary, that boundary emits nothing. Abrupt shutdown, package faults/disable, and skipped callbacks can leave unmatched observations. These events do not create a durable exactly paired quest ledger or change existing gameplay/protocol behavior.

The optional `quest-audit` package keeps separate assignment/map-preparation counts and last-observed quest numbers. Append this entry to your existing configuration and use `reload config`, `extensions check`, then `extensions reload`:

```json
{"id": "quest-audit", "grants": ["events", "storage"]}
```

Actual authenticated-client checks remain required for assignment/map timing, V1/V2 versus later loading paths, joinable late joins, substitutions, and disconnects. The automated tracker/runtime tests and headless registration checks are separate from those live-client gates.

### Initial quest-load acknowledgements

`quest_load_acknowledged` observes one tracked player's initial quest-loading acknowledgement. Register it with the `events` grant. It receives **one copied metadata table**, rather than a player-action context:

```lua
extensions.on("quest_load_acknowledged", function(info)
  extensions.log("Client " .. info.client_id .. " acknowledged quest " .. info.quest_number)
end)
```

The table includes the existing game/quest fields (`lobby_id`, `name`, `episode`, `mode`, `difficulty`, mandatory `quest_number`, and boolean `quest_joinable`), plus decimal-string `client_id` and the base version name `client_version`. `event` is `quest_load_acknowledged`; `reason` is `load_acknowledged`. Zero is a valid quest number. Session IDs remain exact strings, including values beyond signed/Lua integer ranges. No player name, account credentials, permission, quest files, pointers, or live handles are included. Other game/quest metadata events still omit these client fields. Mutating the table has no host effect.

The native boundary is the end of the **initial** `LOADING_QUEST` branch of `on_AC_V3_BB`: the existing empty AC packet is validated, the native loading flag is cleared, and any existing barrier/map preparation work must return successfully before notification. The adapter checks the current registered Client/Lobby identities, actual membership, authenticated connected local game-server phase, primary-game admission, and matching canonical assigned quest. Proxy, replay, spectator, playback, client-customization, and stale memberships are excluded. The game and client membership must already have been tracked.

Each tracked membership can acknowledge the game's one assigned quest at most once. Repeated AC commands or duplicate membership notifications do not repeat the observation. Leaving, disconnect/proxy handoff, and game removal discard member state; package check/reload/disable preserves completed and pending tracking. There is no historical replay. A genuine new membership has fresh state, but only a real initial loading acknowledgement can trigger this event; ordinary late-join synchronization does not.

This is **not quest readiness, start, completion, or visible rendering**. Clients without this acknowledgement (including pre-V3 paths) produce no synthetic substitute. The joinable-quest AC branch, BB `016F`, later visibility/state synchronization, and disconnect-driven barrier release are not this event. Existing Trial Edition, mixed-version, and late-join loading behavior is preserved. No extra packet, client patch, or barrier condition is added.

Ordering follows native processing: early participants' acknowledgements can precede `quest_map_loaded`; the final participant's acknowledgement follows it when that AC handler successfully prepares the map. Do not assume every acknowledgement precedes map preparation or that map preparation means every connected client acknowledged. If a native exception occurs before the hook, no observation is emitted, even if the native loading flag was already cleared. There is no fabricated retry, rollback, or failed-load event.

Handlers may log, read validated settings, and read/write granted namespaced storage. They cannot use player-context messaging, schedule/cancel timers, mutate scenes, or register definitions, even by forging `info.can_act` or `info.client_id`. Granted lobby queries return nil and scene capability queries return unavailable/zero budgets without consulting live state. Callback limits, diagnostics, and fault isolation are unchanged; tracking is consumed before callbacks, so a faulty observer cannot rearm the event or block healthy observers. Failed Lua allocation does not retain client metadata in later callbacks.

The optional `quest-audit` package now stores `loads-acknowledged` and `last-acknowledged-quest` alongside assignment/map counters. Live authenticated-client checks for native timing and exclusions remain acceptance gates; automated tracker/runtime contracts and headless registration do not establish rendering or whole-quest readiness.

### Joinable-quest loading acknowledgements

`quest_join_acknowledged` observes a tracked member's native pending-load flag being cleared while joining a running joinable quest. Register it with the `events` grant. It uses the same single copied client/game/quest metadata table as `quest_load_acknowledged`:

```lua
extensions.on("quest_join_acknowledged", function(info)
  extensions.log("Client " .. info.client_id .. " joined quest " .. info.quest_number .. " via " .. info.reason)
end)
```

`event` is `quest_join_acknowledged`. `reason` identifies the native boundary:

| Reason | Exact observed boundary |
| --- | --- |
| `bb_ready` | A BB `016F` handler cleared `LOADING_RUNNING_JOINABLE_QUEST` and successfully completed its remaining native work, including next-join scheduling. Ordinary `006F` does not qualify. |
| `player_visible` | A non-BB client's validated own-slot player-visible game command was forwarded and cleared `LOADING_RUNNING_JOINABLE_QUEST`. BB visibility, another player's slot, malformed packets, and visibility commands without the pending flag do not qualify. |

The fields remain `lobby_id`, `name`, `episode`, `mode`, `difficulty`, mandatory `quest_number`, boolean `quest_joinable = true`, decimal-string `client_id`, and base `client_version`, plus `event`/`reason`. Versions and reasons are checked together: only BB can report `bb_ready`, and only non-BB clients can report `player_visible`. No account credentials, player name, live objects, or handles are exposed. Modifying the copied table cannot grant authority.

The adapter requires a current authenticated connected local primary-game member, a matching previously tracked assigned joinable quest, and the native JOINABLE_QUEST_IN_PROGRESS flag. It rechecks registry objects and slot membership. Proxy, replay, spectator, playback, client-customization, stale, and non-joinable cases are excluded. A member can be observed once per game/quest membership; repeated packets, retransfers, duplicate joins, and package check/reload/disable do not rearm it. Leave/rejoin starts fresh membership, while disconnect/proxy handoff and game removal discard tracking. Pending observations survive package replacement; historical ones are not replayed.

The event is separate from initial `quest_load_acknowledged`, initial `game_join_ready`, and server `quest_map_loaded` observations. In particular, late-join AC starts native state synchronization and does not itself produce this event. Both reason values describe existing server handling of client signals; they do not prove equivalent rendering/state completeness across versions or establish whole-quest readiness, start, or completion. Non-BB clients, including older versions when they use this native visibility path, are observed only on their actual own-visibility signal. There is no synthetic event immediately after sending files, on map preparation, or when a disconnect releases a barrier.

Callbacks have the same read-only authority as initial quest acknowledgements: bounded logging/settings and granted namespaced storage, no player-context messaging/timers/scenes/late registration, and no live lobby/scene queries. Existing resource limits and fault isolation apply. Tracking is consumed before dispatch; an observer error cannot rearm it or prevent healthy observers. Allocation failures clear both game and client metadata before later callbacks. Native exceptions before the final boundary emit nothing, even if the native loading flag already cleared; no synthetic retry is created.

The optional `quest-audit` package stores `joins-acknowledged` and `last-join-quest` separately from initial-load and map counters. Real BB/non-BB late joins, rejoin/reconnect, mixed-version timing, own-slot filtering, and excluded contexts remain live-client acceptance checks. No new packets, client patches, gameplay mutations, or changes to native loading/visibility/barrier order are introduced.

### Package activation and unload

Register `package_load` and `package_unload` through the existing `events` grant:

```lua
extensions.on("package_load", function(info)
  local count = tonumber(extensions.storage_get("activations") or "0") + 1
  extensions.storage_set("activations", tostring(count))
  extensions.log(info.id .. " loaded: " .. info.reason)
end)

extensions.on("package_unload", function(info)
  extensions.log(info.id .. " unloaded: " .. info.reason)
end)
```

These handlers receive **one metadata table**, not a player context. It contains `id`, `version`, `event` (`package_load` or `package_unload`), and `reason`. There are no player/session identifiers or permission fields. Editing metadata cannot change the host's scope or lifecycle state.

| Notification | When | Reasons |
| --- | --- | --- |
| `package_load` | After the entire candidate set commits, in configured dependency order. Runs for each new instance, including an unchanged package replaced by reload. | `startup` for activation during server creation; `reload` for subsequent administrator reloads/re-enabling. |
| `package_unload` | When an activated instance is retired after successful replacement/removal, explicit disable, or graceful manager destruction. Reverse dependency order for a whole set. | `reload`, `disable`, or `shutdown`. |

Candidate construction, `extensions check`, failed initialization/migration/asset/dependency validation, and refused dependency unloads emit neither notification. All candidates finish validation before the active set changes. After replacement commits, old instances get unload first, then new instances get load. A removed package or `Extensions.Enabled: false` retires the old set with reason `reload`; configuration snapshot reload alone emits nothing.

Both events may log, read their own validated settings, and read granted storage. Load handlers may also write granted storage; those changes use the normal pending-save path and are visible to later callbacks. They see the successfully migrated data version. Unload storage is strictly read-only, including deletion: the replacement already has a staged copy, so an old handler must not change it or lose writes. Neither event may use player-context messaging, create/cancel timers, mutate scenes, register definitions, or request live player/lobby state. Lobby queries return nil and scene-capability queries return unavailable/zero budgets without consulting the live adapter. Initialization/migration restrictions and grant checks still apply independently.

Notifications are observations after commit, not approval hooks. Callback errors cannot veto an activation or retirement and do not roll back earlier storage changes. A successful check does not execute or guarantee successful load handlers. Load errors appear in the activated instance's callback diagnostics; unload errors are logged while removal continues. The normal three-error fault threshold applies, including lifecycle callbacks. Faulted packages execute no further script callbacks, including unload; the host still removes their resources. Retiring an instance cancels its timers independently of Lua. Post-commit scene synchronization errors are logged for normal periodic reconciliation, rather than reporting that package activation was rolled back. Periodic scene reconciliation retries independently of storage-save failures.

Handlers share the existing registration quota, per-call instruction/time/memory/API/output limits, and an aggregate 10 ms elapsed check per package notification. Remaining handlers may be skipped after budget exhaustion or faulting. Each instance is notified at most once per lifecycle boundary; notifications do not replay after dry checks and candidates that never activate receive no unload. Abrupt process termination, a faulted package, or skipped handlers can leave unmatched observations. Unload is not a guaranteed finalization or durability mechanism; normal storage persistence and host-owned cleanup remain responsible for those tasks.

The optional `package-audit` example records activation counts and logs unload reasons without client messages. Enable it with:

```json
{"id": "package-audit", "grants": ["events", "storage"]}
```

Append it to your existing `Extensions.Packages` and use `reload config`, `extensions check`, then `extensions reload`. Existing packages need no new grants unless they choose to register these events or use storage.

### Messaging limits

Text input is limited to 512 bytes per operation and 4096 bytes per callback. Delivery is conservatively truncated to 200 UTF-8 bytes without splitting a valid character, to stay within legacy encoded-text limits, including UTF-16 clients. Keep messages short. Protocol control characters are stripped except newlines; Newserv formatting metacharacters are escaped so `$`, `#`, and `%` remain literal text. Color/control-code formatting is not an extension feature. A whisper command example uses `$extid` to discover the current session ID. The included `chat-tools` commands are `shout`, `whisper`, `extid`, `remind`, `repeatremind`, and `extvisits`; only `extvisits` requires the `announce` user permission.

## Storage and limits

Each package has a namespaced string store with at most 256 keys and 64 KiB of combined key/value bytes. Keys are 1..64 bytes; values are at most 4096 bytes. Embedded zero bytes are rejected. Storage is saved to ignored `system/extensions-state/<id>.json` files. The host coalesces writes approximately every five seconds and flushes before reload/unload and orderly destruction. A crash can lose the latest unsaved changes. Writes replace a temporary file in the same directory; this is not a guarantee against every power-loss failure. Failed writes retain dirty state for retry and do not prevent other packages from saving; a reload/unload that cannot flush still fails rather than discarding unsaved data.

### Package data versions and migrations

Set `"storage_version": 1` in the package manifest to opt into versioned application data. The default is zero for existing unversioned packages. Versions are integers from 0 to 2147483647, independent of `api_version` and the informational package `version` string. Register the upgrade code in the entry script:

```lua
extensions.migrate(function(old_version, target_version)
  assert(old_version == 0 and target_version == 1)
  if extensions.storage_get("total-visits") == nil then
    extensions.storage_set("total-visits", "0")
  end
end)
```

The shipped `chat-tools` manifest and script demonstrate this upgrade, preserving existing visit counts. New installations also start at data version zero with an empty store. An upgrade requires both the `storage` grant and a registered migration; otherwise activation fails. For larger version jumps, the single handler receives the actual old and requested target versions and must implement/validate every supported transition itself. There is no automatic step discovery or downgrade handler. Lowering/removing the manifest version after upgrading stored data rejects activation; restore a compatible package or an appropriate administrator-maintained data backup instead of silently reinterpreting the newer store.

The callback runs after the entry script finishes registering definitions. It gets a fresh instruction/time/API budget and can read its validated settings with `setting` and use `storage_get`/`storage_set` among host actions. It has no player context, cannot message, log, schedule timers, change scenes, register more callbacks/commands, or register another migration. The usual Lua memory, 32-call API, key/value/count/total-byte, and instruction/time limits apply. Plan bulk transformations within these bounds. Ordinary initialization still cannot read/write the store.

Each candidate runtime gets its own storage copy. A successful migration updates that copy and marks its new data version dirty. Every package, command, dependency, and asset must finish staging before the candidate set activates; a later failure discards all candidate changes. The active set and its storage remain intact. The host may flush previously active dirty data before attempting a reload, as before. On equal versions, the migration is not called and does not dirty the store. Completed version/data pairs persist through the normal coalesced/reload/unload/shutdown save path; a restart loads the persisted version and skips already-completed upgrades. A crash before a successful save can require the migration to run again against the older persisted data. Keep migration behavior deterministic and confined to the staged store.

The file envelope remains host format `schema_version: 1`; the new optional `data_version` records the package version:

```json
{"schema_version": 1, "data_version": 1, "values": {"total-visits": "0"}}
```

Existing format-1 files without `data_version` load as data version zero. Corrupt files, unsupported host formats, invalid data versions/values, and failed migrations reject activation without resetting the files. `extensions list` displays each runtime's data version and key count. Atomic replacement applies to each individual file; saves across multiple packages are not a filesystem transaction. Lua globals, timers, and scene objects are not persisted by storage migrations.

Each package gets an independent Lua state with a 4 MiB Lua allocation limit. Each callback/initialization has a 100,000-instruction budget and a 10 ms elapsed-time check every 1000 instructions. Host operations also have a 32-call callback budget. At most 32 timers are pending per package; each 50 ms host tick executes at most one due timer per package, with a rotating package order and an aggregate time check. Timer delivery can be late when the server is busy. Elapsed checks do not preempt a blocking native operation or OS I/O. Storage serialization/writes are bounded but synchronous on the server executor.

The library environment omits filesystem/OS access, package/native loading, dynamic compilation, debug/coroutines, protected script calls, and metatable manipulation. It also removes Lua pattern matching, `string.rep`, `table.move`, and `table.sort`, whose native loops could evade instruction-hook accounting. Basic, math, UTF-8, and the remaining string/table functions are available. This is a deliberately restricted Lua API, not unrestricted Lua compatibility.

Errors are contained at the script boundary and logged; command failures receive a short reply. After three callback failures, the package is faulted and its pending timers are removed. A successful staged reload creates a fresh instance; lifecycle errors can still fault that instance during activation. Faulted packages retain their command names until unloaded/replaced. Successful actions before an error are not rolled back. Resource restrictions reduce mistakes; in-process Lua is not process-level isolation against hostile native code or vulnerabilities. Install only administrator-trusted extensions. There is no raw packet, shell, arbitrary client patch, character mutation, or general network API.

## Administration and testing

For the local review report and validation procedure used before incorporating upstream changes, see [reviewing upstream updates](EXTENSIONS-UPSTREAM.md). This maintenance tool is independent of the running server and Lua APIs.

Server shell commands, also subject to any existing shell access configuration:

```text
extensions list
extensions info chat-tools
extensions scenes lobby-decor
extensions check
extensions reload
extensions disable chat-tools
```

After editing the main server configuration, run `reload config`, then `extensions reload`. The first command refreshes Newserv's config snapshot; the second stages packages from that snapshot and rereads their scripts/manifests. A script-only change needs only `extensions reload`. Reload validates every package and command before replacing the active set. Disable unloads a package for the current session; remove it from the enabled configuration to keep it disabled after reload/restart. Unload dependents before their dependencies. The host retains a failed startup as an empty set so the server can still run and administrators can repair/reload it.

Use `extensions check` before reload to validate the current candidate configuration without activating it. Like reload, it reads manifests/scripts/assets and executes bounded initialization and required storage migrations in separate candidate runtimes. It validates grants, dependencies, command collisions, schemas, and the complete native asset catalog, then reports package/command/asset counts and candidate data versions. It discards those runtimes without changing active packages, timers, scene objects, session tracking, or storage. Candidate migration data is never saved. Check does not flush even pending active storage and can succeed while a storage writer is blocked. Normal background saves continue independently after the command returns. Like reload, it uses the current config snapshot: run `reload config` first after editing the main configuration. A successful check is not an activation or a promise that later-changed files will reload successfully.

`extensions scenes ID` inspects the loaded package's current native scene registry, including a faulted package. It reports backend/experimental-model switches; prepared asset IDs; compressed texture/model/animation byte counts (zero means absent); original/PC envelope compatibility; exact model build allowlists as hexadecimal identifiers; and complete original-client fallback metadata. Owned objects list their key, lobby ID, logical asset ID, and native location bitmask. Event overrides list the lobby ID, restoration value, and last applied value. An administrator's intervening event change may prevent restoration; these are stored override records, not a query of current client state.

This inspection never executes Lua, stages files, sends media, advances recipient caches, clears errors, or reveals payloads, asset paths, setting/storage values, or player/account identities. It shows only the selected package's records in deterministic order, bounded by the existing 64-asset global, 32-object per-package, and 128-event global quotas (model allowlists have at most 16 builds per variant). Objects also report their configured lifetime or persistent status, not a countdown. Unknown/unloaded IDs and malformed arguments fail. A dry check or rejected reload leaves these active records unchanged, apart from normal independent background expiration; a successful replacement drops old objects and replaces prepared assets, and unload removes the package. Size compatibility alone does not establish build allowlisting, scene eligibility, successful delivery, or rendering. Live client acceptance checks remain necessary.

`extensions info ID` inspects the currently active instance, including faulted instances. It shows current/peak Lua allocation and the limit, timers, storage version/key count/byte usage/pending-save status, callback attempts/failures, requested-and-granted permissions, dependencies, setting names/types/set status (values hidden), and command help/usage/permissions/cooldowns. It does not execute callbacks, read replacement scripts, clear counters, or dump storage values. Diagnostics are snapshots, not handles into Lua. Peak allocation counts Lua memory, not total process memory. Callback attempts include dispatched commands, event handlers (including package lifecycle), and due timers that actually enter Lua; denied input, schema failures, cooldown refusals, initialization, and migration staging are excluded. Failures remain cumulative across successful calls and fault the package at three, as before. The last callback error is retained up to 1024 bytes. All error lines are flattened for console display; errors can still contain text deliberately included by a script.

List output retains separate last-check and last-reload failure messages, including failed initial startup. A successful check clears only the check failure; successful reload clears only the reload failure. Per-runtime counters and callback errors reset when a new runtime activates. Inspection and check require the existing administrative shell access; there is no new player command or extra script grant for these diagnostics. No shared history or automatic retry/activation is introduced.

The runtime tests can run without Newserv's phosg/resource_dasm/Asio dependencies:

```sh
cmake -S . -B build/runtime-tests -DNEWSERV_EXTENSIONS_TESTS_ONLY=ON
cmake --build build/runtime-tests
ctest --test-dir build/runtime-tests --output-on-failure
```

They still require the Lua 5.4 headers and library. Full CI builds both feature-disabled and feature-enabled configurations and runs the existing upstream tests plus the runtime tests when enabled. No CI run is started or pushed by local implementation work.

Before a server deployment, run the complete server checks in the target build environment and smoke-test the example package with actual clients: shout/whisper, denied permissions, reload failure, pending timers during disconnect/move, and storage after restart. No arbitrary scene/model support or new client packet compatibility is claimed by these tests.

### Callback timing diagnostics

`extensions info ID` also reports monotonic wall-clock callback time in integer microseconds:

```text
Callback time (us): last=...; peak=...; total=...; over 10 ms=...
```

Last and peak measure one protected callback invocation; total accumulates invocations for the current runtime. Measurement begins at the configured callback budget's start and includes Lua context/table creation, script execution, synchronous native API work, and clearing native query snapshots after the protected call. It excludes timer queue waiting, package staging/initialization/migration, denied dispatch/schema/cooldown refusals, and error reporting performed after the invocation. Ordinary commands, events, due timers that enter Lua, and package lifecycle callbacks are included, including callbacks ending in script errors or memory exhaustion. Very short invocations may round down to zero microseconds.

The deadline count records invocations finishing at or beyond the existing 10 ms deadline. This is an observation, not a new timeout/fault rule: native work cannot be interrupted by a Lua instruction hook, and a callback may finish successfully after that deadline. Existing instruction/time enforcement and the three-error threshold are unchanged. These are elapsed times, not CPU profiling; OS scheduling or native waits can increase them. Use peak time and deadline counts alongside callback failures when diagnosing an extension.

Inspection does not execute callbacks or reset counters. Values survive ordinary successful/failed calls and remain inspectable on a faulted runtime; a replacement runtime starts at zero, and dry checks/rejected candidates leave the active counters intact. Total time and the deadline count saturate at the unsigned 64-bit maximum. Timing is not persisted in extension storage or exposed through a new Lua API, and no account/settings/storage values are included.

## Windows builds and test data

On MinGW toolchains that fail Newserv's default static C++ link, configure with `-DNEWSERV_WINDOWS_STATIC_LINK=OFF` and put matching runtime DLLs on `PATH`. POSIX-thread-capable MinGW builds explicitly select the same Asio thread backend in every translation unit.

If Git checked out symlinks as small text files, prepare a separate fixture for tests instead of rewriting them:

```sh
python3 tests/extensions/PrepareTestFixture.py --source . --destination build/test-data
cmake -S . -B build/server -DNEWSERV_ENABLE_EXTENSIONS=ON -DNEWSERV_WINDOWS_STATIC_LINK=OFF -DNEWSERV_TEST_DATA_DIR=/absolute/path/to/teaserv/build/test-data
cmake --build build/server
ctest --test-dir build/server -R "^(extension-|log-replays$|replay-line-endings$)" --output-on-failure
```

The fixture destination must be new and inside this checkout's `build/` directory. The preparation script reads local Git metadata, copies tracked `system/` and `tests/` files plus the example checker image, and resolves link targets. It does not copy ignored accounts, players, or local configuration. Python and phosg's `jsonformat` must be discoverable for the host smoke test; set `PHOSG_JSONFORMAT_EXECUTABLE` if necessary. Normal Newserv dependencies are still required. See [validation notes](EXTENSIONS-VALIDATION.md) for this machine's dependency-specific adjustments.
