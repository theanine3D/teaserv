# Newserv extensions system: design and protocol investigation

Implementation status: the first local milestone provides the Lua server-extension foundation. The second adds a native scene registry, canonical retail Episode 3 lobby media composition, bounded BMP/GVM/PRS asset preparation, and owned lobby-event overrides. Model/animation transport is experimental, disabled by default, and build-allowlisted; real-client rendering remains an outstanding phase-2 acceptance gate. The Windows integration follow-up fixes Asio header-order differences, CRLF replay parsing, and Sega character conversion; the strict host smoke test and all 16 replays now pass with extensions enabled and disabled. Command-authoring adds optional declarative argument schemas, typed Lua values, and generated usage replies while preserving raw-string handlers. Storage hardening adds package data versions, bounded staged migrations, independent write-error retries, and disk/restart verification. Session lifecycle adds read-only client_login/client_disconnect observations, deduplicated local admission, proxy handoff suppression, and version/game context snapshots. Administration adds validation without activation, package health/command details, and retained reload/check failures. Typed per-package settings add administrator overrides and staged validation. Read-only lobby snapshots add scoped connected-player enumeration with a separate lobby.read grant and optional who example. See [the implemented API](EXTENSIONS.md), [native scenes](EXTENSIONS-SCENES.md), [upstream integration map](EXTENSIONS-INTEGRATION.md), and [validation notes](EXTENSIONS-VALIDATION.md). Package lifecycle adds post-commit load and read-only unload observations, dependency ordering, isolated faults, and graceful shutdown coverage. Primary-game observations add deduplicated first-local-join and actual-registry-removal events with read-only metadata and no player authority. Quest observations add assignment and completed server-map preparation boundaries, with matching-quest deduplication and no client-readiness claim. Individual initial game-join acknowledgements are implemented as read-only game_join_ready observations. Initial and joinable quest-load acknowledgements are also implemented as read-only observations; whole-game/quest readiness, additional scene backends, character mutation, and enhanced-client protocols remain future work. Exact E3/01 login identification is implemented; optional original-client fallback groups substitute smaller assets without account-based overrides. Live interoperability/rendering remain acceptance gates. Source findings below describe the original baseline.

Reviewed source: upstream newserv, commit `46ab6f106618060157b8fc99e840bb6be4f15c11`. This is a source review and implementation plan, not a tested client compatibility claim. The original source was left unchanged.

## Recommendation

Future arbitrary-placement models, HTTPS content delivery, custom music, collision, and capability-restricted custom lobbies are specified in the [custom lobby content implementation plan](CUSTOM-LOBBY-CONTENT-PLAN.md). That proposal is separate from implemented native B9 support and does not expand the meaning of the existing E3/01 marker.

Build a server-side extension API first, with Lua as the initial language. Keep scripting separate from client rendering: scripts request semantic actions, and C++ services choose a supported implementation for each recipient. Chat commands should need no new client protocol. Scene decoration should have several backends, with explicit capabilities and graceful omission of unsupported cosmetic features.

The compatibility rule is: **never send an extension packet to a client that has not explicitly negotiated support for it**. Compatibility must not depend on an original client ignoring an unknown command.

## What already exists

| Existing facility | Evidence in the local source | Consequence |
| --- | --- | --- |
| Compiled chat command registry | `src/ChatCommands.cc:169` and `:3209` | Extend dispatch with a dynamic registry; retain existing commands and their checks. |
| Configurable chat prefix | `src/ReceiveCommands.cc:3666`, `src/DataIndex.cc:931`, `system/config.example.json:240` | `/shout` can use the existing `ChatCommandSentinel` setting. Defaults are `$`, or `@` on the November 2000 prototype. |
| Client quest scripting tools | `src/QuestScript.hh:217` | These assemble/disassemble client quest bytecode; they are not a general server plugin runtime. |
| Native client functions and patches | `src/ClientFunctionIndex.*`, `src/SendCommands.cc`, `system/client-functions/` | Reuse build identification and patch infrastructure where suitable; scripts should not construct arbitrary executable code. |
| Episode 3 lobby media | `src/CommandFormats.hh:2185`, `src/DataIndex.cc:772`, `src/SendCommands.cc:3328` | There is already a protocol for downloading some lobby content. |
| Debug object creation | `src/ChatCommands.cc:1615`, `system/client-functions/Debug/CreateObject.s:4` | Useful prototype, but not a production scene API. |
| External integration | `README.md:744` | HTTP shell execution and a rare-drop WebSocket exist, but do not constitute an in-process extension API. |

I found no general Lua/Python server extension loader or public plugin API in the inspected build configuration and server code. Existing client functions, configuration, quests, and HTTP integration are distinct facilities.

## Investigation: unknown packets and client behavior

`src/CommandFormats.hh` identifies itself as the canonical protocol reference. It documents many individual cases, but I found no general statement or complete dispatcher implementation establishing that every original client safely ignores arbitrary unknown top-level commands or subcommands.

| Finding | Local evidence | Interpretation |
| --- | --- | --- |
| Game-protocol commands `12`, `14`, and `16` are explicitly valid but ignored | `src/CommandFormats.hh:660`, `:696`, `:704` | These specific commands have documented behavior. They do not establish behavior for unassigned opcodes or the separate patch-server protocol. |
| Some opcode entries are marked “Invalid command” | For example `src/CommandFormats.hh:864` | A classification, not proof of safe ignore, disconnect, or crash. |
| Legacy `0E` can loop indefinitely | `src/CommandFormats.hh:540` | If the local Guild Card number is absent from the checked entries, the receive thread softlocks. This is a known handler failure, not an unknown-opcode test. |
| DCv2 legacy `03` can loop indefinitely | `src/CommandFormats.hh:335` | Additional evidence that unusual packet paths can have serious effects. |
| Oversized game subcommands have undefined behavior in `60`/`62` | `src/CommandFormats.hh:3575` | Correct opcode selection alone does not guarantee safety; framing and sizes also matter. |
| Some subcommands are ignored in disallowed areas or when affecting the local player | `src/CommandFormats.hh:3581` | “Ignored” may be conditional on area, version, and target. |
| Episode 3 `B9` ignores unsupported media types and out-of-limit payloads | `src/CommandFormats.hh:2186` | A documented fallback inside a recognized command; not a generic extension transport. |
| Client handler addresses are catalogued across many builds | `notes/handler-tables.txt:38`, `:165` | Useful entry points for examining client dispatch. Missing table entries alone do not reveal fallback behavior. |
| Invalid forwarded commands historically crashed clients, sometimes corrupting saves | `README.md:42` | Relevant historical warning, without proving the behavior of arbitrary unknown opcodes. |

**Do not confuse the server's behavior with the client's.** `src/ReceiveCommands.cc:5982` logs and throws for an unimplemented incoming command. `src/GameServer.cc:141` catches command-processing exceptions and disconnects that connection. This describes a packet received by Newserv, not one received by the game.

Newserv already has custom protocol extensions: command `30` requests extended player information through a patch, and `6xE4` supports patched enemy damage synchronization (`src/CommandFormats.hh:7208`). `EnemyDamageSyncGC.s:44` installs an `E4` handler in the client table. These are useful precedents for coordinated client/server features, not evidence that an unpatched receiver can accept the same traffic.

### Follow-up research needed to answer the unknown-opcode question precisely

For each exact executable/build, inspect top-level and game-subcommand dispatch around the handler tables. Record lookup bounds, missing-handler behavior, phase restrictions, packet size validation, receive-buffer advancement, and whether the next valid packet is processed. Distinguish game-server traffic from patch/data-server traffic.

Test representative DC v1/v2, PC v2, GC Episodes 1&2, Xbox, BB, and Episode 3 clients; track prototypes separately. Include regions and revisions where dispatcher implementations differ. Record binary hashes and patch state, not just a broad platform name. Run controlled tests against copied saves or disposable profiles, covering login, lobby, loading, and game states. Test well-framed unknown opcodes separately from malformed lengths, and observe crashes, disconnects, hangs, framing desynchronization, and recovery on a following known command. Expand only where code comparison justifies equivalence.

The deliverable should be a per-build matrix with evidence and “unknown” entries where untested. No client executables were executed in this investigation, and the premise about newly available client decompilations has not been independently established here.

## Scene decoration: capabilities and limits

The disc-only premise has an important exception. Retail **Episode 3** supports `B9` lobby media updates with types 1 (texture/GVM), 2 (model), 3 (animation), and 4 (delete all media). It is not valid on Episode 3 Trial Edition. Models need a separately sent texture with matching location flags; animations also need a model. Placement uses predefined location bits rather than arbitrary world transforms.

The documented limits are compressed data at most `0x3800` bytes, decompressed data **less than** `0x37000` bytes, and apparently eight distinct banners/models. `B9` acknowledgement is sent even if the client ignores the content, so it is not proof that an asset loaded or rendered. These statements come from `src/CommandFormats.hh:2185–2248`.

Newserv's low-level sender accepts a media type and payload, but its configured content loader only accepts `.bmp`, `.gvm`, and `.gvm.prs` (`src/DataIndex.cc:772`). The README explicitly says custom lobby 3D models are not implemented (`README.md:418`). Implementing model/animation preparation, validation, and lifecycle is still necessary. Also resolve a boundary discrepancy: the loader rejects decompressed sizes greater than `0x37000`, while the protocol comment requires fewer than `0x37000`; use the stricter limit until client analysis confirms it.

`$makeobj` invokes a compiled `CreateObject` client function. Its supplied implementation targets only `3OE1` and `3SE0`, which the README maps to GC v1.1 US and Episode 3 US. The README warns that the object exists only for the caller, is absent from server map state, and may cause a disconnect if it emits update commands. Do not expose it directly as a supported shared-object feature.

| Backend | Intended use | Availability |
| --- | --- | --- |
| Existing holiday/event presentation | Built-in lobby decorations | Use existing version-aware event behavior; not arbitrary model placement. |
| Native Episode 3 media | Custom textures/models/animations at supported lobby slots | Retail Episode 3 only, after implementing and verifying model support. |
| Validated client object constructor | Reuse selected assets already present and loadable in the current scene | Exact builds with a verified adapter or patch; asset existence on disc is insufficient. |
| Negotiated scene protocol | Arbitrary cosmetic transforms, additional scenes, optional custom assets | Modified clients advertising the required capabilities. |

For an unsupported original client, omit an optional decoration or use an explicitly defined supported substitute. Require an enhanced-only room if a feature affects collision, navigation, combat, or shared gameplay and cannot be represented consistently. Do not let an invisible decoration become a blocker for vanilla players.

## Extension host and API

Use one independent Lua state per extension, with a pinned runtime version selected during implementation. Lua's C API provides independent states, allocator control, and instruction hooks that fit resource accounting ([official Lua manual](https://www.lua.org/manual/5.4/manual.html)). My recommendation is based on the small embedded API needed here, not a claim that Lua is the only viable language. Keep the C++ service interfaces language-neutral. Add Python later through a worker process if its ecosystem is needed; Python audit hooks are explicitly not a sandbox ([official Python documentation](https://docs.python.org/3/library/sys.html#sys.addaudithook)).

An extension package should contain `extension.json`, an entry script, optional configuration/schema, and prebuilt assets. The manifest declares a unique ID, version, API compatibility range, dependencies, requested permissions, and required/optional client capabilities. Use `system/extensions/<id>/` for packages and a server config allowlist for enabled packages and permission grants. Startup validates paths, dependencies, duplicate IDs, and command conflicts before activating anything. Server admins control installation; clients never upload executable server extensions.

Expose narrow services:

| Service | Initial operations |
| --- | --- |
| Commands | Register name, argument schema, help, permission, rate limit, and handler. |
| Players | Obtain immutable snapshots; send private messages; request validated character actions. |
| Lobbies | Enumerate permitted members, broadcast text, inspect scene and event state. |
| Scenes | Query recipient capabilities; create/update/remove an owned cosmetic object using a logical asset ID. |
| Events | Subscribe to connection, lobby join/leave, scene-ready, game start/end, and unload. Add combat/item events later. |
| Timers | Schedule bounded callbacks and cancel owned timers. |
| Storage | Namespaced bounded key/value data, schema version, and explicit migrations. |
| Administration | Per-extension logs, health, enable/disable/reload, and resource usage. |

Do not expose raw C++ pointers, unrestricted packet sending, shell execution, unrestricted filesystem/network access, or arbitrary memory-patching functions in the normal API. Use opaque handles with generation/session identifiers so stale callbacks cannot affect a new occupant of a reused client or lobby ID. Recheck caller permissions, extension grants, target identity, current scene, and capability support when an action executes.

The two permission axes are independent: an extension must have the server grant, and the invoking user must have the required command permission. Event callbacks without a user execute under explicitly granted extension authority.

Run state changes on the existing Asio server executor; keep callbacks short and nonblocking. Queue bounded work and make ordering explicit. C++ coroutine boundaries catch script failures so they do not escape into the connection handler and disconnect the invoking player. Never hold raw client state across an asynchronous wait.

Limit memory with a per-state allocator; limit script instructions and callback time; bound timers, queued jobs, API work, output bytes, scene objects, and storage. Hooks cannot preempt arbitrary native C work, so exported functions and permitted library operations must also be bounded. Avoid opening `io`, `os`, `debug`, native module loading, or arbitrary package search; accept script source rather than arbitrary bytecode. Resource controls reduce accidental failures; they do not provide process-level isolation from native bugs or hostile plugins. Require an OS-restricted worker process for genuinely untrusted code.

On repeated failures, disable the extension and remove its registrations and owned resources. For reload, validate a replacement in a staging state with side effects disabled, then switch registrations at an executor boundary. Tag timers/tasks with an extension generation and cancel the old generation. Preserve the old instance if staging fails. Do not promise automatic rollback of gameplay actions already committed by earlier callbacks.

### Example API shape (proposed, not implemented)

```lua
extensions.command("shout", {
  permission = "chat.shout",
  arguments = {{name = "message", type = "text", required = true}},
  cooldown_seconds = 10,
}, function(ctx, args)
  ctx.lobby:broadcast_text(ctx.player:name() .. ": " .. args.message)
end)

extensions.on("scene_ready", function(ctx)
  ctx.scene:spawn_cosmetic({
    key = "welcome-statue",
    asset = "welcome.statue",
    placement = {slot = "south_platform"},
    unsupported = "omit",
  })
end)
```

Use the configured prefix, so this command is `$shout` by default or `/shout` when configured. Preserve escaped-prefix handling and built-in dispatch. Reject name collisions rather than silently overriding admin commands. A later transform-based placement is a separate capability from Episode 3 fixed-slot placement. Return structured failures and supported alternatives when an asset/backend is unavailable.

`/levelup` needs its own service, not a wrapper around assigning a level field. Validate level limits, compute EXP and stats coherently, persist through existing character services, and apply the correct version-specific synchronization. The README documents limitations for `$edit` on V3 and on DC/PC character persistence (`README.md:685`). Initially enable progression mutation only on versions and states with a verified implementation; reject unsupported cases clearly.

## Backwards-compatible protocol design

1. Every connection starts with no negotiated extension capabilities. Native documented capabilities, such as retail Episode 3 lobby media, are separately assigned from exact client identification.
2. A modified client sends an authenticated-session `EXT_HELLO` after normal login, in a deliberately selected phase. It includes a protocol magic, supported version range, client build identifier, feature bits, and receive/asset limits. The server does not probe stock clients with a new packet.
3. Only the modified Newserv version knows this request handler. A modified client connecting to an older server must remain in legacy mode unless support is known through a separately defined deployment/bootstrap mechanism. The current server disconnects on unimplemented commands, so unsolicited hello traffic to old servers is not safe.
4. Select a reserved opcode only after surveying all relevant versions and existing extensions. Do not reuse `30` or `6xE4`. In particular, Newserv dispatches top-level commands through `handlers[msg->command & 0xFF]`; a new BB high-byte value is not automatically a separate namespace. Add exact command validation/dispatch where required.
5. The server answers with the negotiated intersection and session-bound settings. Scene traffic begins only after negotiation and scene readiness; reconnect/ship transfers reset negotiation. Capability claims grant protocol compatibility, never admin authority.
6. Send extension traffic directly per recipient. Gate at the central encoder/send boundary and block automatic lobby forwarding of extension payloads. Translate to a legacy representation only when one is verified. Proxy mode is disabled for the initial extension API; later support requires independent negotiation for each connection leg.
7. Define a versioned envelope with explicit byte lengths, message type, object identity/generation, and a negotiated payload limit inside each platform's existing framing/encryption. Respect the existing outbound `0x7C00` command limit (`src/Channel.cc:117`) and any smaller command/build limits; split assets at the application layer if needed. Unknown optional messages may be skipped by the *new* client parser using validated lengths; incompatible required messages disable the feature. Malformed framing is handled as a protocol error. Document byte order, alignment, and every maximum.
8. Scene messages cover snapshot begin/end, spawn, transform/update, and remove. Use scene epochs and revisions so late callbacks cannot resurrect objects after a scene switch. Replay the current snapshot for late joiners, with per-recipient omission or substitution.
9. Keep asset transport separate from object spawning. Prefer preinstalled assets first. If downloads are added, negotiate the channel, use a content-addressed allowlisted catalog, verify hashes, bound compressed/decompressed sizes and decode work, and wait for explicit asset readiness. Do not assume arbitrary URLs, modern mesh formats, or PC patch downloads work on consoles.

No final opcode assignment or claim of universal client support is made by this plan.

## Fork architecture and incorporating upstream updates

Maintaining a separately developed fork while regularly incorporating upstream Newserv updates is an explicit design requirement. The goal is a small, understandable integration surface and repeatable validation. No architecture can guarantee conflict-free updates when upstream changes the same interfaces or behavior.

### Initial fork and branch layout

Leave GitHub's **Copy the master branch only** option enabled for this project. We need the upstream default branch as our starting point; copying every upstream development branch is unnecessary. This selection does not prevent future upstream merges or creation of our own branches. Additional upstream branches can be copied later if needed. See [GitHub's fork documentation](https://docs.github.com/en/pull-requests/how-tos/work-with-forks/fork-a-repo?tool=webui).

Use `origin` for the user's fork and `upstream` for the original Newserv repository. Recommended branch roles:

| Branch/ref | Purpose |
| --- | --- |
| `upstream/master` | Remote-tracking reference refreshed by fetching upstream; contains no fork modifications. |
| `upstream-mirror` | Optional published branch in the fork, kept identical to upstream master by fast-forward updates only. |
| `extensions` | Stable development line for the extensions-capable server; can become the fork's default branch. |
| `integrate/upstream-<date>` | Temporary branch from `extensions` on which an upstream revision is merged and validated. |
| Feature branches | Focused extension changes submitted to the fork's development line. |

The initial GitHub fork may retain `master` unchanged while `extensions` is created. Branch names are conventions, not requirements, and need not match the upstream default branch. Keep the actual upstream reference configurable if the original project renames its branch. Do not force-reset a branch containing extension development to match upstream.

### Keep upstream-facing changes narrow

Use the following dependency direction:

```text
Existing Newserv code
    -> Small command and lifecycle hooks
    -> Newserv adapter layer
    -> Stable extension services
    -> Extension manager and Lua runtime
    -> Scripted extensions
```

Put the package loader, runtime, permission model, dynamic registry, scene registry, and extension storage in dedicated modules, such as `src/Extensions/`. Confine modifications to upstream files to deliberate integration points: server ownership, configuration, command dispatch, lifecycle notifications, and supported send/receive paths. Maintain a short integration map naming each changed upstream function, the hook's purpose, ordering, and expected state invariants.

The adapter layer translates `Client`, `Lobby`, character services, and existing packet helpers into stable semantic operations. Scripts receive snapshots and opaque handles, not upstream structure layouts or writable C++ objects. If upstream changes an internal field or object lifetime, repair the adapter instead of requiring every script to change. Version the extension API independently of upstream revisions and publish migration notes for intentional API changes.

Reuse upstream services for messaging, permissions, character changes, and version handling. When a small extraction is necessary to make an existing operation reusable, keep it separate from the feature implementation and preserve its existing behavior. Avoid copied implementations, broad refactors, formatting churn, and new conditionals scattered across unrelated game logic. Keep generic fixes and upstream bug fixes in separate commits from extension functionality so each can be reviewed or adopted independently.

Add an optional build switch, proposed as `NEWSERV_ENABLE_EXTENSIONS`. With it disabled, exclude the runtime dependency and preserve the original server behavior. Keep extension-specific configuration and assets separate from upstream example data where practical; preserve upstream defaults. A separate repository or dynamically loaded C++ ABI is not required: narrow interfaces and separate modules within the fork are sufficient initially.

### Upstream integration procedure

1. Fetch upstream and record the candidate commit, its changes, and the last successfully integrated revision. Advance the optional mirror only by fast-forward.
2. Create an integration branch from the current extensions line and merge the selected upstream revision. Prefer normal merge commits for updates to the published fork; avoid routinely rebasing shared development history. Merge coherent upstream history by default. Cherry-pick only for a deliberate selective backport after checking dependencies, and record its source revision.
3. Resolve textual conflicts and inspect behavioral changes at every integration point, even when Git reports a clean merge. Pay particular attention to client/lobby lifetime, asynchronous ordering, permission checks, character persistence, protocol framing, and client identification.
4. Build and run applicable upstream checks with extension support disabled and enabled. Run extension integration tests and the relevant client compatibility checks. Any upstream-only failure must be diagnosed and recorded separately; it is not automatically acceptable just because it also occurs without extensions.
5. Review the integration diff, update the capability matrix and adapter map as needed, then promote the validated merge through a pull request in the fork. Record the upstream commit and verification results in release notes. Retain the previous working release for rollback.

Integrate upstream frequently enough to keep the divergence understandable, and prioritize relevant security or protocol fixes. A clean merge establishes source compatibility only; it does not prove that runtime behavior or client compatibility is preserved.

### Maintenance acceptance criteria

- Upstream baseline builds and checks remain usable, and CI covers both build modes on supported targets.
- Contract tests cover command registration, event ordering, permissions, stale handles, reload/unload, and script failures across adapter changes.
- Recipient-level protocol tests ensure original clients receive no custom packets; mixed-client checks run whenever affected networking or client-identification code changes.
- Each release identifies its upstream base and extension API version. Supported scripts retain compatibility across routine upstream integrations unless a documented API migration is necessary.
- Client patches tied to executable addresses, shared object synchronization, and character mutation receive explicit review when their dependencies change. These are expected to require more maintenance than the isolated script host and chat commands.

## Implementation sequence and acceptance gates

Server-wide announcements are implemented through the explicit `chat.announce` grant and `extensions.announce`. Registered event callbacks can use granted server authority without a live player; commands and player timers additionally require the account's ANNOUNCE flag. Native delivery, bounded escaped text, recipient gates, per-package/global cooldowns surviving reload, and normal refusal results preserve the adapter boundary. The optional server-announcements example demonstrates game-opening messages and an admin command. Real-client presentation remains unverified; see [announcements](EXTENSIONS.md#server-wide-announcements).

An offline upstream audit tool now compares locally available revisions against the recorded shared baseline. Its maintained watch inventory covers integration hooks and related client/lobby, framing, codec, and quest services. Reports separate watched changes, all other upstream paths, and same-path fork changes including uncommitted files. It performs no fetch, merge, activation, or compatibility verdict; human review, both build modes, and live acceptance remain required. See [upstream auditing](EXTENSIONS-UPSTREAM.md).

Bounded repeating timers are implemented as `extensions.every`, sharing existing grants, quotas, callback limits, and player-context cleanup. They use fixed-delay rescheduling without catch-up, support self-cancellation, stop after callback failure, and run alongside one-shots by earliest due deadline. The chat-tools example adds repeatremind; see [repeating timers](EXTENSIONS.md#repeating-timers).

Callback performance diagnostics now report last/peak/total monotonic elapsed microseconds and completed invocations reaching the existing callback deadline through `extensions info`. Accounting includes protected callback/native work and failure cleanup, excludes staging and refused input, and does not change fault or timeout policy. Tests include slow native work, actual memory exhaustion, events/lifecycle/timers, and replacement/reset behavior; see [callback timing](EXTENSIONS.md#callback-timing-diagnostics).

Asset-specific recipient support queries now expose the native planner's shared primary/fallback selection, media-profile compatibility, scene/model switches, and exact model build allowlists through `scene_asset_support`. They are scoped to the calling package, require scene.media, and do not reserve slots or establish delivery/rendering. The lobby-decor example adds `decorcheck`; see [asset support queries](EXTENSIONS-SCENES.md#asset-support-queries).

The live-action adapter now shares explicit current local-membership eligibility across callers, messaging recipients, and lobby enumeration: registered lobby identity and actual client slot, supported local game-server role/version/phase, authentication/connection, and proxy exclusion. Account flags and extension grants remain separate; existing timer cancellation and primary-game observation gates are preserved. See [live membership checks](EXTENSIONS.md#live-membership-checks).

Native decorations now support optional bounded lifetimes through the fourth `scene_spawn` argument. Registry-owned monotonic deadlines expire independently of player-bound Lua timers, release slots/quotas, and use the existing per-recipient cleanup path. Persistent calls remain the default; successful replacement renews/cancels the deadline. The lobby-decor example adds `decortimed`, and administrator diagnostics show the configured duration. Visual timing remains a live acceptance gate; see [temporary decorations](EXTENSIONS-SCENES.md#temporary-decorations).

Permission-filtered command discovery is implemented through the separate `commands.read` grant and `extensions.commands()` snapshots. The optional command-help package provides bounded `exthelp` pages and `extusage` details across active extensions. Built-ins and invocation authorization remain unchanged; read-only observations cannot query live command state. See [player help](EXTENSIONS.md#command-discovery-and-player-help).

Per-package native scene diagnostics are implemented as `extensions scenes ID`: bounded value-only prepared-asset/fallback, object-location, and event-override snapshots. Inspection preserves active state through dry checks and rejected staging, requires existing shell access, and adds no Lua authority or client protocol. These records support live scene acceptance work without claiming delivery/rendering; see [administration](EXTENSIONS.md#administration-and-testing).

Joinable-quest `quest_join_acknowledged` observations now cover native BB ready and non-BB own-visibility boundaries, with distinct reasons, joinable metadata checks, and per-membership deduplication. They preserve existing packet/loading behavior and make no whole-quest/rendering readiness claim. See [joinable-quest acknowledgements](EXTENSIONS.md#joinable-quest-loading-acknowledgements).

Initial `quest_load_acknowledged` observations now expose the existing initial quest-loading acknowledgement with bounded copied client/game metadata and per-membership/quest deduplication. Native barrier/map ordering and clients without an acknowledgement are preserved; no late-join or whole-quest readiness is inferred. See [quest-load acknowledgement API](EXTENSIONS.md#initial-quest-load-acknowledgements).

Individual `game_join_ready` observations now use the existing native initial-load acknowledgement, current membership checks, and per-membership deduplication. They preserve upstream loading order and carry no player-action authority. They do not establish whole-game, quest, or rendering readiness. See [join readiness](EXTENSIONS.md#individual-game-join-readiness).

Quest assignment and server-map preparation observations are implemented using the primary-game tracker and metadata scope. They preserve original loading/barrier behavior and distinguish server preparation from client readiness or quest completion. See [quest observation API](EXTENSIONS.md#quest-assignment-and-server-map-preparation).

Primary-game opening/removal observations are implemented with stable manager tracking, bounded copied metadata, and metadata-only player scope; separately granted server announcements are available. Spectators, playback, patch-enabler games, proxies, and stale memberships are excluded. These events do not establish client or quest readiness, game completion, or combat results. See [game observation API](EXTENSIONS.md#primary-game-observations).

Package load/unload observations are implemented after staged validation and activation. Load callbacks may initialize granted storage; unload callbacks may read it and log without modifying the staged replacement. Checks and rejected candidates emit no lifecycle events. Configuration order controls activation and reverse-order retirement; faults cannot veto a committed switch. See [package lifecycle](EXTENSIONS.md#package-activation-and-unload).

Read-only current-lobby/player snapshots and an optional who command are implemented as a server-side API milestone. A separate lobby.read grant, per-query authorization, current-session membership filtering, copied tables, and bounded results preserve the adapter boundary. Automated contracts pass; real authenticated-client display/filtering checks remain open. See [lobby query API](EXTENSIONS.md#current-lobby-and-connected-players).

Per-package configuration is implemented as typed, bounded manifest settings with administrator overrides and a read-only Lua getter. Defaults, required values, integer/string limits, and unknown overrides validate before activation; dry checks preserve active settings. The shipped chat-tools cooldown demonstrates the API. This server-side milestone adds no protocol or client-identifier changes; see [settings setup](EXTENSIONS.md#per-package-settings).

The larger existing-B9 envelope for the Episode III PC recompilation is implemented as a controlled native-media addition: [PC lobby media](EXTENSIONS-PC-LOBBY-MEDIA.md). Following the client implementation confirmation, Teaserv recognizes the exact `E3 01` advertisement, and the `E3 02` modpack profile, in existing `9D`/`9E` login bytes after normal authentication. `E3 02` also starts the Caelum modpack handshake (`F2`) that gates extra card lobbies; see [Caelum modpacks](CAELUM_MODPACKS.md). Capability is connection-local and reset each login attempt; stock/unknown markers retain original limits. The account-based override is removed; configured and extension media can provide complete original-client fallback groups. This receive-only advertisement does not complete general Phase 3 negotiation or its live mixed-client acceptance gate. Preserve original-recipient filtering, exact-version/authentication/proxy/replay gates, and final size checks when integrating upstream updates, including when extensions are compiled out.

| Phase | Deliverables | Acceptance gate |
| --- | --- | --- |
| 0: Compatibility baseline | Source evidence ledger, exact-build capability matrix, legacy packet captures, core service extraction boundaries, fork branch layout, adapter integration map, and optional build/CI strategy | Unknown behavior stays marked unknown; native media and patch support are distinguished; upstream integration requirements are recorded before implementation. |
| 1: Server extensions MVP | Loader/manifests, Lua host, permissions, dynamic command registry, logging, timers, storage; shout/whisper examples | Works with original clients without new packets; unload and script failures leave core commands functional. |
| 2: Native scene support | Logical scene registry; event adapter; retail Episode 3 texture/model/animation backend; asset preparation and quotas | Verified assets display on actual target clients; limits, eight-slot contention, late join, and cleanup tested. Trial/non-Ep3 recipients receive no B9. |
| 3: Enhanced-client prototype | One chosen build, client-initiated negotiation, safe envelope parser, installed-asset cosmetic spawn/update/remove (partial: login markers `E3 01`/`E3 02`, the Caelum modpack handshake and modpack lobbies are implemented; no general envelope parser) | A mixed room works; captured traffic proves stock recipients receive no custom packets. |
| 4: Additional builds and assets | Per-build adapters, optional patch-assisted backend, more scenes, optional negotiated asset delivery | Each capability is enabled only after its build/scene tests pass. |
| 5: Operational hardening | Staged reload, migrations, diagnostics, documentation and example packages | Resource exhaustion, malformed input, reconnects, and extension faults are contained. |

Phase 1 integration points: `CMakeLists.txt` for an optional runtime dependency; new `ExtensionManager`, `LuaRuntime`, `ExtensionAPI`, and dynamic command registry modules; `ServerState` for ownership/executor integration; `DataIndex` for configuration; `ChatCommands` for dispatch; `ReceiveCommands`/`GameServer` for lifecycle hooks; `ShellCommands` for administration. Phase 2 adds `SceneService` and backend interfaces around the existing lobby/media pathways. Phase 3 adds capability state to `Client`, exact wire definitions in `CommandFormats`, and gated send/receive handlers.

Use meaningful tests for command conflicts and permissions, stale handles, runaway scripts, native-call budgets, timer cancellation, storage quotas, reload failure, and disconnect during callbacks. Protocol tests should cover downgrade, missing capabilities, opcode aliases, length limits, truncated payloads, late joins, scene transitions, and patched/unpatched mixed rooms. Test packet omission at the recipient boundary, not just in plugin logic. For B9, verify rendering separately from acknowledgement, texture/model ordering, strict size boundaries, slot exhaustion, and rebuilding remaining media after a delete-all. Validate shared gameplay state before allowing any interactive object backend.

The first useful release is commands and events on original clients. Arbitrary model rendering across all original versions is a separate client engineering effort; the API should expose that limitation honestly while allowing supported native backends immediately after validation.
