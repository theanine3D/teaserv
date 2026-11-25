# Clock, schedules, player data, boards and Episode 3 Meseta

This part of the extension API (still API 1) lets a package act on the calendar, remember things per player, rank
players, and give Episode 3 Meseta. It adds no packet type and no client change: Meseta uses the existing `B7`
update, so it works for stock Episode 3 clients and for Caelum. Two example packages ship with it: `daily-reward`
and `player-of-the-period`.

The code is in `src/Extensions/Clock.*`, `PlayerData.*`, `ManagerPlayers.cc` and the runtime. The only call from
upstream code is one observation in `on_CA_Ep3` (see the [integration map](EXTENSIONS-INTEGRATION.md)).

## New grants

| Grant | Allows |
| --- | --- |
| `schedule` | `extensions.schedule`, `extensions.service_after` |
| `players.store` | `extensions.player_get`, `extensions.player_set` |
| `boards` | `extensions.board_add`, `board_get`, `board_top`, `board_reset` |
| `economy.meseta` | `extensions.meseta_grant` |

`extensions.now()` and the `ep3_battle_ended` event need no new grant (the event needs `events`).

## Server settings

```json
"Extensions": {
  "Enabled": true,
  "TimeZone": "local",
  "Economy": {"MaxGrant": 100000, "DailyLimitPerPackage": 10000000, "BalanceCap": 9999999},
  "Packages": []
}
```

- `TimeZone`: `"local"` (default, the server computer's time zone) or `"UTC"`. It decides what "today", "this week"
  and schedule times mean.
- `Economy.MaxGrant`: the most Meseta one call may give (1 to 1,000,000,000).
- `Economy.DailyLimitPerPackage`: the most one package may give in one server day, all players together.
- `Economy.BalanceCap`: a grant never raises a balance above this (at most 4,294,967,295). The default stays below
  the largest amount the client is known to show.

Invalid values reject `extensions reload` and keep the active packages. `extensions list` shows the limits, the
number of known Episode 3 accounts, queued grants and registered schedules.

## The clock

`extensions.now()` returns a table of integers: `utc` (Unix seconds), `offset_seconds`, `year`, `month`, `day`,
`hour`, `minute`, `second`, `weekday` (1 = Monday to 7 = Sunday), `yday`, `iso_year`, `iso_week` and `day_number`
(days since 1970-01-01 in the server's zone). It works everywhere, including initialization. The Lua `os` library
remains unavailable.

Use `day_number` for "once per day" and `iso_year` with `iso_week` for "once per week". The ISO year differs from
the calendar year around New Year.

## Schedules and service timers

```lua
extensions.schedule("weekend-show", {minute = 0, hour = 20, weekday = {6, 7}}, function(info)
  extensions.log(info.name .. " fired at " .. info.utc)
end)
```

- Register during initialization only, at most 16 per package. The name follows the package ID rules.
- Rule fields: `minute` (0-59, required), `hour` (0-23), `weekday` (1-7), `day` (1-31) and `month` (1-12). Each is
  one integer or an array of integers; an omitted field means "any". Or use `every_minutes` (1-1440) alone: it fires
  when the minute count since midnight 1970-01-01 (server zone) is a multiple of it.
- The handler receives one table: `event` (`"schedule"`), `name` and `utc`.
- The host checks once per minute against the wall clock. A rule fires at most once per minute. Minutes that pass
  while the server is down, or that a clock change skips, are **not** replayed; the repeated hour when clocks go
  back does not fire twice. A package that must not miss a period compares a stored period name with
  `extensions.now()` in `package_load` (see `player-of-the-period`).
- `extensions.service_after(ms, handler)` starts a one-shot timer with no player (50 ms to 6 hours). It returns a
  timer ID, or nil and an error when the package's 32-timer quota is full. It can be created in any callback except
  `package_unload`, and `extensions.cancel(id)` cancels it from any callback. A player leaving never cancels it. It
  is not kept across reload or restart.

Schedule and service-timer handlers run under **extension authority**, like `package_load`: there is no player, so
reply, broadcast, whisper, player timers, scene calls and lobby queries are unavailable. Logging, settings, storage,
player data, boards, Meseta grants and (with `chat.announce`) announcements work. The usual callback budgets apply.

## Player keys

Every player context (`ctx`) now has `player_key` when the account is a permanent one: an opaque string, `p`
followed by 24 hexadecimal digits. It is the same for that account in every session and after restarts, and it
does not reveal the account or Guild Card number. Temporary accounts have no key.

The key is a keyed hash of the account number. The secret is created on first use and kept, with the list of known
Episode 3 players and queued grants, in `system/extensions-state/host.players.json`. **Back this file up with the
rest of the folder.** Without it, keys change and stored player data no longer matches anyone.

## Per-player storage

```lua
extensions.player_set(ctx.player_key, "last_day", "20416")
local last = extensions.player_get(ctx.player_key, "last_day")   -- string or nil
extensions.player_set(ctx.player_key, "last_day", nil)           -- delete
```

Each package has its own store per player: at most 16 keys per player, key names of 1-32 lowercase letters, digits,
`-` or `_`, values up to 256 bytes, and 50,000 players per package. Not available during initialization or
migration; read-only in `package_unload`. An invalid player key or a full quota is a script error.

The data is in `system/extensions-state/<package>.players.json`, saved about every five seconds and before a reload.
It belongs to the host, not to a package instance: reloading keeps it, `extensions check` never changes it, and it
is not part of `storage_version` migrations. A damaged file is reported and never reset.

## Boards

Lua has no sort function here, so the host keeps scores and ranks them.

| Function | Behavior |
| --- | --- |
| `extensions.board_add(board, player_key, amount, name)` | Add an integer (may be negative) to the player's score and return the new score. `name` (optional, up to 64 bytes) is the name shown in rankings; an empty or omitted name keeps the old one. |
| `extensions.board_get(board, player_key)` | The score, or nil. |
| `extensions.board_top(board, count)` | Up to `count` (1-50) entries `{rank, player_key, name, score}`, highest score first. With equal scores, the player who reached that score first is ahead. |
| `extensions.board_reset(board)` | Empty the board; returns false if it does not exist. The 50 best entries of the closed period stay readable as `board_top("<board>.previous", n)`. |

At most 8 boards per package and 50,000 entries per board. Board names follow the storage key rules.

## Episode 3 Meseta

```lua
local ok, status, balance = extensions.meseta_grant("self", 1000, "daily-20416", "daily login reward")
local ok, status = extensions.meseta_grant(player_key, 100000, "prize-2026-W48", "Player of the Week prize")
```

- **Target:** `"self"` is the player of the current callback (commands, player timers, `lobby_join`). A player key
  names any known player; this works from every callback except `package_unload`, including schedules.
- **Amount:** 1 to 1,000,000,000, further limited by the server settings above.
- **Grant ID** (1-64 bytes, required): the host remembers the last 32 IDs per player and package. A repeat returns
  `true, "duplicate"` and gives nothing. Build the ID from the reason and the period (`"daily-" .. day_number`), so
  a crash or a repeated event cannot pay twice.
- **Reason** (optional, up to 100 bytes): written to the server log, and shown to a player who receives a queued
  grant.

| Result | Meaning |
| --- | --- |
| `true, "granted", balance` | The player is online on an Episode 3 client. The account was saved and the client's balance updated. |
| `true, "queued"` | The player is offline (or on another game version). The grant is stored and added at the next Episode 3 login, before the login balance is sent; the player then sees `You received N Meseta: <reason>` in the first lobby. |
| `true, "duplicate"` | This grant ID was already used for this player. |
| `false, "not_ep3"` | The key is not a known Episode 3 player (never logged in with Episode 3 on this server), or the account is temporary. |
| `false, "limit"` | Over `MaxGrant`, over the package's daily limit, or more than 32 grants already queued for this player. |
| `false, "cap"` | The balance is already at `BalanceCap`. A grant that would pass the cap is cut to reach it exactly. |
| `false, "infinite_meseta"` | The server's `Episode3InfiniteMeseta` option is on; balances are not real. Queued grants wait until it is off. |
| `false, "unavailable"` | `"self"` was used without a player, or the package lost its grant. |

Every accepted grant writes one server log line with the package, player key, amount, new balance, grant ID and
reason. Wrong argument types, a malformed player key or a missing grant are script errors.

Chat mutes do not block rewards. The package's own grants decide what it may give; nothing else limits which
callbacks may call `meseta_grant`, so install only packages you trust and set the limits to suit your server.

## Event `ep3_battle_ended`

Register with the `events` grant. The handler receives one table and runs under extension authority.

```lua
extensions.on("ep3_battle_ended", function(info)
  for _, team in ipairs(info.teams) do
    if team.won then
      for _, p in ipairs(team.players) do
        if p.player_key then extensions.board_add("wins", p.player_key, 1, p.name) end
      end
    end
  end
end)
```

| Field | Value |
| --- | --- |
| `event` | `"ep3_battle_ended"` |
| `lobby_id`, `name` | The game's ID (decimal string) and name |
| `winner_team` | 0 or 1 |
| `tournament` | true for a tournament match |
| `rounds` | The battle server's round counter at the end |
| `duration_s` | Seconds since the battle started |
| `rules_modded` | true when the game uses a Caelum modpack card catalog instead of the stock one |
| `teams` | Two tables: `team` (0/1), `won` (boolean) and `players` |

Each player has `slot` (0-3), `is_com`, `name`, and for humans `player_key` (absent for temporary accounts) and
`client_id`. A human who left the game before the result is listed with `is_com = true`, an empty name,
`disconnected = true` and no key.

The event fires once per battle, when the battle server decides the result (`Server::set_battle_ended`), for normal
and tournament games. It does not fire for spectator teams, battle replays, or a result the server cannot decide
(for example a team with no players). What the server does when a player disconnects in the middle of a battle has
not been checked with real clients; do not count such results until it has.

## Example packages

**`daily-reward`**: Meseta once per server day, when the player first enters a lobby that day.

```json
{"id": "daily-reward", "grants": ["events", "players.store", "economy.meseta", "chat.reply"],
 "Settings": {"meseta_per_day": 1000, "streak_bonus": 0, "streak_max_days": 7, "message": "Daily login reward:"}}
```

`streak_bonus` is added for each consecutive day after the first, up to `streak_max_days`.

**`player-of-the-period`**: counts battle wins per week (Monday to Sunday) or month, announces the leader at the
end and gives the prize, at once or at the winner's next login. `$ranking` shows the standings.

```json
{"id": "player-of-the-period",
 "grants": ["commands", "events", "schedule", "storage", "players.store", "boards", "economy.meseta", "chat.announce", "chat.reply"],
 "Settings": {"period": "week", "prize": 100000, "count_com_wins": false, "min_rounds": 3,
              "max_wins_per_day": 20, "title": "Player of the Week"}}
```

Wins against computer-only teams count only with `count_com_wins`. Battles shorter than `min_rounds` do not count,
and each player counts at most `max_wins_per_day` wins per day (a simple guard against win trading). With equal
wins, the player who got there first wins. The prize must not exceed `Economy.MaxGrant`: raise that limit for a
larger prize. The period is checked every hour, at package load and after every battle, so a server that was off
when the period ended settles it at the next start.

Merge the entries into your existing `Extensions.Packages` (see `system/extensions/players.config.example.json`),
then run `reload config`, `extensions check` and `extensions reload`.

## Tests

- `extension-player-data` (Windows and Linux, no server needed): calendar and ISO-week cases, schedule rules,
  store/board/grant-ID limits, runtime scope and grants, and both example packages with a synthetic host (streaks,
  duplicate protection, win rules, period rollover, restart catch-up).
- `extension-players-smoke` (`tests/extensions/PlayersSmokeTest.py`, real server, about three minutes): a schedule
  firing on the wall clock, files on disk, the refusal for unknown players, settings validation, restart, and a
  damaged player file.

Not yet checked with real clients: the balance shown after a live grant, the queued-grant message at login, and the
battle event from an actual match (including a mid-battle disconnect).
