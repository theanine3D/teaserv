# Caelum modpacks

Open **Mods**, below Servers in the launcher. **Install** copies a verified ZIP
into `mods` next to Caelum. Installation does not automatically enable a pack;
use its Enable button. ZIPs placed in that folder directly are also listed.
Corrupt ZIPs or packs without valid root metadata show **Invalid** and cannot
be enabled. No archive contents are extracted to the filesystem.

Each ZIP must contain a root `mod.txt`, saved as UTF-8:

```ini
name=My texture and music pack
author=Example Author
website=https://example.com/
version=1.0
region=any
```

`name`, `author`, and `version` are required. `website` is optional. `region`
defaults to `any`; other values are `pal`, `usa`, and `japan`. A mismatching pack
is disabled for the selected disc, including its textures. Its saved preference
is retained for when the matching disc is selected again.

Example ZIP contents:

```text
mod.txt
character_textures.GVM
music/kenkyu_c.adx
texturepack/characters/tex1_128x128_00e114eb45398fde_14.png
texturepack/environment/tex1_128x128_08d779bb4e0904ac_14.dds
```

Outside `texturepack/`, files replace disc files by their basename, ignoring
ASCII case and directories. Rebuild whole archives such as AFS or GVM to replace
their contents. Enabled ZIPs are ordered by filename; the later pack wins a
collision. Within a pack, the last matching ZIP entry wins. Loose mod directories
from the earlier development overlay are no longer loaded; package them as ZIPs.

Files inside `texturepack/`, at any depth, use Dolphin's `tex1_` content-hash
convention. These hashes describe texture bytes, dimensions, palette and format;
they are not guest memory addresses. Identical texture content matches across
PAL/USA/Japan automatically. Differing regional artwork needs its own hashes or
a region-specific pack. Aurora handles PNG, DDS, mip sidecars, palette hashes
and hash wildcards using its existing streaming replacement system.

Enable preferences are local `<pack>.zip.enabled` sidecars in the mods folder.
Use `PSO_MODS` to choose another folder. ZIPs are retained as verified immutable
bytes for the run, so editing a ZIP takes effect on the next launch.

Supported archives: ordinary single-volume ZIP, stored or deflated, up to 1 GiB
compressed and 4 GiB expanded, at most 65,534 entries, 256 MiB per entry and 16 KiB
for metadata. ZIP64, encrypted entries, symlinks, traversal/absolute paths and
duplicate case-insensitive entry names are rejected. Verification checks local
headers, bounds, decompression and every entry's CRC.

## Online play

Local disc-file overrides are blocked before connecting and through already-open
virtual disc handles. Enabled `texturepack/` replacements remain available.
When an offline file mod has already been read, Caelum cold-boots into an online
mode with local file overrides disabled from startup, returning to the title.
Choose Online again after that restart. Selecting Offline in that clean online
mode cold-boots back to normal offline mod loading. A restart also clears any
server gameplay data already read when its session ends. This prevents gameplay
tables cached in guest memory from carrying between mod contexts.

Unmodified Newserv receives a conventional login with an ignored capability
marker, and no new client command is sent unless a server offers the mod protocol.
Teaserv can offer a required pack list. A required pack that is already in the
mods folder (the same SHA-256, under any file name, enabled or not) is used for
the session without asking. For packs not installed yet, Caelum asks Yes/No,
downloads with a progress window, verifies SHA-256 and ZIP contents, and offers
Retry/Cancel on failure. No or Cancel disconnects, saves and returns to the
title. Downloaded packs are installed under their SHA-256 filename, show
"Downloaded from a server" in Mods, and are used only for online sessions that
require them; they stay disabled for offline play unless you enable them there.
The server flow never changes offline enable preferences.

## Teaserv configuration and protocol

Set `CAELUM_MODPACK_MANIFEST` to a UTF-8 manifest file. The default path is
`system/caelum-modpacks.txt`, relative to the server's working directory. With no
file, Teaserv offers an empty list. Use the same manifest on the login and game
server processes. The file begins with `CMOD1` on its own line, followed by
tab-separated lines:

```text
CMOD1
<64 lowercase SHA-256 hex characters><TAB><display name><TAB>https://host/pack.zip
```

Replace `<TAB>` with actual tabs. Get a ZIP's hash with PowerShell
`(Get-FileHash -Algorithm SHA256 -LiteralPath 'pack.zip').Hash.ToLowerInvariant()`.
HTTP and HTTPS are supported; TLS certificates are validated, and HTTPS cannot
redirect down to HTTP. Share links of Google Drive (`drive.google.com/file/d/<id>/view`,
`open?id=`, `uc?id=`, `docs.google.com/uc?id=`, `drive.usercontent.google.com/download?id=`),
Dropbox (`www.dropbox.com/scl/fi/...`, `/s/...`), OneDrive (`1drv.ms/...`,
`onedrive.live.com/...`) and SharePoint (`<tenant>.sharepoint.com/:u:/...`) may be
used as they are, for files shared as "Anyone with the link": Teaserv sends the
link unchanged and the client derives the direct download (`direct_download_url`
in `src/Episode3/Modpacks.hh`; Drive direct endpoint, Dropbox `dl=1`, OneDrive
shares API, SharePoint `download=1`). Folder and Docs links are refused when the
manifest is parsed. Drive and Dropbox limit downloads of popular files, and
Microsoft refuses anonymous API downloads of some personal OneDrive links since
2025, so busy servers should host packs on their own web server.
A list is limited to 32 unique packs and 30,000 wire bytes.
The client also bounds downloads to 1 GiB and verifies the full server list
before publishing any session overrides. Interrupted downloads do not install
partial ZIPs. Server installs never create `.enabled` sidecars.

### Card mods and stock clients

A pack that adds new cards gives Caelum clients cards that a stock Episode III
client does not have: seeing such a card in a battle, or receiving it in a trade,
could crash a stock game or damage its save. While any pack of the mod list adds
cards, Teaserv keeps the two kinds of client apart. Caelum clients are those that
finished the modpack handshake (E3/02 + F2); every other Episode III client is a
stock client. Lobbies stay shared. Separated:

* games: each game remembers whether a Caelum client made it (lobby flag
  `CAELUM_MODDED`); the other kind cannot join it ("Card mods are active..."),
  and it shows as not joinable in the game list;
* battle tables: a table with both kinds does not start a match;
* spectator teams: a spectator team cannot watch a game of the other kind;
* tournaments: a player cannot register while a connected player of the other
  kind is registered (matches are played at battle tables as well);
* card trades are refused across kinds (they happen inside games, so this only
  covers games from before the list changed).

Packs that do not add cards (card art, models, text, scenes, changed stats of
existing cards) leave cross-play open.

How Teaserv decides: it reads each listed pack from `system/caelum-modpacks/`
(or the folder named by `CAELUM_MODPACK_DIR`), finding it by SHA-256 among the
`*.zip` files there. A pack adds cards when its card table
(`PsoCardDataTbl_Base.prs`, in any folder of the ZIP) holds a card ID that
`system/ep3/card-definitions.mnr` lacks; the table's end record is ignored. **A
listed pack without a local copy, or one that cannot be read, counts as adding
cards**, so copy every pack ZIP into that folder. The check runs at startup and
again whenever a Caelum client is sent the list; the server log names each pack's
result. `teaserv check-caelum-modpacks <zip>...` (run where `system/ep3` is)
prints the same check for any ZIPs. Code: `src/Episode3/CaelumCardMods.cc`.

### Server modpacks

A server modpack is encrypted and works only on the servers it names (design and
wire format: Caelum project `pso_ep3_server_modpacks.md`). It is listed in
`caelum-modpacks.txt` like any pack; Caelum recognises it from the ZIP.

* **Identity.** Teaserv makes `system/caelum-server-identity.key` on first start
  (or uses `CAELUM_SERVER_IDENTITY`) and logs its public key. Give that public key
  to modpack creators; they seal packs for it. **Back the file up and keep it
  secret.** Losing it makes every pack sealed for it unusable; another server with
  a copy of it can unlock those packs. Moving the server means moving this file.
* **Keys.** `system/caelum-server-keys.txt` (or `CAELUM_SERVER_KEYS`): one
  `<pack id> <pack key> [name]` line per pack, as `caelum_pack_tool seal` prints
  it; `#` starts a comment. Read at startup and again for every Caelum login.
  Never published, never logged.
* **Handshake.** While a Caelum client is in the mod handshake, it may send one
  `F2` flag `4` (`CSK4`, one-time X25519 key + pack ids). Teaserv answers `F2` flag
  `5` with the keys it holds for those packs, wrapped to the client's key and
  signed with the identity. Anything else (a second request, a malformed one)
  disconnects. Stock clients never see these commands.
* **Card check.** The card-mod cross-play check opens a server modpack's local
  copy (in `system/caelum-modpacks/`) with its key; without a key it counts as
  adding cards.
* The identity file, the keys file and `system/caelum-modpacks/` are in
  `.gitignore`.

Wire extension, native authenticated Episode III sessions only:

* Login 9D/9E bytes at raw offsets `0x1A/0x1B`: `E3 02` advertises mods plus the
  existing extended media capability. `E3 01` remains media-only; stock zeroes
  advertise neither. Proxy and replay sessions never negotiate mods.
* Server command `F2`, flag `1`: manifest text, with ordinary PSO zero padding.
* Client command `F2`, flag `2`, empty payload: every pack verified and activated.
* Client command `F2`, flag `3`, empty payload: decline/cancel/failure.
* Teaserv holds normal login completion until the acknowledgment. It disconnects
  on cancellation or unexpected commands while waiting, and allows 30 minutes
  for the prompt/download flow. No F2 command is sent to stock or media-only clients.

The shared parser is `pso_recomp/shared/mod_protocol.hpp`, mirrored in Teaserv
as `src/Episode3/Modpacks.hh`. Keep these byte-for-byte synchronized.

## Extra lobbies

A pack can add online lobbies after the 20 stock ones. It holds the lobby's
scene files and a root `caelum_lobbies.txt`, one lobby per line:

```text
# lobby number, scene stem, based-on lobby, teleporter name
20 map_cardlobby06 15 LOBBY-CARD06
```

* Lobby numbers run from 20 to 63. The number is also the lobby's place in the
  server's lobby list, so the server's numbers must match.
* The scene stem names `Scene/<stem>n.rel`, `c.rel`, `.gvm`, `.tam`, `s.gj`,
  `s.gvm` and `s.njm`. A copy of a card lobby's seven files is a valid start.
* The based-on lobby (0-19; 15-19 are the card lobbies) gives the object set
  (teleporters, counters, tables) when the pack has no `SET/<stem>o.dat`.
* The name is shown in the teleporter menu, after LOBBY-CARD05. The stock rows are
  upper case. It is the rest of the line (optionally in quotes), up to 40 characters.
* The scene stem is 3-23 characters: letters, digits and `_`. A lobby whose
  `Scene/<stem>n.rel` is not in an active pack is skipped (logged).
* The file is at most 64 KB.

The lobbies appear only on a server that lists them. Teaserv reads
`system/caelum-lobbies.txt` (or the file named by `CAELUM_LOBBIES`), one
`<lobby number> <name>` per line (names up to 32 characters, file up to 16 KB),
numbers from 20 without gaps:

```text
20 LOBBY-CARD06
```

Put the pack in the server's mod list (`system/caelum-modpacks.txt`) as well.
Teaserv lists the extra lobbies, and lets players join them, only after a
client has accepted that list (E3/02 + F2). Stock clients and other servers see
the usual 20 lobbies. A malformed `caelum-lobbies.txt` is logged at startup, and the
server then runs without extra lobbies. A missing file (including a wrong
`CAELUM_LOBBIES` path) silently means no extra lobbies. Extra lobbies have
the fixed lobby IDs 0xCAE00000 + number, so game IDs are the same with or
without them.
