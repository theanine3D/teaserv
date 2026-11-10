# Custom lobby content: implementation plan

Date: 2026-10-23

Status: the GLB/HTTPS scene design below is proposed future work. A narrower mechanism is already implemented and live-tested (2026-11-09): Caelum modpack offers (`E3 02` login profile and the `F2` handshake) and extra card lobbies 20-63 that come from modpacks, listed and joinable only after the handshake. See [Caelum modpacks](CAELUM_MODPACKS.md). That mechanism is separate from this design and does not close its acceptance gates.

This plan integrates the custom-content proposal from the Caelum (PSO Episode III PC) project with the subsequent feasibility review. The initial target is Teaserv plus the modified Episode 3 PC client. Other clients require their own implementation and acceptance checks; the existing GameCube client does not acquire these capabilities merely by connecting to Teaserv.

## 1. Objectives and boundaries

Allow administrators to customize stock lobbies and define additional custom lobbies using remotely hosted models, textures, animation, and music. Use HTTPS for asset delivery so asset size is independent of game-protocol packet size. Preserve ordinary Newserv interoperability for the modified client and safe behavior for original clients.

Provide three levels of customization:

| Lobby/content mode | Original clients | Capable modified clients |
| --- | --- | --- |
| Stock lobby with optional cosmetic additions | Normal stock presentation and movement; no custom packets. | Additional visual models and optional replacement music. |
| Restricted lobby based on a stock scene | Admission refused unless all required capabilities and content are available. | May include collision or changes to walkable/interactive space. |
| Fully custom lobby | Not listed or joinable. | Explicit custom environment, collision, spawn/camera/interaction definitions, and optional decorations/music. |

The existing native seasonal-decoration/B9 system remains available. The new renderer/backend does not consume native decoration slots. Scene replacement policy must specify whether native decorations are meaningful in that scene; a fully custom environment must not accidentally retain invisible stock objects or collision.

Initial non-goals: arbitrary gameplay scripts downloaded to clients, custom combat rules, user-uploaded assets, full glTF feature coverage, moving collision, progressive geometry streaming, and immediate support for every PSO version.

## 2. Existing implementation and important constraints

- Teaserv has Lua packages, grants, lifecycle/game/quest observations, a native scene registry, and server-wide announcements. These can support administration and later content scripting, but do not implement GLB rendering or a general enhanced-client scene protocol.
- The exact `E3 01` login advertisement currently means larger native Episode 3 B9 media support. It must retain that meaning. It does not establish JSON, HTTPS, GLB, custom audio, collision, or custom-lobby support.
- Native media uses GVM/Ninja/animation payloads and fixed placement locations. Arbitrary GLB placement requires a separate client rendering path or an explicit conversion into a supported existing rendering path.
- `Channel::send_command` enforces a whole-command size limit of `0x7C00`. Moving assets to HTTPS does not remove this limit from control messages.
- Original lobby menus are not arbitrary-length directories. Newserv documents expected counts of 10/15/20 by client version, a fixed receive array, and overflow risks. Extra custom lobbies must not simply be appended to a stock client's `83` list. Episode 3's stock menu expects 20 entries. (Teaserv appends its extra lobbies only for clients that completed the Caelum modpack handshake; stock clients still receive exactly 20 entries.)
- Hiding a menu entry does not enforce admission. Several membership paths exist, and `Lobby::add_client` is not a complete higher-level eligibility check.
- Existing native-media, mixed-client, and scene-rendering acceptance gates remain open. This plan does not close them.

Relevant local references:

- [Existing extensions plan](EXTENSIONS-PLAN.md)
- [Native scene backend and current limitations](EXTENSIONS-SCENES.md)
- [Existing client advertisement contract](EP3-CLIENT-CAPABILITY-HANDOFF.md)
- [Existing PC lobby-media support](EXTENSIONS-PC-LOBBY-MEDIA.md)
- [Integration map](EXTENSIONS-INTEGRATION.md)
- [Recorded verification and open acceptance gates](EXTENSIONS-VALIDATION.md)

Source locations to audit again during implementation: `src/Channel.cc`, `src/CommandFormats.hh`, `src/SendCommands.cc::send_lobby_list`, lobby join senders, `src/ReceiveCommands.cc::on_84`, Guild Card/choice-search handlers, `src/ServerState.cc` membership methods, and `src/Lobby.cc` membership/admission methods. These are inspection targets, not a complete list of required changes.

## 3. Client/server responsibility split

Teaserv owns administrator configuration, approved asset IDs, scene/object state, capability negotiation, lobby admission, revisions, and per-recipient control messages. External HTTPS hosting/CDNs provide asset bytes. Teaserv need not act as an asset proxy or download arbitrary URLs during a player join.

The modified client owns asynchronous downloads, validation, caching, decoding, rendering, collision integration, audio playback, readiness reporting, and cleanup. GLB parsing alone does not provide rendering or collision in PSO.

Keep the server implementation in isolated content services with narrow adapters to upstream networking and lobby code. Reuse the extension host's semantic scene boundary instead of exposing packet layouts or upstream pointers to Lua. Preserve both extensions-enabled and disabled builds. A configuration-driven content service should not require Lua; its build/configuration switch and relationship to existing scene services must be explicit before implementation.

No changes to the separate client repository are implied by saving this plan. Client/server contracts should be written as a handoff before coordinated implementation.

## 4. Capability negotiation and old-server compatibility

Define a versioned capability exchange. Example logical capabilities, with final names to be agreed:

- Versioned scene manifests and object snapshots.
- A particular GLB rendering profile.
- Custom audio and its supported codecs.
- Static collision.
- Additional/custom lobby directory and full scene replacement.

Negotiate supported features and limits, not merely a boolean modified-client flag. Capabilities describe protocol/format support and never confer administrator permissions or prove that a client is honest.

Bootstrap rules:

1. Preserve the existing `E3/01` native-media advertisement unchanged.
2. Audit an additional backward-compatible advertisement in existing login fields, or another verified bootstrap mechanism, before choosing its bytes. Do not assume unused fields are safe without checking their readers.
3. Teaserv responds only to clients advertising the new bootstrap. Exchange detailed capabilities only after server support is established.
4. A modified client connecting to ordinary Newserv stays in legacy mode. It must not send an unsolicited unknown handshake packet; upstream Newserv can disconnect on unimplemented incoming commands.
5. Reset negotiation on each connection/login attempt and server transfer. Proxy/replay support is initially excluded.
6. Send every custom control message directly to a negotiated recipient. Never use generic lobby forwarding for these messages.

Already in use for modpack lobbies: the `E3 02` login profile as the bootstrap and `F2` (offer / accept / decline) as the control command. A GLB scene feature must either build on them or audit its own choice.

Select opcodes only after auditing exact command dispatch, aliases, build differences, and phase restrictions. Specify byte order, framing, alignment, maximum lengths, and behavior for unknown optional versus required messages. No opcode is reserved by this document.

## 5. Configuration: definitions, assets, and instances

Use a dedicated versioned lobby-content configuration, referenced by the normal server configuration. Preserve upstream stock defaults; allow overrides of existing lobby definitions without requiring administrators to copy all stock lobby definitions into a new file.

Separate:

- **Lobby definition:** stable logical ID, name, stock/custom mode, base scene if applicable, admission policy, required capabilities, scene revision, and environment settings.
- **Asset:** stable administrator-assigned ID, approved HTTPS URL, content hash, byte size, format/profile, and applicable limits/metadata.
- **Object instance:** stable instance ID, asset reference, transform, animation selection, collision policy, and optional lifetime/extension ownership.
- **Environment root:** explicit `root_scene` reference. Never infer this from array order or from the first listed GLB.
- **Music:** asset reference, volume, loop range, fallback, and jukebox override policy.

Stable logical lobby/scene IDs must be distinct from reusable runtime lobby IDs and legacy scene-number fields. Do not encode arbitrary new scene IDs by increasing a stock lobby-number byte.

Illustrative shape only; field names and wire schema are not finalized:

```json
{
  "schema_version": 1,
  "assets": {
    "welcome-statue": {
      "url": "https://cdn.example.invalid/pso/statue.glb",
      "sha256": "<64 hexadecimal characters>",
      "bytes": 123456,
      "profile": "teaserv-glb-static-v1"
    }
  },
  "lobbies": {
    "card-lobby-cosmetics": {
      "mode": "stock_overlay",
      "stock_lobby": 16,
      "revision": 1,
      "objects": [
        {
          "id": "welcome",
          "asset": "welcome-statue",
          "position": [0, 0, 0],
          "rotation_xyzw": [0, 0, 0, 1],
          "scale": [1, 1, 1],
          "collision": "none"
        }
      ]
    }
  }
}
```

The example URL/hash/size are placeholders, not a usable asset. Location and scale must be chosen after coordinate conversion and walkability checks. A visual object in a mixed-client lobby must not obscure required interactions or materially obstruct normal play even if its collision is disabled.

Validate configuration and all local metadata before activation. Reject duplicate IDs, unknown asset references, invalid transforms, unsupported required profiles, and inconsistent lobby requirements. Preserve the active configuration after rejected staging. Do not block the server executor on remote asset checks; use an offline publishing/validation step and bounded readiness handling.

## 6. Manifest and scene protocol

Use a small bounded game-protocol control message that references a downloadable manifest by HTTPS URL and expected hash, together with protocol/schema version and scene revision/generation. Large asset lists belong in the manifest, not a potentially oversized game packet.

JSON is suitable for manifests. Encode it once as UTF-8 JSON; do not add unnecessary nested stringification. Apply maximum byte size, nesting depth, entry counts, URL lengths, and strict field/type validation.

A scene snapshot identifies the scene generation and revision and describes required assets, optional assets, root/environment data, and object instances. Later updates use stable object IDs, ownership, revisions, and explicit spawn/update/remove semantics. Define duplicate/stale/out-of-order handling and atomic snapshot completion.

State must distinguish server intent, client receipt, asset readiness, and installed scene readiness. A native packet acknowledgement or completed HTTP request is not proof of rendering or collision installation.

Keep the manifest's source identity, content hash, scene identity, and active connection association together. A response from an earlier lobby or connection must not install content into the current scene.

## 7. GLB content profile and renderer prototype

GLB is the initial interchange format because it can package meshes, materials, textures, and animation. Require self-contained assets: GLB can still reference external resources, so the loader/publishing tool must reject unapproved external buffer/image dependencies rather than automatically fetching them.

Start with a deliberately narrow profile: static triangle meshes, embedded texture formats supported by the chosen decoder, limited material behavior, and bounded scene hierarchy. Reject unsupported required glTF extensions. Add skins, morph targets, compression extensions, and richer materials only with independent tests and explicit profile versions/capabilities.

Before selecting the final renderer architecture, prove one textured model in a stock lobby with:

- Correct view/projection alignment, depth testing, material appearance, and transparency behavior.
- A measured mapping between glTF coordinates and PSO world coordinates, including units, handedness, winding, normals, and rotations.
- Safe ownership of CPU/GPU resources and complete cleanup on lobby transitions.
- Measured frame time and memory use on the intended minimum PC configuration.

glTF uses a right-handed coordinate system with Y-up and distances in meters. Do not assume PSO uses the same scale or orientation. Choose one documented authoring-to-game conversion and apply it consistently to geometry, instances, collision, and spawns.

The client agent must choose and validate either conversion into the existing PSO rendering path or a host-side renderer integrated with it. Do not assume a separate overlay automatically shares depth, lighting, or resource lifetime with the existing engine.

Root environment scenes are static initially. Decorations select an explicit animation clip and loop policy; do not automatically play every clip. Define initial pose, playback speed, start/restart behavior, and synchronization needs. Animation must not silently alter authoritative collision.

## 8. Collision and full scene replacement

The proposed Blender `collision` object/material convention can be supported by an export/validation tool. Translate it into explicit node metadata or a separately identified collision asset. Define exactly which geometry is selected; a single material named collision must not ambiguously classify a multi-material mesh.

Collision meshes are hidden from rendering, simplified, static initially, and bounded by triangle/complexity limits. Validate coordinates and indices, build an appropriate acceleration structure, and handle supported transforms consistently. Animated, skinned, deforming, and moving collision are deferred.

Custom collision or changed movement requires a capability-restricted lobby. Every admitted participant must install the same required scene/collision revision before movement is enabled. Continue normal server-side movement and packet validation; client capability/readiness claims are not a security authority.

A full environment definition also needs:

- Walkable floor, safe player spawn positions/orientations, and out-of-bounds/fall recovery.
- Camera behavior/bounds, lighting/background, and supported interaction definitions.
- Exit/teleporter locations and any retained NPC, terminal, or jukebox behavior.
- Explicit disabling/replacement of stock rendering, collision, and triggers; hiding geometry alone is insufficient.
- An audit of native movement, visibility, and interaction assumptions tied to stock scenes.

Keep the explicit root transform at identity/origin after the defined authoring conversion. Additional instances have their own transforms. A whole scene need not be a single mesh, but its environment role and required dependencies must be unambiguous.

## 9. Downloads, cache, and scene readiness

Initially download and validate complete GLB assets before installation. Progressive geometry streaming is unnecessary for the first implementation. Run download/parse work away from the receive/render loop; commit GPU and collision resources on the appropriate owning thread.

Bound simultaneous requests, download time, retries, individual/total bytes, cache size, decode work, CPU/GPU memory, and installation time. Use immutable content hashes as cache keys, temporary files and atomic publication for completed cache entries, and bounded eviction. Do not use untrusted URLs as filesystem paths.

The required-scene transition is:

1. Server offers an eligible scene and its required revision/manifest.
2. Client downloads, validates, prepares required render/collision resources, and reports readiness tied to that connection/offer/revision.
3. Server revalidates eligibility, capacity, current revision, and the pending transition before committing the membership/scene switch.
4. Client installs the prepared scene safely and only then allows normal movement. Define the final acknowledgement and failure recovery during protocol design.

Do not leave a player admitted and walking on an absent or partially installed custom floor. Offers/reservations require bounded lifetimes, capacity rules, cancellation, and protection against stale/duplicate readiness reports.

Optional cosmetic failure leaves a usable stock scene; optional music failure restores stock music. Missing required custom content aborts admission or returns the player to a safe stock lobby. Permission/capability revocation, disconnect, timeout, and lobby removal cancel pending transitions.

Tag asynchronous jobs with connection/scene generations. Leaving lobby A cancels or invalidates its pending installations even if a shared cached download finishes later. Teardown releases rendering, collision, animation, and audio ownership without freeing resources still used by another scene/job.

Initially defer required geometry/collision changes until the next entry or an explicit coordinated reload. Do not replace occupied collision casually; either wait for participants to prepare the same revision or move them safely elsewhere.

## 10. Music and jukebox integration

Use one initial codec/profile instead of supporting many decoders immediately. Ogg Opus is the preferred candidate for the PC implementation; MP3 is acceptable if an appropriate decoder is already integrated and tested. Make the final choice after examining the existing client mixer/decoder path and verifying CPU cost, seeking, and loop behavior.

Specify volume, loop start/end in a defined sample timebase, and encoder-delay/trimming behavior. Correct compressed-audio sample positioning matters for seamless loops. Start with cached complete downloads; bounded audio streaming can follow if needed.

Playback priority is: jukebox override, then custom lobby music, then stock lobby music. When a jukebox selection expires, restore the current lobby's intended music, not a stale track from a previous scene. Define whether custom music resumes or restarts. Leaving cancels the old lobby's playback/requests. Decoder or download failure falls back cleanly.

This plan does not add custom jukebox song listings or guarantee sample-synchronized music between clients.

## 11. Security and trust model

Only administrator-approved configuration supplies remote asset locations. Extensions refer to approved asset IDs through a bounded service; they do not get arbitrary URL download, packet injection, or client code execution APIs.

Custom scene/control messages must be server-authored and accepted only in the negotiated server-session state. Reject client attempts to submit/relay these controls. All incoming game packets reach clients through the server, including some forwarded player traffic; checking that bytes arrived on the server socket is not by itself sufficient.

Require HTTPS certificate validation. Restrict schemes and approved hosts/ports; disable redirects initially or fully revalidate every redirect. Validate resolved destinations and prevent unwanted loopback/private/link-local access in public-CDN mode, with any trusted LAN-development exception explicitly configured. Never send game-account credentials/cookies to asset hosts or use embedded URL credentials. Apply equivalent policy to any future server-side publishing fetcher.

Validate expected byte count and cryptographic content hash. A hash is only as trustworthy as the manifest that supplies it. For protection against server impersonation or tampering with the legacy game connection, use signed manifests with keys trusted independently, or an equivalent authenticated control channel. Define key enrollment, server identity, key rotation, and development behavior before production deployment. A public key delivered solely over the same unauthenticated connection does not establish trust.

Encryption of JSON is not a substitute for authentication or parser safety. Even authenticated content can be malformed or excessively expensive. Validate container bounds, meshes, indices, transforms, animation data, texture dimensions/decoded memory, audio duration/channels, collision complexity, and aggregate resource use before installation. Do not trust file extensions, MIME types, claimed sizes, or URL allowlists as proof of safe decoding. Fuzz parsers and exercise resource-exhaustion cases; consider process isolation for decoding untrusted content where practical.

Content is data only. Do not load native libraries, scripts, shaders supplied as arbitrary executable code, or browser/HTML content through this mechanism. Asset budgets and a client download opt-out remain available; opting out makes required-content lobbies unavailable but must leave stock play usable.

## 12. Lobby directory and centralized admission

Implemented for modpack lobbies: per-recipient directory filtering (the `83` list) and a join check in `Lobby::join_error_for_client`, used by direct selection (`84`) and the preferred-lobby path. Required-scene readiness, GLB manifests and signing are still open.

Capable clients receive a versioned custom-lobby directory/menu. Preserve the stock `83` list's count/layout for original clients; filtering is performed per recipient. Audit client-side menu allocations and names/count bounds too.

Create one central eligibility/admission policy for both required capabilities and required scene readiness. Apply it before membership mutation, with no ordinary bypass for administrative moves. Enforce it independently of menu visibility and search output.

Audit at least:

- Direct lobby selection and custom-directory joins.
- Guild Card searches, choice/player searches, and resulting join/teleport actions.
- Login preferred-lobby selection, automatic lobby selection, return from games, and fallback/overflow handling.
- Administrative relocation and future extension-driven moves.
- Reconnects/server transfers, stale menu selections, direct fabricated join requests, and capability resets.

Search results must not provide an actionable incompatible destination. Choose a consistent unavailable/omitted result policy, then still reject attempts at the admission boundary. Unsupported clients must never receive a join packet selecting an invalid/custom scene type.

Use stable custom definition IDs with separate runtime memberships and an explicit mapping to any valid legacy scene framing required by the modified client's state machinery. Exact field mapping requires a client/server protocol audit; do not assume a new lobby number is sufficient.

## 13. Extension integration and administration

Expose semantic operations for registered assets and owned instances: support query, spawn, transform/update, remove, and optional music selection. Final Lua names/grants remain to be designed; examples in this document are not new API promises.

Separate cosmetic placement authority from collision/full-environment authority. Commands need account permissions in addition to package grants; automated events use only deliberately granted service authority. Unsupported/temporarily unavailable capabilities return explicit normal refusal results. The server, not each script, handles per-recipient omission or supported substitution.

Reuse ownership, staged validation, quotas, scene generations, and unload/fault cleanup principles from the existing native scene service. Implement backend-specific validation rather than making B9 payloads or arbitrary network packets available to scripts. Required collision/environment changes use controlled scene transitions, never an ordinary cosmetic spawn call.

Provide administrator preflight validation, prepared-asset/scene diagnostics, negotiated-support visibility, and bounded error reporting without exposing credentials. Retain previous validated configuration after staging failure. Explain whether retirement of an extension removes optional objects or invalidates a required scene; required-world ownership must not disappear underneath occupants on an ordinary script fault.

## 14. Implementation sequence and acceptance gates

| Milestone | Work | Acceptance gate |
| --- | --- | --- |
| 0: Contract and renderer experiment | Audit client renderer/collision/audio hooks; define bootstrap, format subset, coordinates, budgets, trust policy, and wire schema. Prove one local static GLB first. | Correct alignment/depth and cleanup with measured resource use; no claim of general GLB support. Bootstrap/old-server tests agreed before new network traffic. |
| 1: Optional static decoration | HTTPS manifest/asset download, validation/cache, negotiated snapshot, one non-colliding object in a stock lobby. | Modified client renders it; original client remains usable in the same lobby and receives no custom control packets. Test timeout, missing/corrupt asset, cancellation, reconnect, and stale completion. |
| 2: Audio and controlled animation | Selected music codec, explicit loops, jukebox precedence, one supported animation subset. | Audio fallback/restore and scene cleanup work; bounded animation/memory/frame time; unsupported features fail safely. |
| 3: Restricted collision lobby | Central admission, custom directory, static collision, prepare/readiness/commit transitions, spawn/recovery. | All admitted clients share required content revision; every incompatible join route is denied; slow/failing clients stay safe; no partial-floor movement. |
| 4: Complete custom environment | Root scene, stock state replacement, camera/lighting/interactions/exits, native assumptions audited. | No invisible stock collision/triggers; stable movement and game-return/reconnect paths; reliable recovery when assets or revisions fail. |
| 5: Scripted lifecycle and hardening | Owned dynamic optional instances, bounded updates, diagnostics, publishing validation, reload/fault handling, broader supported profiles. | Late joiners obtain coherent state; stale updates cannot resurrect objects; faults/reloads preserve valid required worlds; both server build modes and upstream contracts pass. |

Capability negotiation and the minimal protocol required for milestone 1 are part of that milestone's implementation, not postponed until custom lobbies. No milestone is complete solely because the server compiles or receives an acknowledgement.

Test with the modified Episode 3 PC client first, then an unmodified Episode 3 client in the same stock lobby. Add BB/other clients only for omission/admission regression checks until they gain a separately validated content implementation. Record exact builds, packet captures, scene/asset hashes, performance measurements, and observed rendering.

## 15. Maintaining upstream compatibility

Confine upstream changes to intentional configuration, capability, directory, admission, and send/receive adapters. Keep client-format handling, asset policy, scene state, and extension adapters in dedicated modules. Keep native B9 behavior and limits independent of the new backend.

Update [the integration map](EXTENSIONS-INTEGRATION.md) and [upstream watch inventory](EXTENSIONS-UPSTREAM-WATCH.json) when adding hooks. Follow the [upstream review procedure](EXTENSIONS-UPSTREAM.md), including full-diff/callee inspection and enabled/disabled builds. A clean merge does not establish protocol or scene compatibility.

The repository owner continues to handle project commits, pushes, and pull requests manually. Future implementation authorization must not be inferred as permission to modify the remote repository or the separate client project.

## 16. Decisions to resolve before implementation

1. Exact modified-client repository/build, renderer integration strategy, and the minimum supported PC hardware.
2. A backward-compatible bootstrap advertisement and audited control packet framing/opcodes. (Modpack lobbies use `E3 02` and `F2`; a GLB feature may reuse or extend them.)
3. Initial GLB/material/animation profile and measured resource ceilings.
4. Coordinate conversion, collision representation, movement integration, and environment metadata contract.
5. Initial audio codec and integration with the existing jukebox/mixer.
6. Manifest trust/key enrollment and approved hosting policy, including explicit local development behavior.
7. The pending-transition/capacity protocol and final readiness/commit acknowledgements.
8. Optional server build/configuration switches and final semantic extension APIs/grants.

These are design tasks for milestone 0, not reasons to assume unsupported behavior or enable partially validated custom lobbies.

## References

- [Khronos glTF 2.0 specification](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html): GLB packaging and external references, coordinate conventions, animation, and application metadata.
- [Ogg Opus specification, RFC 7845](https://www.rfc-editor.org/rfc/rfc7845.html): sample positioning, pre-skip, and seeking considerations.
- [OWASP URL/destination validation guidance](https://cheatsheetseries.owasp.org/cheatsheets/Server_Side_Request_Forgery_Prevention_Cheat_Sheet.html): redirect/destination restrictions applicable to a downloader handling remotely supplied URLs. Client-side downloading is not itself a server-side request, but the network-destination validation principles are relevant.
