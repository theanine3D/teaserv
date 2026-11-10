# Larger Episode 3 lobby media

Teaserv supports the Episode III PC recompilation's larger **existing B9** payloads. This implements the server portion of `NEWSERV_LOBBY_MEDIA_HANDOFF.md` (2026-10-21), checked against the supplied client's `port/net/hand_media.c`. The existing login packet advertises receive support using the agreed `E3 01` marker, confirmed in the client's `pso_ep3_lobby_media.md` (2026-10-22), or `E3 02`, which means the same plus Caelum modpack support. No new packet type, fragmentation, or server acknowledgement is introduced.

## Limits and compatibility

| Profile | Maximum PRS bytes | Maximum decompressed bytes |
| --- | ---: | ---: |
| Original GameCube / Dolphin (default) | `0x3800` = 14,336 | `0x36FFF` = 225,279 |
| Modified PC port (`E3 01` login marker) | `0x7BF0` = 31,728 | `0x1000000` = 16,777,216 |

The complete packet is `0x10 + align_up(compressed_size, 4)` bytes: four bytes of game header, twelve bytes of B9 fields, and padded PRS data. Its maximum is `0x7C00` bytes. The size field contains the unpadded PRS length. Reserved fields and padding are zero. Type 4 remains the existing empty delete-all command. Eight media slots per type remain unchanged; extensions retain their conservative eight texture-backed group budget.

Original-client output stays **strictly below `0x37000`**, preserving Teaserv's existing conservative boundary. The handoff's suggested `gc_ok <= 0x37000` is deliberately tightened: the modified client itself selects the safe heap path at that boundary, while the original decoder's overflow behavior is unsafe. PC output may reach 16 MiB inclusive. These byte limits do not imply that every native asset format or GPU resource arrangement is supported.

Configured `Episode3LobbyBanners` are loaded up to the PC envelope and classified by both sizes. Both initial banner paths and extension scene snapshots omit incompatible groups for original clients. A dependent texture/model/animation group is omitted together if any member is incompatible. Final B9 transmission independently checks the connected recipient's exact retail Episode 3 version and actual PRS sizes, even if a caller or catalog flag is wrong. Trial Edition receives no B9 through this sender. Replay and proxy sessions never gain the PC profile.

## Automatic client identification

The modified client retains the GameCube base version and sends exactly `E3 01` (or `E3 02` when its mod API is present) in `9D`/`9E::unused3`: payload offsets `0x16/0x17`, complete-packet offsets `0x1A/0x1B`. Teaserv validates the existing login shape and authenticates normally before recognizing the marker, and selects the larger profile before initial banners. Only exact retail `GC_EP3`, authenticated, local, non-replay sessions qualify. `E3 02` selects the same profile and also marks the session Caelum-modpack capable (see [Caelum modpacks](CAELUM_MODPACKS.md)). `00 00` and all other pairs retain original limits.

The capability belongs to the connection and is cleared before each handled login attempt, including malformed or failed attempts. It is never saved on the account, in security data, or in character files; reconnects must advertise again. It grants no gameplay privileges or model/build eligibility. The current client emits the marker in both base and extended `9E` builders; Teaserv also recognizes the shared field in `9D`. `DB` is unchanged. `PSO_MEDIA_MARKER=0` on the PC client emits zero bytes for regression testing while its larger receive paths remain enabled.

The account-ID override has been removed. Only the connection's authenticated E3/01 or E3/02 advertisement enables the larger envelope. Remove `Episode3ExtendedMediaTestAccounts` from existing local configuration; it no longer enables any session. Teaserv selection was exercised with a real Caelum client on 2026-11-09 (`E3 02`). Real-client original-Newserv interoperability and mixed-client rendering/captures remain acceptance gates. See the [wire contract and checklist](EP3-CLIENT-CAPABILITY-HANDOFF.md).

## Configured original-client fallbacks

Existing three-field `Episode3LobbyBanners` entries keep their behavior. An optional fourth filename supplies an original-client substitute:

```json
"Episode3LobbyBanners": [
  [1, 48, "large.gvm.prs", "small.gvm.prs"]
]
```

Files reside under `system/ep3/banners/`. Modified clients receive the primary; unmodified clients receive the fallback, even when the primary happens to fit original limits. Fallbacks must satisfy both original compressed and decompressed limits; oversized, empty, malformed PRS or unreadable fallbacks reject configuration loading. There is no automatic resizing, transcoding, or client download negotiation.

For model/texture/animation groups sharing the same location flags, supply a fourth filename on **every member**. Partial fallback groups reject configuration loading so textures and models cannot come from different variants. The media type and locations remain the same; each fallback replaces that entry's data. Types 2/3 also accept `.bml`/`.bml.prs` filenames. As with primary configured media, administrators are responsible for native format, material/texture references, location semantics, and actual rendering. Prepared variant pairs for BMP entries use the same generated texture index.

Without a fallback, original-compatible primary media remains visible to both profiles; incompatible groups are omitted for original clients. Selection is shared by initial configured-banner sends and extension snapshots, including builds with extensions OFF. Final send-time PRS size checks remain mandatory.

## Extension assets and scripting

Large extension assets explicitly opt into PC preparation in their manifest:

```json
"assets": {
  "pc-banner": {
    "texture": "assets/pc-noise.gvm.prs",
    "media_profile": "pc-port",
    "fallback": {"texture": "assets/small.gvm.prs"}
  }
}
```

Omitting `media_profile`, or choosing `"original"`, retains the original preparation limits. Unknown profile names are errors. Choosing `pc-port` only raises asset preparation budgets; each recipient must independently qualify for transmission. Actual compressed and decompressed sizes determine compatibility, so small assets prepared with this profile can still reach original clients. Catalog and scene quotas remain in effect.

The optional `fallback` supplies a complete original-client variant. It always uses original limits; models/animations require matching dependencies, their own model build allowlist, and the same placement category as the primary. Both variants count toward catalog byte limits. See [fallback asset authoring](EXTENSIONS-SCENES.md#original-client-fallback-groups) for model and validation rules.

PC-profile file reads are bounded to 16 MiB, compressed inputs to `0x7BF0`, and decompression to 16 MiB. Texture validation still accepts only the documented conservative GVM subset: 1..16 textures, RGB565/RGB5A3, square powers of two 8..256, and no mipmaps or palettes. Larger packets can hold less compressible textures or larger multi-texture sets; this does not unlock arbitrary image dimensions. PRS streams are normalized with upstream's compressor and terminator before transmission.

`extensions.scene_capabilities()` adds `extended_ep3_media`, `max_media_compressed_bytes`, and `max_media_decompressed_bytes`. Numeric budgets are zero when native media is unavailable. Existing ownership, grant, lobby, experimental-model, and exact model-build gates still apply. Models and animations remain experimental and disabled by default; increasing a byte limit does not validate their structure or rendering.

## Reproducible server fixtures

`extension-native-assets` writes self-generated fixtures to the selected full server build's `lobby-media-fixtures/` directory. They contain valid GVM headers and RGB565 pixel storage, with deterministic noise/zero pixels; they are server transport/parser fixtures, not artist-authored decorations.

* `pc-noise.gvm[.prs]`: one 128x128 texture with 18,000 deterministic pseudorandom pixel bytes and a zero tail; compressed output exceeds `0x3800` and fits `0x7BF0`.
* `pc-multi.gvm[.prs]`: two 256x256 zero-filled textures; decompressed output exceeds `0x37000` while compressed output still fits `0x3800`. This tests the independently important decompressed-size gate.

The tests verify exact PRS round trips, original-profile rejection, PC-profile acceptance, over-limit rejection, packet boundary alignment, mixed-profile omission and cleanup, Lua capability budgets, and real package reload behavior. See [validation](EXTENSIONS-VALIDATION.md) for executed checks; measured fixture sizes and hashes follow.

Generated with the real upstream PRS codec in `build/server-on/lobby-media-fixtures/` on 2026-10-22:

| File | Bytes | SHA-256 |
| --- | ---: | --- |
| `pc-noise.gvm` | 32,848 | `1be4493b71b98538a9d27c3a56d4f5ec6d28bc0aace34fb8a6ac6d26cdb32e34` |
| `pc-noise.gvm.prs` | 20,427 | `5431d8a3d25c6eac7fbb379a4ab430d2759c68f0b42b445cc5924d881a63c499` |
| `pc-multi.gvm` | 262,272 | `eee82dc7e1b56a2d5b6c981b919e756b5238c69382d9dff48b3677b5142176ba` |
| `pc-multi.gvm.prs` | 3,403 | `86784b46be1ce5234b2951c4b9b35325875e72a7ae5c6a71fba4d173a43b81fa` |

GVM global texture indices are `0x180` for the noise texture and `0x190`/`0x191` for the multi-texture set. The corresponding padded B9 packets are 20,444 and 3,420 bytes respectively. Generated binaries stay in ignored build output; the test source reproduces them without external media downloads.

## Client-side follow-up

No client or network rendering test is claimed here. Connect the modified client and record an online CARD lobby session with these exact fixtures. Verify receipt, complete decoding, visible placement, delete/rebuild behavior, original-client omission in a mixed lobby, and reconnects. Repeat using `PSO_MEDIA_MARKER=0` and an original GC/Dolphin client to verify original limits remain selected, including when reusing the same account. Keep experimental model tests separate.

The agreed receive advertisement covers only the existing B9 envelope. Apart from the Caelum modpack handshake that `E3 02` starts, general bidirectional extension negotiation remains separate. Media exceeding `0x7BF0` compressed bytes requires a separately designed client/server fragmentation protocol and is rejected here.

The [client capability handoff](EP3-CLIENT-CAPABILITY-HANDOFF.md) records the implemented login marker, original-client fallbacks, and outstanding cross-server acceptance tests. Source compatibility with original Newserv is supported by its ignoring these bytes; live interoperability still needs verification.
