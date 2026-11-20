# Caelum voice chat — server side

Caelum (the PC port of Phantasy Star Online Episode III) has peer-to-peer voice chat. Players in the same lobby or
battle room talk **directly to each other**; Teaserv never receives, mixes or forwards voice. The server only:

1. tells each Caelum player that voice chat exists here and how to reach the others (address-discovery and relay
   servers, with per-login relay credentials),
2. keeps each room's list of voice users (player or spectator),
3. passes the small connection-setup messages between members of the same room (rate-limited),
4. optionally runs a relay (TURN) for players whose routers block direct connections. The relay forwards encrypted
   packets it cannot read.

Stock GameCube/Dolphin clients are never sent any of this. Only clients that completed the Caelum handshake
(`Client::caelum_mod_ready`, the F2 flag exchange used by Caelum modpacks) take part.

## Rules

- **Rooms.** A public lobby is one room. A battle room is the game plus its spectator teams.
- **Spectators listen only.** In a battle room, spectators are listed with role `S`. Every Caelum client refuses
  voice from role `S`, so a spectator cannot disrupt a match even with a modified client. Spectators still hear the
  players.
- **Quiet lobbies.** `CaelumVoice.QuietLobbies` turns voice off in chosen public lobbies. The roster says `quiet=1`;
  clients make no connections there and the server relays nothing.
- **Defaults on the client** (Caelum settings): voice chat on (hearing others), sending off until the player turns on
  "Send my voice", push-to-talk on the V key.

## Configuration (`config.json`, `"CaelumVoice"`)

| key | default | meaning |
|---|---|---|
| `Enabled` | `true` | voice chat on this server |
| `QuietLobbies` | `[]` | public lobby numbers (1-based, as in `LobbyEvents`) without voice |
| `TurnEnabled` | `true` | run the built-in relay (libjuice TURN server) |
| `TurnPort` | `3478` | UDP port of the relay (it also answers address-discovery / STUN requests) |
| `TurnRelayPortBegin` / `TurnRelayPortEnd` | `0` / `0` | UDP port range the relay uses toward players (0 = any) |
| `TurnCredentialHours` | `12` | lifetime of a player's relay password (a new one is issued at every login) |
| `TurnMaxAllocations` | `1000` | relay sessions at once (each relayed connection uses one) |
| `StunServers` | `[]` | extra address-discovery servers, `"host:port"` |

**Firewall / router:** forward UDP `TurnPort` (default 3478) to the server, plus the relay port range if you set
one (otherwise the relay picks free ports; a range is easier to forward). Without the relay, players behind strict
routers (many mobile and some home connections) may not hear each other.

The relay address sent to each player is the same address the player uses to reach the game server
(`LocalAddress` / `ExternalAddress` rules of `DataIndex::connect_address_for_client`).

## Protocol (command F2; flags 1-5 are the Caelum modpack handshake)

| flag | direction | body |
|---|---|---|
| `0x10` CONFIG | server → client, once after login | text lines: `v=1`, `self=<guild card>`, then `ice=<url>[ <user> <password>]` per server (`stun:host:port`, `turn:host:port?transport=udp`) |
| `0x11` STATE | client → server | one byte: bit 0 voice on (in the room's voice chat), bit 1 may send |
| `0x12` ROSTER | server → client, on every room change | text lines: `v=1`, `room=<L<lobby id>|G<game id>>`, `quiet=0|1`, `battle=0|1`, then `peer=<guild card> <P|S> <lobby slot>` per voice user |
| `0x13` SIGNAL | both ways | u32 LE peer (target when sent, sender when received; the server rewrites it), u8 kind (1 offer, 2 answer, 3 candidate, 4 bye), 3 zero bytes, payload ≤ 2048 bytes (opaque to the server) |

Signals are dropped when the sender has voice off, the room is quiet, the target is not a voice user of the same room,
or the sender exceeds 20 signals per second (burst 50). Malformed signals are logged and ignored, never fatal.

Code: `src/CaelumVoice.hh/.cc` (formats, limits, relay), `src/CaelumVoiceServer.cc` (rooms, rosters, relay of
signals, credentials), hooks in `ServerState` (lobby changes), `ReceiveCommands.cc` (`on_F2_Ep3`, config at login).
Tests: `run-static-tests`, section "Caelum voice chat" (formats, config validation, rate limit, credentials, a live
relay start). The client side is in the Caelum project (`pso_ep3_caelum_voice_chat.md`).

## Third-party code

libjuice (MPL-2.0, https://github.com/paullouisageneau/libjuice, commit `b89c792e`), fetched by CMake at configure
time; set `FETCHCONTENT_SOURCE_DIR_LIBJUICE` to a local checkout of that commit to build offline.
