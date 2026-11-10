# Native lobby scenes

The second implementation milestone adds a shared scene registry, retail Episode 3 lobby media, and Newserv's existing live lobby-event adapter. It introduces no new packet type, client patch, interactive map object, or arbitrary model loader. (The Caelum modpack handshake and extra lobbies are separate; see [Caelum modpacks](CAELUM_MODPACKS.md).) Automated results establish server behavior; rendering and the documented eight-slot behavior still need real-client confirmation. Models/animations remain explicitly experimental because their internal structures are not fully validated.

## Enable the example

Build with `NEWSERV_ENABLE_EXTENSIONS=ON` and merge `system/extensions/scenes.config.example.json` into the full server configuration. Keep `ExperimentalModels` false. The example enables `lobby-decor`; retain a separate `chat-tools` entry if you want both packages.

```json
{
  "Extensions": {
    "Enabled": true,
    "Scenes": {"Enabled": true, "ExperimentalModels": false},
    "Packages": [
      {
        "id": "lobby-decor",
        "grants": ["commands", "events", "chat.reply", "scene.media", "scene.event"]
      }
    ]
  }
}
```

Use `reload config`, then `extensions reload`. Asset/script-only changes need only the latter command. The example registers a checker banner above the west teleporter when a retail Episode 3 client joins a CARD lobby. Repeated joins update the same key, without consuming another slot. Its deterministic 32×32 RGB checker BMP contains no game-disc or third-party artwork.

Administrators with the existing `CHANGE_EVENT` account flag can use `$decor show`, `$decor remove`, `$season 1`, and `$season release`. The configured command prefix applies. The join handler can recreate a manually removed banner on a later join; use `extensions disable lobby-decor` to unload it entirely. After reload, current members can use `decor show`; a successful reload removes old-generation objects and scripts recreate them when their callbacks next run.

## Lua API and permissions

| Operation | Behavior |
| --- | --- |
| `extensions.scene_capabilities()` | Live-context query returning `native_ep3_media`, `experimental_models`, `fixed_slots`, `lobby_events`, and `arbitrary_transforms` (always false). |
| `extensions.scene_asset_support(asset)` | Requires `scene.media`; return a fresh `{supported, reason, variant}` table for this package's asset and the caller's current recipient filters. No object registration or packet send. |
| `extensions.scene_spawn(key, asset, slot, lifetime_ms)` | Requires `scene.media`; register/replace this package's cosmetic object in the current lobby. The fourth argument is optional: nil/0 is persistent, or use an integer 50..3600000 for automatic expiry. Returns `ok, reason`. |
| `extensions.scene_remove(key)` | Requires `scene.media`; remove this package's key in the current lobby. Returns `ok, reason`. |
| `extensions.scene_event(number)` | Requires `scene.event` plus the account's `CHANGE_EVENT` flag; claim/update the lobby holiday event, returning `ok, reason`. Valid values are 0..14 except unused value 2. |
| `extensions.scene_event(nil)` | Release this package's event override and restore its previous value if an administrator has not intervened. Idempotent when nothing is owned. |

Command definitions may use `permission = "change-event"`. The event adapter independently checks the account flag even if a script exposes an unrestricted command; this applies to player-triggered event callbacks and timers too. There is no anonymous startup-authority API yet. Capabilities describe the backend for the current authorized session, not grants or asset availability. A read-only capability query returns all false for a departed or unauthorized player context, allowing membership handlers to omit unavailable work without faulting. Queries during script initialization remain errors.

Asset IDs resolve through the calling package's manifest, not file paths or pointers supplied by Lua. Ownership is `(package, lobby, key)` and comes from the host; changing the Lua context table cannot impersonate an owner. Keys follow the existing lowercase ASCII name rules and 64-byte limit.

Occupied slots, missing assets, exhausted scene quotas, and unsupported recipients return `false, reason`, without faulting the package. For mutating calls, wrong argument types, missing grants, or stale live contexts remain script errors. Success means registry acceptance, not client rendering. Unsupported experimental models are omitted per recipient, including their dependent texture.

### Asset support queries

```lua
local support = extensions.scene_asset_support("welcome")
if support.supported then
  extensions.log("Selected asset variant: " .. support.variant)
else
  extensions.log("Asset omitted: " .. support.reason)
end
```

This query uses the same variant selection and asset eligibility helper as the native media planner. It selects the complete original-client fallback for an original-profile recipient when configured, and the primary for a PC-profile recipient. It then checks retail Episode 3, connected/non-proxy state, canonical CARD scene eligibility, envelope compatibility, experimental-model enablement, and the selected variant's exact model build allowlist. It never downgrades a PC-profile recipient to an original fallback to bypass a model build mismatch.

The returned table contains only `supported` (boolean), `reason` (nil on success, otherwise a stable code below), and `variant` (`primary` or `fallback` for a known selected asset, otherwise nil). A known asset may report its selected variant even when another filter rejects it. The asset ID is looked up only in the calling package's prepared catalog; paths and other packages' asset namespaces cannot be queried. Each call returns independent scalar copies and reveals no payload, file path, client/account ID, or model pointer.

| Reason | Meaning |
| --- | --- |
| `context_unavailable` | No live authorized player scope, including read-only observation callbacks. The live adapter is not queried from those callbacks. |
| `scenes_disabled` | The administrator disabled the native scene backend. |
| `unknown_asset` | The package has no prepared asset with that ID, or the ID is invalid. |
| `unsupported_client` | Not a connected, non-proxy retail Episode 3 media recipient. |
| `unsupported_scene` | Outside the supported canonical CARD lobby scene. |
| `profile_limit` | The selected variant is incompatible with the recipient's original/PC envelope limits. |
| `models_disabled` | The selected asset contains a model and experimental models are disabled. |
| `build_not_allowed` | The selected model variant does not allow this exact client build. |

The call requires `scene.media`, takes exactly one string of at most 64 bytes, and uses the normal API/resource budgets. Wrong argument count/type/size or a missing grant is a script error. Initialization and migrations cannot query live asset support. Stale or read-only contexts return the unavailable table with the grant. The query does not refresh files, expire objects, mutate ownership, reserve a slot, create timers, advance recipient caches, or send packets.

`supported` means the asset passes these recipient filters. It does not check slot occupancy, object/group/catalog quotas, baseline conflicts, later state changes, model/GPU semantics, successful delivery, or rendering. A later `scene_spawn` can still fail, and a registered object can still be omitted by contention rules. Model/animation transport remains experimental. Administrators using the shipped lobby-decor package can run `$decorcheck checker` (substitute the configured prefix and asset ID) without changing decorations.

### Temporary decorations

```lua
local ok, reason = extensions.scene_spawn("temporary-banner", "checker", "teleporter-west", 30000)
```

The optional fourth argument sets a server-side lifetime in milliseconds, measured on the monotonic clock from a successful registration/replacement. Omit it, pass nil, or pass integer 0 for the existing persistent behavior. Nonzero lifetimes must be integers from 50 to 3600000 (one hour); wrong types, fractions, negative values, or out-of-range values are script errors before reaching the adapter. Existing ownership, placement, asset, permission, and quota checks still apply. Registration success does not mean a client displayed the object.

Expiration belongs to the registry and requires only `scene.media`, not `timers`. It uses no Lua timer slots and invokes no script callback. It still removes the object after its creating player leaves or loses permission; ordinary package fault/unload/reload cleanup can remove it sooner. The server checks due objects on the existing nominal 50 ms executor tick and before spawning or preparing a recipient snapshot. Cleanup marks affected lobbies for the existing coalesced reset/rebuild pathway, preserving upstream baseline media, surviving objects, recipient profiles/fallbacks, build allowlists, and final packet gates. Unsupported clients receive no expiration/reset packet. Executor scheduling, the normal 250 ms send coalescing, and network/client behavior mean this is not a precise visual deadline; a very short-lived object may never be transmitted or displayed.

A successful spawn with the same `(package, lobby, key)` replaces its deadline as well as its asset/location: another finite lifetime renews it from that call; nil/0 makes it persistent. Rejected replacements preserve the old object's deadline, unless it was already due and normal expiration removed it first. Deadlines live in the object, so deleting/reusing a key or slot cannot leave an old timer that later removes its replacement. Due objects release their slot and object quota when expired. Validated catalog replacement clears old objects/deadlines; checks and rejected catalog staging preserve active state. Nothing is persisted across restart.

The shipped `lobby-decor` example adds `$decortimed 30` for administrators with `CHANGE_EVENT`, using the configured prefix. It replaces the same `welcome` key used by `$decor show`, `$decor remove`, and the join handler. A later join or `$decor show` makes that key persistent again. `extensions scenes lobby-decor` reports each object's configured lifetime (or persistent); it is registry metadata, not a countdown or delivery confirmation.

## Availability

For administrator inspection, use `extensions scenes lobby-decor` (substitute the package ID). This reports the active prepared primary/fallback sizes and compatibility, model build allowlists, owned object locations, and event override records without sending packets or running scripts. It omits paths and payloads. Registry acceptance and size compatibility do not confirm recipient delivery or rendering; see [administration](EXTENSIONS.md#administration-and-testing).

| Client/scene | Native media | Live holiday events |
| --- | --- | --- |
| Retail GC Episode 3 CARD lobby | Texture banners; experimental models/animations with exact build allowlists | Yes |
| Episode 3 Trial Edition | No B9, including no reset packets | Existing event adapter |
| GC Episodes 1&2 / Xbox lobby | No B9 | Existing event adapter |
| BB lobby in normal game-server phase | No B9 | Existing event adapter |
| DC / PC v1/v2 / pre-retail prototypes | No B9 | No new live-event implementation |
| Games / proxies / replays | No extension lobby objects; proxies/replays never receive local scene updates | No changes through this API |

Both adapters require `Extensions.Scenes.Enabled`. Media transmission gates on exact `Version::GC_EP3` at the final recipient boundary, not `is_ep3()`, which includes Trial Edition. Native decoration placement is conservatively limited to canonical CARD lobbies (default blocks 16..20) without a client lobby-number override; custom/overflow layouts are not implemented, so the Caelum extra lobbies (numbers 20 and up) receive no native scene placement. No support-discovery probes are sent. Existing upstream baseline media can remain cached during games, as before; extension lobby objects are removed on transition. Membership hooks are not render-ready events. Native media uses the already-supported login/before-join preparation pathway, with script-created updates following the membership callback.

## Asset catalog

Add an `assets` dictionary to the package manifest alongside its existing fields:

```json
{
  "assets": {
    "checker": {"texture": "assets/checker.bmp"}
  }
}
```

The package must request and receive `scene.media`. Preparation runs during staging, using upstream's BMP/GVM encoder and PRS codec; callbacks cannot access files or compression functions. Paths, including resolved symlinks, must stay within the package directory. The complete catalog is validated before changing active state, so malformed asset replacements preserve the old set.

Texture inputs are `.bmp`, `.gvm`, or `.gvm.prs`. BMPs must be uncompressed positive-height RGB24/RGBA32, square powers of two from 8 to 256 pixels. Dimensions and pixel bounds are checked before decoder allocation. Generated GVMs use RGB565/RGB5A3, with host-assigned global indices starting at `0x100`.

Prepared GVMs use a deliberately conservative subset matching Newserv's encoder: metadata flags `0xF`, 1..16 sequentially indexed textures, square 8..256 dimensions, RGB565/RGB5A3, no mipmaps/palettes, consistent names/formats/dimensions/lengths, and no trailing data. Other native variants are rejected. Authors of model texture sets must ensure appropriate material references and globally compatible texture indices; semantic GPU-resource validation is not implemented.

By default every payload must compress to at most `0x3800` bytes and decompress to **strictly fewer than** `0x37000`; input reads are bounded to 512 KiB. An asset may explicitly select `"media_profile": "pc-port"` for `0x7BF0` compressed / 16 MiB decompressed preparation, with per-recipient selection based on the exact E3/01 or E3/02 login marker. See [larger PC lobby media](EXTENSIONS-PC-LOBBY-MEDIA.md) for configuration and the implemented client identification contract. PRS decompression enforces the selected output limit and rejects trailing bytes after a detected stop opcode. All accepted data is recompressed with upstream's compressor, which emits an explicit terminator; clean opcode-boundary EOF accepted by its decoder is normalized instead of transmitted unchanged. Poorly compressible images can be rejected despite valid dimensions. The GVM format/dimension subset above remains unchanged.

Capabilities also expose `extended_ep3_media`, `max_media_compressed_bytes`, and `max_media_decompressed_bytes`; budgets are zero for recipients without native media support. Mixed-profile snapshots preserve compatible baseline media and omit whole incompatible dependency groups.

There are at most 64 catalog assets and 1 MiB of compressed data, 128 scene objects total, and 32 objects per package across lobbies. Temporary objects count against the same quotas. Destroyed lobbies are pruned periodically. Objects in persistent/default lobbies survive departures until explicitly removed, expired, faulted, unloaded, or replaced; they are not persisted across restarts.

### Original-client fallback groups

A PC-profile asset can provide a complete smaller variant in `fallback`:

```json
{
  "assets": {
    "welcome": {
      "texture": "assets/large.gvm.prs",
      "media_profile": "pc-port",
      "fallback": {"texture": "assets/small.gvm.prs"}
    }
  }
}
```

Modified clients receive the primary variant; original clients receive `fallback` when supplied. Without a fallback, compatible primary groups still work on both profiles, and oversized groups are omitted for original clients. The script uses the same asset name and placement call for both clients.

A fallback requires its own texture and may supply its own model, animation, and `model_builds`. It must use the same placement category as its primary: texture-only replaces texture-only; model groups replace model groups. An animation is optional but requires that variant's model. Fallbacks always use original byte limits (`media_profile` may be omitted or `original`); nested fallbacks and PC-profile fallbacks are rejected. Both variants must pass staging before activation; failures preserve active packages. Paths and formats obey the same validation rules as primary assets, and both variants count toward the 1 MiB catalog budget while the pair consumes one logical asset/object/group slot.

Each selected model variant keeps its own exact-build allowlist and requires experimental models to be enabled. A fallback does not grant model support. Authors must make the fallback model and texture references consistent; it is a complete replacement, never a mixture of primary and fallback dependencies. Profile changes, removal, reload, and faults use the existing delete-all/rebuild cache handling. Visible rendering remains an acceptance test.

### Experimental native models

An asset may additionally declare:

```json
{
  "statue": {
    "texture": "assets/statue.gvm.prs",
    "model": "assets/statue.bml.prs",
    "animation": "assets/statue-motion.bml.prs",
    "model_builds": ["3SE0"]
  }
}
```

Animation is optional. Models require a texture; animations require both. `.bml`/`.bml.prs` means the **prepared B9 media payload**, following Newserv's saved-media naming, not an arbitrary disc archive or modern mesh. No OBJ/glTF/FBX conversion or native-model authoring tool is included.

Model/animation inspection checks byte budgets and documented aligned media tokens only. It does not validate nested pointers, material/animation semantics, rendering, or collision. Global `ExperimentalModels: true` and an asset-specific allowlist of 1..16 exact retail Episode 3 build IDs are both necessary to send them. There are no wildcard allowlists. Use only administrator-prepared, target-client-tested, cosmetic assets. Synthetic model bytes in tests are parser fixtures, never renderable samples; the example ships no model/animation binary.

## Placement and lifecycle

Texture slots are `counter-south`, `counter-west`, `counter-north`, `counter-east`, `teleporter-west`, `teleporter-east`, `banner-south`, `banner-north`, and `floor-banners`. Model slots are `south-platform`, `jukebox`, `battle-west`, `battle-east`, `battle-southeast`, `battle-southwest`, `counter-northwest`, `battle-wall`, and `inside-counter`. Some flags represent several built-in placements. Texture-only assets cannot use model slots, where they would appear sideways/partly underground. Arbitrary transforms and interactive entities are unavailable.

Upstream `Episode3LobbyBanners` and extensions share the documented eight-group native limit. A matching texture/model/animation set counts as one group, following source comments; actual client slot behavior still needs confirmation. Upstream slots and other owners cannot be overwritten. Updating an existing owned key consumes no extra slot. Later upstream slot/capacity changes take priority, with excess/conflicting extension groups omitted in deterministic owner/key order; their logical registrations can reappear when the conflict is removed.

B9 type 4 deletes **all** media, so removal/replacement sends a reset followed by the complete desired snapshot, preserving baseline banners and surviving packages' objects. Textures precede dependent models/animations. Unsupported recipients receive no reset or other B9. Unchanged snapshots are not resent; late joiners and scene transitions get their appropriate view. A partial send invalidates the cache and retries a complete snapshot later. A B9 acknowledgement is not rendering confirmation; clients also acknowledge ignored content.

Script changes coalesce by lobby and publish at most once per 250 ms on the host's 50 ms tick. Initial joins and administrative cleanup may reconcile immediately. Every five seconds the host reconciles changed upstream media and prunes destroyed lobbies. An invalid/ambiguous baseline prevents reconstruction rather than erasing it; fix that configuration before expecting cleanup to complete.

Unload, fault quarantine, and successful reload remove owned media. Holiday ownership is exclusive per lobby; cleanup restores the prior event only if the current event equals the owner's last applied value, preserving intervening administrator changes. A manual assignment of that exact same value is indistinguishable from no change; event-generation tracking is future work.

## Required client verification

Tests cover registry ownership, quotas, dependency ordering, preservation, late joins, transitions, unsupported-recipient omission, actual BMP/GVM/PRS preparation, strict expansion limits, and Lua grants/results. See [validation notes](EXTENSIONS-VALIDATION.md) for actual local outcomes.

Before deployment, verify the example with retail Episode 3 clients, baseline banners, two competing extensions, join/leave, game entry/exit, removal, unload, and failed/successful reload. Inspect mixed-version traffic to confirm Trial Edition/non-Ep3 receive no B9. Models/animations additionally require exact-build rendering/memory and cosmetic-only checks with real prepared assets. This milestone does not establish arbitrary 3D model compatibility across PSO versions.
