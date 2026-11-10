# Episode III client capability marker: client implementation handoff

Prepared 2026-10-22 for the developer/agent working on the modified Episode III PC client.

## Objective and current status

Keep the modified client compatible with unmodified Newserv while allowing Teaserv to recognize its larger existing-B9 media receive limits automatically. Retain the original GameCube Episode III identity, credentials, packet sizes, and login flow. Advertise a narrowly defined receive capability in two existing unused login bytes.

**The E3/01 contract is now implemented on both sides.** The client confirmation in the Caelum project's `pso_ep3_lobby_media.md` (2026-10-22) reports both `9E` builders emitting the pair after serialization and before encryption. Teaserv now recognizes it after normal authentication and before initial media. Automatic Teaserv selection was exercised with a real Caelum client on 2026-11-09 (login with `E3 02`, the modpack handshake, an extra card lobby). Login against unmodified Newserv, mixed-client captures, and B9 media rendering remain unverified.

Update 2026-11-10: a second profile, `E3 02`, is now defined. It means everything `E3 01` means and also advertises Caelum modpack support; see [Caelum modpacks](CAELUM_MODPACKS.md). The client sends `E3 02` when its mod API is present and `E3 01` otherwise.

Teaserv retains original/PC media profiles, configured-banner classification, extension asset preparation, per-recipient filtering, and final B9 size validation. The account-ID override has been removed. Only the connection marker selects PC limits; optional complete original-client fallback groups provide smaller substitute assets. Remove `Episode3ExtendedMediaTestAccounts` from old local configurations.

This handoff follows `NEWSERV_LOBBY_MEDIA_HANDOFF.md` dated 2026-10-21. The client's existing larger-media implementation was inspected in the Caelum project's `pso_recomp/port/net/hand_media.c`.

The supplied original upstream baseline is newserv commit `46ab6f106618060157b8fc99e840bb6be4f15c11`. Compatibility statements below refer to that inspected source, not every historic or future server implementation.

## Wire contract: profile E3/01 (implemented)

Use the existing `unused3[2]` field in the common GameCube login payload, defined by `C_Login_DC_PC_GC_9D` and inherited by `C_Login_PC_GC_9E`.

| Field | Proposed value |
| --- | --- |
| First byte, payload offset `0x16` | `0xE3` |
| Second byte, payload offset `0x17` | `0x01` |
| Same offsets in the complete plaintext packet, including its four-byte header | `0x1A`, `0x1B` |
| Meaning of the exact pair `E3 01` | Retail Episode III modified client supports the B9 receive profile defined below |
| Normal original-client value | `00 00` |

Treat these as **two bytes**, not a native-endian integer. The first byte identifies this proposed Episode III capability family; the second selects the complete version-1 profile. This is a project-local convention, not an officially reserved PSO protocol value. No earlier example marker from discussion is an implemented contract; implementations use this document's exact pairs.

Version 1 is a fixed profile, not a general "modified client" privilege or an extensible bit mask. Unknown pairs, including `E3 00`, `E3 03`, `00 01`, and `00 00`, must select original limits. `E3 02` is the modpack profile: it implies the E3/01 receive limits and also advertises Caelum modpack support (see [Caelum modpacks](CAELUM_MODPACKS.md)). A higher version is not implicitly compatible. Future capabilities require an explicitly defined contract.

Profile E3/01 promises all of the following:

| Receive property | Requirement |
| --- | --- |
| Protocol | Existing B9, retail GameCube Episode III layout |
| Maximum compressed payload | `0x7BF0` = 31,728 bytes, inclusive |
| Maximum complete packet | `0x7C00` = 31,744 bytes, inclusive |
| Maximum decompressed payload | `0x1000000` = 16,777,216 bytes, inclusive |
| Original safe-path boundary | Decompressed bytes strictly below `0x37000`; the PC heap path handles the boundary and above |
| Media types | Existing 1 texture, 2 model, 3 animation, 4 delete-all semantics; existing model safety gates still apply |
| Capacity | Existing eight slots per type; no capacity expansion advertised |

Only advertise this profile when both larger-packet handling and bounded larger-output decompression are active in the running client build. A build/runtime option disabling either enhancement must emit `00 00` instead. This marker does not assert that arbitrary models/animations have been validated or rendered correctly.

## Why this can remain compatible with original Newserv

The inspected upstream login handler `on_9D_9E` validates the existing packet shapes, reads the normal login fields, and does not inspect `unused3`. The protocol definition describes these two bytes as unused and normally zero. Changing only those bytes preserves the length and every field upstream uses for identity and authentication.

| Client | Server | Expected behavior |
| --- | --- | --- |
| Original client, `00 00` | Original Newserv | Existing login and media behavior |
| Modified client, `E3 01` | Original Newserv | Existing login; marker ignored; original server sends its existing media |
| Original client, `00 00` | Teaserv with marker support | Original media limits |
| Modified client, `E3 01` | Teaserv with marker support | PC media limits for this connection |

The client does not need a server acknowledgement for this receive-only advertisement: it already accepts small media, and an original server can continue sending small media. The `E3 01`/`E3 02` marker itself must not cause the client to send new commands, larger uploads, or any server-specific traffic. Only a server that receives `E3 02` may then offer the Caelum modpack handshake (`F2`), and the client sends `F2` only in answer to that offer; stock Newserv never sees a new command. Future bidirectional extensions would need separate server capability discovery.

Source inspection supports this design; a real modified-client login against the unmodified baseline remains an acceptance test. Other private servers may validate otherwise-unused fields and require their own compatibility tests. If a user needs an escape hatch for such a server, an explicit per-server client option can suppress the marker while retaining the client's larger receive implementation. No speculative packet probe or automatic disconnect/retry scheme is needed.

## Client implementation scope

1. Locate construction/transmission of the actual retail GC Episode III `0x9E` login packet, including initial login and subsequent ship/block/game-server connections. Cover `0x9D` as well if this client actually emits it. Do not add a new login command or force a different existing command.
2. Identify the common payload's `unused3` bytes. Modify exactly those bytes, preserving all other fields, including `sub_version`, language, credentials, hardware ID, `is_extended`, and server-provided client configuration/security data.
3. Write `E3 01` for a build with the full profile enabled; explicitly write `00 00` when disabled. Initialize each newly constructed packet rather than relying on stale memory or a previous login buffer.
4. Prefer a packet-builder change if available. If using a send hook, limit it to known GC Episode III login commands/shapes and modify the final plaintext bytes after relevant serialization/byte swapping but before encryption. Do not write byte offsets into encrypted packets, generic non-login traffic, or an outer transport frame. Verify where the actual four-byte PSO header begins.
5. Keep packet lengths and header flags unchanged. The existing four-byte GC header means payload `0x16` maps to complete-packet `0x1A`; confusing these offsets would corrupt other login fields.
6. Re-advertise on every new server connection. Do not persist the marker in saves, credentials, or the opaque client configuration returned by the server. Continuing to echo that configuration unchanged preserves original-server behavior.
7. Keep current B9 bounds, allocation checks, cleanup, and small-media behavior intact. Unknown/invalid media must retain the client's existing rejection behavior.

Expected existing GC login sizes, for checking the hook against the actual builder:

| Command form | Payload bytes | Complete packet bytes |
| --- | ---: | ---: |
| `9D` base | `0xC8` | `0xCC` |
| `9D` GC extended | `0x12C` | `0x130` |
| `9E` base | `0xE8` | `0xEC` |
| `9E` GC extended | `0x14C` | `0x150` |

These describe the inspected server definitions, not proof of which forms this client currently emits. Inspect the live builder/capture before selecting the hook. The extended-layout flag and extensions must retain their existing meanings.

Conceptual logic, not a drop-in hook:

```text
when serializing a recognized GC Episode III login packet:
    payload[0x16] = larger_B9_profile_enabled ? 0xE3 : 0x00
    payload[0x17] = larger_B9_profile_enabled ? 0x01 : 0x00
    continue the original serialization/encryption/transmission sequence
```

## Matching Teaserv implementation

The following server requirements are implemented in the local Teaserv checkout. These describe the integration contract to preserve during upstream merges; the client agent need not edit Teaserv. Automated verification is recorded in [server validation](EXTENSIONS-VALIDATION.md); the live acceptance checklist below is open except where an item notes otherwise.

* Add a default-false connection-local capability to `Client`; do not store it on an account or infer it from region/build, IP, character data, or earlier sessions.
* Clear it on each login attempt, validate the existing command shape, and recognize only the exact `E3 01` byte pair. Interpret it only for authenticated exact `Version::GC_EP3`, after normal version selection. It must be available before the initial banner send.
* Extend `ep3_media_profile_for_client()` to select the PC profile from this capability, preserving exact-version, authentication, proxy, and replay gates. Current proxy/replay sessions do not gain enhanced media.
* Retain final send-time size checks and whole-group compatibility filtering for configured and extension media, in both extensions-ON and extensions-OFF builds.
* No account setting enables PC limits. Select optional complete fallback groups for original clients, enforcing both original size budgets; retain per-model build and experimental gates.
* Log recognized profile selection without exposing credentials. Test zero/unknown markers, reconnect reset, unauthenticated/Trial/non-Episode-III cases, and identical-account connections with different markers. Verify original clients receive their configured fallbacks while modified clients receive the primary assets.

The marker is a client-declared receive capability, not authentication, administrator permission, or proof of trusted code. Never broaden gameplay privileges or model/build allowlists based on it. Explicit advertisement is sufficient for deciding what that connection claims it can receive.

## Existing B9 envelope and fixtures

No new B9 structure is needed:

```text
complete plaintext B9 packet:
  0x00: u8 command B9; u8 flag 00; u16le complete length
  0x04: u32le media type
  0x08: u32le location flags
  0x0C: u16le unpadded PRS byte count
  0x0E: u16le zero
  0x10: PRS payload, followed by zero padding to a multiple of four
```

Complete bytes = `0x10 + align_up(PRS_bytes, 4)`, never above `0x7C00`. The `0x10` already includes the four-byte game header. Type 4 has no PRS payload. There is no multi-packet reassembly protocol.

Server-generated fixtures are written by the `extension-native-assets` test to (relative to the Teaserv checkout):

`build/server-on/lobby-media-fixtures`

| File | Bytes | SHA-256 |
| --- | ---: | --- |
| `pc-noise.gvm` | 32,848 | `1be4493b71b98538a9d27c3a56d4f5ec6d28bc0aace34fb8a6ac6d26cdb32e34` |
| `pc-noise.gvm.prs` | 20,427 | `5431d8a3d25c6eac7fbb379a4ab430d2759c68f0b42b445cc5924d881a63c499` |
| `pc-multi.gvm` | 262,272 | `eee82dc7e1b56a2d5b6c981b919e756b5238c69382d9dff48b3677b5142176ba` |
| `pc-multi.gvm.prs` | 3,403 | `86784b46be1ce5234b2951c4b9b35325875e72a7ae5c6a71fba4d173a43b81fa` |

`pc-noise` tests compressed bytes above `0x3800`. `pc-multi` independently tests output above the original `0x37000` buffer despite a small compressed payload. They contain deterministic RGB565 pixels; texture indices are `0x180` and `0x190`/`0x191` respectively. Their padded B9 packets occupy 20,444 and 3,420 bytes. These fixtures pass server format/codec tests; visible client rendering is still unverified. Regenerate them with Teaserv's `extension-native-assets` test if necessary; use the same recorded hashes when comparing sessions.

## Acceptance tests and report to return

1. **Plaintext packet regression:** compare marker-enabled and marker-disabled login packets with otherwise identical inputs. Exactly two payload bytes must differ. Confirm unchanged length/flags/version/credentials/configuration, and cover each actually used extended/reconnect form. Redact credentials from reports and shared captures.
2. **Unmodified Newserv compatibility:** connect the marker-enabled client to the original baseline; authenticate, enter an online CARD lobby, switch ship/block, and reconnect. Verify that the login is accepted and normal small media still load. This requires no server modification and can be tested before Teaserv marker support exists.
3. **Marker off:** confirm `PSO_MEDIA_MARKER=0` emits `00 00` on every login and retains normal server compatibility. The confirmed client keeps both receive enhancements enabled in this mode; any future build that disables either enhancement must also suppress the marker.
4. **Automatic Teaserv selection** (exercised with a real Caelum client on 2026-11-09 using `E3 02`; the connection-local and unknown-pair checks below are still open): verify that `E3 01` selects the larger profile and `00 00`/unknown pairs retain original limits. Use the same account on separately established connections to prove the result is connection-local.
5. **Real media session:** send each named fixture through Teaserv, record the session with the client's existing network-recording tools, and check complete reception/decompression, visible placement, delete-all/rebuild, and reconnect behavior. A B9 acknowledgement alone is not proof of successful loading or rendering. Use non-overlapping supported location flags and available slots.
6. **Mixed clients:** confirm an original GC/Dolphin session receives neither oversized fixture, while the marker-enabled session receives both. Verify both compressed-size and decompressed-size filtering, including initial banners and extension scene snapshots.
7. **Receive boundaries:** retain client checks for `0x7BF0` PRS bytes / `0x7C00` complete packet, padding cases, over-limit and truncated packets, and output at/above the 16 MiB bound. Distinguish parser/transport fixtures from renderable assets; exercise decompression boundaries in an isolated client test rather than uploading arbitrary oversized bytes as a model.

Return the exact code hook/function used, emitted plaintext marker offsets/values, actual login forms observed, build/runtime gating behavior, tests performed and results, and any mismatch with this proposed contract. Distinguish source inspection, synthetic tests, recorded traffic, and visible rendering. If the client cannot safely use these bytes, report the evidence before selecting a different field; the server and client must agree on the same contract.

No server acknowledgement, custom opcode, appended login payload, changed base version, save migration, or new model format is part of this change. The repository owner handles commits, pushes, and pull requests manually.

## Server references

* `src/CommandFormats.hh`: `C_Login_DC_PC_GC_9D::unused3`, inherited login layouts, and `S_UpdateMediaHeader_Ep3_B9`.
* `src/ReceiveCommands.cc`: `on_9D_9E` and initial media delivery.
* `src/Client.hh`: default-false connection-local capability storage.
* `src/Episode3/LobbyMediaCapabilities.hh`: exact marker recognition and recipient policy, shared with static tests.
* `src/SendCommands.cc`: `ep3_media_profile_for_client` and `send_ep3_media_update`.
* `src/Episode3/LobbyMedia.hh`: shared byte limits and B9 body encoding.
* [Larger PC lobby media](EXTENSIONS-PC-LOBBY-MEDIA.md): current configuration, asset profiles, fixtures, and implementation status.
* [Server validation](EXTENSIONS-VALIDATION.md): completed server tests and remaining real-client acceptance gates.
