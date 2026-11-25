# Caelum extension channel and lobby objects

A Lua extension can place models with motions, and music, in any public lobby: one of the 20 stock lobbies or a
Caelum extra lobby (20-63, see [Caelum modpacks](CAELUM_MODPACKS.md)). The server keeps the state of each lobby and
sends it to every Caelum client in that lobby, also to players who enter later. Stock GameCube/Dolphin clients are
never sent any of this: in a stock lobby they see and hear nothing of it (and cannot enter the extra lobbies).

Status: implemented on the server and in Caelum (which draws the objects and plays the music; see the Caelum
project's `pso_ep3_caelum_extension_objects.md`). Each side passes its own tests, and Caelum was checked offline
with stock models. **No session between a real client and server has been run yet.**

Files: `src/CaelumExt.h` (the wire format; the same file as Caelum's `pso_recomp/shared/caelum_ext.h`),
`src/Extensions/CaelumScenes.*` (the registry; no Newserv types), `src/Extensions/ManagerCaelum.cc` (clients and
sending) and the runtime. The feature exists only in builds with `NEWSERV_ENABLE_EXTENSIONS`.

## Lua API (grant `caelum.scene`)

`lobby` is a lobby number or a lobby's menu name. Numbers count the game's lobby list from 0: 0-19 are the stock
lobbies (server names `LOBBY1` ... `LOBBY20`), 20-63 the Caelum extra lobbies of
`caelum-lobbies.txt`. A name matches any case; when two lobbies share a name, the lower number wins. Games and the
overflow lobby are not public lobbies. All calls work in every callback except initialization, migration and
`package_unload`; they need no player, so a schedule can start a show.

| Function | Behavior |
| --- | --- |
| `extensions.caelum_object(lobby, key, spec)` | Create or replace this package's object `key` (1-32 characters: lowercase letters, digits, `-`, `_`). Returns `ok, reason`. |
| `extensions.caelum_remove(lobby, key)` | Remove it. Returns true if it existed. |
| `extensions.caelum_music(lobby, file, options)` | Start (or replace) this package's music in the lobby. `options` may hold `loop` (boolean, default true) and `start_utc_ms`. Returns `ok, reason`. |
| `extensions.caelum_music(lobby, nil)` | Stop this package's music. Returns `ok, reason` (`no_music` when there was none). |
| `extensions.caelum_clear(lobby)` | Remove all of this package's objects and music from the lobby. Returns true if anything changed. |
| `extensions.caelum_listeners(lobby)` | How many clients in the lobby can show extension objects, or nil when the lobby does not exist. |
| `extensions.caelum_lobby(lobby)` | `{number, name, listeners}` of that lobby, or nil when the server has no such lobby. |

To read a position for `x`, `y`, `z`, stand there and type `$xyz` (the `lobby-tools` package): it prints your
position to one decimal place and the lobby's name and number. The height is exact once you stop walking.

`spec` fields:

| Field | Meaning |
| --- | --- |
| `model` | Required. File name of a BML archive that holds the model, its textures and its motions. |
| `model_part` | Optional. Name of the model inside the BML (for example `singer_body.gj`). Default: its first model. |
| `motion` | Optional. Name of the motion inside the BML (for example `sing.njm`). Default: no motion. |
| `texture` | Reserved; not used (a BML carries its textures). |
| `x`, `y`, `z` | Position in the lobby's coordinates (numbers, default 0). |
| `rot_y` | Rotation around the vertical axis in degrees (default 0). |
| `scale` | Default 1; from 0.001 to 1000. |
| `loop` | Whether the motion repeats (default true). |
| `visible` | Default true. With false, clients only load the files, so a later `visible = true` shows the object without a pause. |
| `start_utc_ms` | When the motion started, in Unix milliseconds. Default: now. Every client computes the current pose from it, so everyone sees the same moment, including players who enter later. |
| `rot_x`, `rot_z` | Optional turns in degrees, after `rot_y` (default 0). |
| `attach` | A player's session ID (`ctx.client_id`). The object is attached to that player: it moves and turns with them. The player must be in that lobby (else `player_not_here`); the object is removed when they leave the lobby or disconnect. |
| `bone` | With `attach`: `"root"` (default: the player's position and facing), `"head"` (skeleton node 58, where the game draws head parts), `"head_top"` (node 57) or a node number 0-63. On a bone, `x`/`y`/`z`/`rot_*` are in that node's frame, which turns with the animation; the head node's axes are oblique, so use `upright`. |
| `upright` | With a bone: the bone gives only the place; the object stays level and faces the player's way (`x`/`y`/`z` in the player's frame). The setting for hats: `{bone = "head", upright = true, y = 1.5}` sits on the head and bobs with it. |
| `follow` | With the root: a pet. It trails the player (each frame it moves 8% of the way to its place `x`/`z` beside the player), turns the way it moves, and a jump of over 150 units snaps it. |
| `move_motion` | With `follow`: the motion played while the pet is catching up (more than 2 units away); `motion` otherwise. |

Attached objects are drawn by the client for each player right after the player's body (the game's
`DrawAttachedParts`), so they follow the animation exactly and appear in the see-through-walls outline too. In the
player's frame `x` is sideways (negative: to the player's right), `y` up and `z` forward; the head is about 13.8 units
above the feet. Only clients that named `scene.attach` in HELLO receive attached objects; others are sent the rest of
the room. A package that should give a returning player their pet again does so in its `lobby_join` handler.

File and member names are base names without folders: 1-47 characters of letters, digits, `_`, `.` and `-`, not
starting with a dot. The client looks files up like any game file, so they normally come from a server modpack
listed in `caelum-modpacks.txt`; the server does not check that the files exist.

**What Caelum can show.** Both of the game's model formats, chosen by the member's extension: prop-format models
(`.gj`, the format of lobby and city props) and character-format models (`.nj`: NPCs and other skinned bodies,
with their motions). The textures must be in the same BML. Some stock creature archives cannot be read by the
game's loader outside a battle (the client reports them). Music is a standard ADX file. Objects have no collision and cannot be used. A
client that cannot load a file reports it (`asset <id> ok=0` in the server log) and shows the rest.

Refusal reasons: `no_such_lobby` (the server has no public lobby with that number or name), `player_not_here` (the
`attach` player is not in that lobby), `invalid_file`, `invalid_key`,
`invalid_value` (not a finite number, scale out of range), `lobby_full` (64 objects per lobby, all packages
together), `package_quota` (256 objects per package, all lobbies together), `music_owned` (another package's music is playing there),
`unavailable`. A lobby number outside 0-63, a wrong type or an unknown field is a script error.

A lobby has at most one music track. Objects and music are not saved: after a restart or a package reload they are
gone, and the package places them again (for a running show, from a start time it stored). Unloading or faulting a
package removes everything it placed. `extensions scenes ID` lists a package's objects and music; `extensions list`
shows the number of connected channel clients and objects.

```lua
extensions.schedule("show", {minute = 0, hour = 20, weekday = 6}, function()
  extensions.caelum_object(20, "singer", {model = "concert_singer.bml", model_part = "singer_body.gj",
    motion = "sing.njm", x = 0, y = 0, z = -60, rot_y = 180})
  extensions.caelum_music(20, "concert_song.adx", {loop = false})
  extensions.service_after(12 * 60 * 1000, function() extensions.caelum_clear(20) end)
end)
```

## Protocol (command F2, flags 0x20-0x2F)

Flags 1-5 are the modpack handshake and 0x10-0x13 voice chat. Bodies are text lines ending in `\n`, zero padded.

| flag | direction | body |
|---|---|---|
| `0x20` HELLO | client → server, once, right after the client's F2 flag 2 | `v=1`, `caps=<names separated by commas>` (`scene.model`, `audio.music`, `scene.attach`), `objects=<n>` (the most objects per room the client can show; missing = 16) |
| `0x21` WELCOME | server → client | `v=1`, `caps=<the names both sides know>` |
| `0x22` SNAPSHOT | server → client | `v=1`, `room=<L or G><lobby id>`, `now=<server Unix ms>`, then `obj <package>:<key> model=<file> [part=<name>] [tex=<file>] [motion=<name>] x= y= z= ry= [rx=] [rz=] scale= start=<Unix ms> loop=<0\|1> show=<0\|1> [attach=<lobby slot 0-11> bone=<-1..63> [upright=1] [follow=1] [move=<motion>]]` per object and at most one `music <file> start=<Unix ms> loop=<0\|1>` |
| `0x23` | — | reserved |
| `0x24` EVENT | client → server | `v=1`, `room=<room>`, `asset <id> ok=<0\|1>`: an object's files loaded or failed. Written to the server log (at most 500 per session, 1 KiB each); it never changes server state. |

Rules:

- **The client speaks first.** A Caelum from before this channel ends the session when it receives an unknown F2
  flag, so the server sends 0x21-0x23 only to a client that sent HELLO, and room state only when HELLO named an
  ability. A Teaserv from before this channel logs and ignores HELLO.
- A snapshot is the **whole** state of the client's current room and replaces what the client had. The server sends
  one when the client enters any lobby or game (an empty one outside the extra lobbies, which clears the client)
  and to everyone in a lobby when something there changes.
- `now` lets the client convert the server's times to its own clock.
- The client drops a snapshot that does not parse and keeps the previous state. Unknown lines and unknown fields
  are skipped, so later versions can add to the format.
- Limits: 64 objects per room; a snapshot body is at most 0x7000 bytes (the server stops adding objects at that
  size, which only very long names reach). 64 objects with every field at its
  longest and a music line take 28,176 bytes (checked by Caelum's `tools/caelum_ext_check.c`).
- A client receives at most as many objects as its HELLO's `objects=` line says, the earliest placed first. A
  Caelum from before that line (it holds 16 and refuses a larger snapshot) is sent the first 16.
- Caelum loads every extension object into its PC asset memory, not the GameCube's original 24 MiB heaps, so the
  number of objects is not limited by console memory. Objects that use the same BML share one load: twelve
  players with the same hat cost one load. A new file is read once per game tick; objects using a file that is
  already loaded appear at once. A file the game cannot read is tried once, not once per object.

## The `concert` package

At the set time a performer and a song appear in one lobby for `duration_minutes`; then the lobby is normal
again. `lobby` is the lobby's menu name or its number (see above), so any stock or Caelum lobby can be used; only
Caelum players see and hear the show. `warn_first_minutes` (default 15) and `warn_second_minutes` (default 5)
before the start, a server-wide announcement says "A concert starts in 15 minutes in lobby Stage!"; 0 turns one off.
`prefetch_minutes` before the start, clients in the lobby load the model without showing it. Players who
enter during the show see and hear it from the right moment. After a server restart or a package reload during a
show, it comes back from its original start and still ends on time. Moderators can use `$concert start` and
`$concert stop`.

```json
{"id": "concert",
 "grants": ["commands", "events", "schedule", "storage", "caelum.scene", "chat.announce", "chat.reply"],
 "Settings": {"lobby": "20", "weekday": 6, "hour": 20, "minute": 0, "duration_minutes": 12, "prefetch_minutes": 5,
              "warn_first_minutes": 15, "warn_second_minutes": 5,
              "model": "concert_singer.bml", "model_part": "singer_body.gj", "motion": "sing.njm",
              "music": "concert_song.adx", "x": 0, "y": 0, "z": -60, "rot_y": 180, "scale_percent": 100,
              "announce": "A concert is starting in the concert lobby!"}}
```

The performer file can be made with the Caelum Modkit: Characters tab > **New Character…** copies one of the game's
characters under a name of your own, to change in Blender and retexture; the built modpack holds it under that name.

`weekday` is 1 (Monday) to 7 (Sunday), or 0 for every day; times use `Extensions.TimeZone`. `model` is required;
`music` may be empty. Positions are whole lobby units (`$xyz` reads them). `lobby` is a string: `"20"` is lobby
number 20, `"Stage"` the lobby of that name. The reminders are not sent while a show is running. For several
shows, copy the package under another ID. Put the model BML and the ADX file in a modpack listed in
`caelum-modpacks.txt`; a Caelum extra lobby also goes in `caelum-lobbies.txt`. See `system/extensions/concert.config.example.json`.

## The `pets` and `wearhat` packages

Two example packages built on attached objects. Both remember each player's choice for their account (the
`players.store` grant) and put it back in every lobby the player enters, until they turn it off (and only while they
may still use it). Each pet and hat can be for everyone, for moderators only or for admins only (the `who` column:
`everyone`, `moderator`, `admin`; the account roles of the Admin GUI's Accounts window, checked with
`extensions.has_permission`). Players are shown only what they may use; staff see which entries are restricted. The server removes
the object when the player leaves a lobby; games are skipped. Only Caelum players see them.

- `$pet <name>`: a pet that follows you (`follow`), `side` units to your right and `behind` units behind you. It
  plays its idle motion while you stand still and its walk motion while it follows you. `$pet off` sends it home;
  `$pet` lists the pets.
- `$wearhat <name>`: a hat on the head bone (`bone = "head"`, `upright`). `$wearhat off` takes it off; `$wearhat`
  lists the hats.

The pets are listed in `system/extensions/pets/pets.txt` and the hats in `system/extensions/wearhat/hats.txt`
(package data files, see EXTENSIONS.md), one line each. A pet line names the BML, the model in it and which of its
motions are the idle and the walk, since every BML names its motions its own way. `-` leaves a field out, `#` starts a
comment; edit a file and run `extensions reload`:

```text
# pets.txt
# name    model                  part                 idle                  walk             scale  who
booma     bm_ene_booma.bml       booma_body.nj        booma_wait.njm        booma_walk.njm   50     everyone
eggman    bm_obj_city_sonic.bml  re_b_eggman_body.nj  wait_eggman_body.njm  -                50     moderator

# hats.txt
# name    model                  part                 height  scale  tilt  who
crown     hat_crown.bml          crown.gj             1.2     80     -5    admin
```

(The Booma and crown names are placeholders; take the real member names from the BML.) Invalid lines are skipped and
named in the server log. The `choices` setting of each package adds more without editing the file: entries separated
by commas, fields by colons in the same order (file names cannot contain either). A name in both the file and `choices`
is taken from the file.

| List | Fields | Defaults for `-` |
| --- | --- | --- |
| `pets.txt` | `name model part idle walk scale who` | first model, no motion, walk = idle, 100, everyone |
| `hats.txt` | `name model part height scale tilt who` | first model, 1.5 units above the head bone, 100, 0 degrees (`rot_x`), everyone |

```json
{"id": "pets", "grants": ["commands", "chat.reply", "events", "lobby.read", "caelum.scene", "players.store"],
 "Settings": {"choices": "cat:pet_cat.bml:cat_body.nj:cat_idle.njm:cat_walk.njm:60", "side": 9, "behind": 6}},
{"id": "wearhat", "grants": ["commands", "chat.reply", "events", "lobby.read", "caelum.scene", "players.store"],
 "Settings": {"choices": "tophat:hat_top.bml::1.5:100:0:everyone,crown:hat_crown.bml:crown.gj:1.2:80:-5:admin"}}
```

The shipped lists use stock models (Sonic, and Eggman for moderators, from the city; an aquarium fish as a hat) so the
packages work without a modpack; a server puts its own BMLs in a modpack listed in `caelum-modpacks.txt`. A pet model made
with the Caelum Modkit's **New Character…** keeps its motions; its walk motion goes in `move_motion`. Players are not
told when an entry is invalid; the server log names it. See `system/extensions/accessories.config.example.json`.

## Tests

- `extension-caelum-scenes` (Windows and Linux): registry rules and quotas, the snapshot text read back by the
  client's own parser from the shared header, refusals of malformed text, the Lua API (including a schedule
  placing an object), and the shipped `concert` package with a synthetic host (prefetch, start, restart during a
  show, weekday rule, manual start and stop).
- Caelum: `tools/check_caelum_ext.py` (the two header copies are the same; the parser as the game uses it; Windows
  and Linux).

Not checked: any exchange with a real client (HELLO/WELCOME, snapshots on lobby change, cleanup on unload).
