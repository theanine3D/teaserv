# Teaserv Admin

Teaserv Admin is a small program for the person who runs a Teaserv server. It sits in the system tray, starts and
stops the server, shows its console and logs, edits its settings in labelled fields, and draws graphs of player
activity. The server itself stays the same console program; Teaserv Admin runs it in the background and talks to its
normal command shell.

It lives in `tools/admin-gui/`. Upstream files carry only the few small hooks listed in the
[integration map](EXTENSIONS-INTEGRATION.md).

## Building and distribution

Teaserv Admin is a mandatory part of every complete Teaserv build and every binary release, with Lua extensions enabled or disabled. Follow the [Teaserv build instructions](../README.md#building-teaserv-from-source) to build the server and package Admin together. Python 3.9+ with Tk and PyInstaller are build requirements; released GUI applications do not require a separate Python installation. Running the GUI directly from Python remains available for development.

## Starting it

You need the server folder: the folder that holds `system/` (with `system/config.json`) and the server program
(`teaserv.exe` on Windows, `teaserv` on Linux). The server must be a build that contains this feature (`--force-shell`
and the `stats` command; see "What the server gained" below).

- **With Python 3.9 or newer installed** (tested with 3.10 and 3.13; with Tk, which the Windows installer includes): double-click
  `tools/admin-gui/teaserv_admin.pyw`, or run `pythonw tools/admin-gui/teaserv_admin.pyw`. No other packages are
  needed.
- **From a binary release**: launch the included `Teaserv Admin.exe` (Windows) or `teaserv-admin` (Linux), supplied alongside the server. Builders must package it using `tools/admin-gui/build_exe.py` as part of the complete build; see the instructions above.

At the first start the program looks for the server folder next to itself, above itself and in the current folder. If
it finds none, it opens **Settings > Admin program**, where you choose the folder. Options:

| Option | Meaning |
| --- | --- |
| `--server-dir FOLDER` | Use this server folder |
| `--start` | Start the server right away |
| `--show` | Open the status window at start |
| `--no-tray` | Do not use the tray; show the status window instead |
| `--settings FILE` | Keep the program's own settings in this file (default: `teaserv-admin-settings.json` beside this installation on Windows and Linux; `--server-dir` uses that folder) |

Preferences are local to each installation. Packaged applications use the executable's folder; running from source
uses the server checkout root. Launching from another working directory does not select another installation's
preferences. `--server-dir FOLDER` uses that folder's preferences unless `--settings FILE` is also supplied.
The old shared user-profile settings file is ignored, so deleted or different installations cannot supply stale paths.
The installation folder must be writable to save preferences; there is no fallback to shared user-profile settings.

Only one copy of the program runs per settings file.

To start it with Windows, put a shortcut to it in the Startup folder (press Win+R, type `shell:startup`), and tick
**Start the server when this program starts** in Settings > Admin program.

## The tray icon (Windows)

The program starts in the tray with no window. Windows 11 puts new tray icons behind the arrow next to the clock; drag
the icon onto the taskbar to keep it visible.

The icon is a disc with a T. Its colour shows the server state, and the tooltip says it in words:

| Colour | State |
| --- | --- |
| Green | The server is running |
| Amber | The server is starting or stopping |
| Grey | The server is stopped |
| Red | The server stopped with an error |

Right-click the icon for the menu: **Start**, **Stop**, **Restart the server**, **View console**, **Settings**,
**Logs**, **Statistics**, **Accounts**, **Quit**. A left click opens the small status window with the same actions as buttons.
Its **Always On Top** box (right of Restart) keeps that window above all other windows; the choice is remembered.

## Start, stop and restart

- **Start** runs the server without a console window. Before it starts, the program checks that the server's game
  ports are free. On Windows two servers can open the same port without an error, and players would then reach one of
  them at random; so if a port is taken (for example by a server started from a `.bat` file), the program refuses to
  start and names the ports.
- **Stop** sends the server its own `exit` command. The server ends its work, saves and closes by itself. The program
  waits for that. It never closes the server by force on its own. If the server has not closed after 30 seconds (you
  can change the time), the program asks you whether to keep waiting or to force it, and warns that forcing can damage
  files that are being written.
- **Restart** is a stop followed by a start, once the old server has closed.
- If players are online, Stop, Restart and Quit ask first and say how many players will be disconnected.
- **Quit** stops the server (after asking) and then ends the program.
- If the program itself is ended from outside (Task Manager, a crash), the server notices that its command line is
  gone and shuts down cleanly by itself. When Windows shuts down or you log off, the program sends the stop command
  and waits up to 8 seconds.

What a stop saves: accounts, teams and Episode 3 tournament state are written to disk the moment they change, so a
stop has nothing left to write for them. Episode 3 (and every other console version) keeps characters on the
player's memory card, not on the server. Blue Burst characters live on the server and are saved every minute while
the player is online; plain newserv does not save them once more at `exit`, so Teaserv now saves every connected Blue
Burst player after the server's work has ended and before the program closes (the log says how many).

## Console

The console window shows the server's output as it appears, with warnings and errors in colour. Type a shell command
in the line at the bottom and press Enter; **List commands** sends `help`. The Up and Down keys recall earlier
commands. The server prints nothing for a command that worked and has nothing to say; a failed command prints a line
that starts with `FAILED:`.

## Settings

The Settings window shows `system/config.json` as labelled fields grouped by topic: General, Network, Accounts, Lobby
events, Episode 3, Games, Logging, Statistics. Each field has a short explanation.

- **Checks.** Values are checked before saving: number ranges, the 16-character server name, IP addresses, port
  numbers, list sizes. A value that fails is not saved, and the window shows the field and the reason.
- **Only your changes are written.** The program replaces just the values you edited. Comments, layout and every
  other value in `config.json` stay exactly as they were. Fields you did not touch are never written.
- **Backup before each save.** The previous file is copied to `config-backups/` in the server folder, with the date
  and time in its name (`config.json.2026-11-12_14-24-47.bak`). To go back, stop the server and copy a backup over
  `system/config.json`. Backups are never deleted by the program.
- **Restart or not.** After saving, the window says which changes can be applied at once and which need a restart,
  and offers a button for each: **Apply now (no restart)** sends `reload config`; **Restart the server now** restarts.
  Fields that need a restart are marked "needs a server restart": the game, DNS and other listening ports, the
  statistics interval and the welcome message (a changed message reached players only after a restart). The status window keeps a note of saved settings that still wait for a restart.
- **Episode 3 lobby media** edits the pictures, models and animations of the Episode 3 card lobby: the
  `Episode3LobbyBanners` list in `config.json` and its files in `system/ep3/banners/`. See "Episode 3 lobby media"
  below.
- **Caelum lobbies** edits `system/caelum-lobbies.txt` (extra card lobbies 20 to 63) as a list of names. Needs a
  restart.
- **Caelum modpacks** finds plain and encrypted modpack ZIPs in `system/caelum-modpacks/` automatically.
  This empty folder is created by fresh builds and included in release archives. The page shows its full path and
  an **Open Folder** button. Names come from each ZIP's root `mod.txt`; SHA-256 hashes are calculated from the
  local ZIP bytes. Only **Download address** is editable. **Refresh** rescans the folder and rehashes every ZIP
  without freezing the window. URLs stay with the same filename when a pack is rebuilt; existing manual listings
  and renamed files are matched by hash, then by an unambiguous name where possible. Existing pack order is kept;
  newly found ZIPs are appended in filename order. Remove a ZIP from the folder and Refresh to remove its listing.
  **Save** writes `system/caelum-modpacks.txt`; upload the exact same ZIP to each download address. Invalid ZIPs,
  duplicate copies and missing URLs are reported before saving. Filename/URL associations are saved beside the
  manifest in `system/caelum-modpacks-admin.json`, so they survive reopening Admin even if a ZIP has changed.
  The manifest is used for new logins. Restart the server after changing packs that add or modify cards.
  See [Caelum modpacks](CAELUM_MODPACKS.md); server key and identity files remain separate.
  **Copy Public Key** (beside Open Folder; its tooltip explains it) copies this server's Caelum identity public key,
  the key modpack creators enter as "Server keys" in the Caelum Modkit to build encrypted server modpacks. It is
  calculated from `system/caelum-server-identity.key` with the server's own method (Monocypher EdDSA with BLAKE2b,
  repeated in `caelum_identity.py` and checked against Monocypher), so the server need not be running. Without that
  file (the server makes it on its first start) the page says so.
  Encrypted server modpacks get a **Server pack key** field (characters hidden unless **Show** is ticked). Paste the
  key or the whole key line from the Caelum Modkit (Build Modpack → Copy Key Line). A line below says at once whether
  the key opens the pack and whether the pack was made for this server's public key. **Save** writes
  `system/caelum-server-keys.txt` (one `<pack id> <pack key> <name>` line per pack; other lines and comments stay;
  the previous file is kept in the backups folder) and refuses while an encrypted pack has no working key. The server
  reads the file again at every Caelum login, so no restart is needed.
  The **Built-in download server** box at the top of this page (`TeaservModpackHTTPEnabled`,
  `TeaservModpackHTTPPort`, `TeaservModpackHTTPAddress` in `config.json`; off by default) lets Teaserv serve the
  ZIPs itself. While it is on, the page fills in every download address as
  `http://<address>[:<port>]/modpacks/<file name>`, greys the address fields out and says why. The address is the
  box's own value, else the External address (Network), which must then be an IPv4 address; Save refuses
  otherwise. Typed addresses are kept in `caelum-modpacks-admin.json` and return when the box is turned off. Turning
  the server on or changing its port needs a server restart.
- **Other settings** is the fallback for every key that has no field: choose a key and edit its text exactly as it is
  in the file (for example `PortConfiguration`, the proxy destinations, quest and item tables, the `Extensions`
  group). The text is checked for mistakes before it is saved. A button opens `config.json` in your text editor.
  `Episode3LobbyBanners` is not listed here; it has its own page.
- **Admin program** holds the program's own settings: server folder, server program, folders with DLLs, start the
  server with the program, the stop time limit, how long logs are kept, how often live numbers are fetched.

If `config.json` has a mistake that makes it unreadable, the window says where (line and column) and shows no
fields until it is fixed.

### Episode 3 lobby media

Settings > **Episode 3 lobby media** lists every entry of `Episode3LobbyBanners` and lets you add, change, reorder and
remove entries. What the server supports is described in [Larger Episode 3 lobby media](EXTENSIONS-PC-LOBBY-MEDIA.md);
in short, each entry is one file shown at one or more fixed places in the card lobby:

| Field | Meaning |
| --- | --- |
| Type | Texture (a picture), model or animation. A model needs a texture at the same places; an animation needs both. |
| Places | Where in the lobby: a hexadecimal number of place bits. **Choose places...** shows the 32 places as tick boxes with their descriptions. Places 00800000 and above are meant for models. |
| File | The file the server sends. Textures: `.bmp` (24 or 32 bits, square, 8x8 to 2048x2048, sides a power of 2), `.gvm` or `.gvm.prs`. Models and animations: `.gvm`, `.gvm.prs`, `.bml` or `.bml.prs`. |
| Original-client file | Optional smaller file for original GameCube and Dolphin clients (the fourth field of the entry). |

**Choose a file...** takes a file from anywhere. A file that is not in `system/ep3/banners/` yet is copied there when
you press Save (files inside that folder, also in subfolders, are used where they are). If a different file with the
same name is already there, the page asks before replacing it, and the old file is kept in `config-backups/`.
Removing an entry leaves its file in the folder; the page lists files that no entry uses.

**Checks.** The page reads every file the way the server does when it loads the list, and shows for each entry the
compressed and decompressed size and who will see it: all players, only Caelum players (too large for original
clients, who see nothing at those places), or Caelum players the main file and original clients the original-client
file. A list the server would refuse is not saved, and the entry shows why:

- the name must have one of the extensions above for its type, and the file must be readable;
- BMP files must be 24-bit, or 32-bit (uncompressed or with byte-sized bit fields), square, with sides a power of 2
  from 8 up; the page turns them into a GVM texture exactly as the server does;
- main files must fit Caelum clients: at most `0x7BF0` bytes compressed and 16 MiB decompressed;
- original-client files must fit original clients: at most `0x3800` bytes compressed and `0x36FFF` decompressed;
- `.prs` files must be complete PRS data;
- when one entry at some places has an original-client file, every entry at those places needs one, there may be only
  one entry of each type there, and there must be a texture (and a model under an animation).

The server compresses `.bmp`, `.gvm` and `.bml` files itself with its optimal PRS compressor. The page asks the server
program for that size (`teaserv compress-prs --optimal`, run without a window), so the numbers are the server's own;
a large picture can take up to a minute. Without the server program the page cannot know the compressed size of such
a file; it says so and saves the list, and the server checks the size when it loads it.

The page also points out lists the server accepts but that probably do not do what you want: a model or animation
without its texture, a lone picture at a model place, a completely transparent 32-bit BMP, more than 8 groups of
places (the game shows at most 8), places shared by two groups, and two entries of the same type at the same places.
The last three also make Lua extension scenes refuse to run in that lobby.

Saving rewrites only the `Episode3LobbyBanners` value in `config.json` (one entry per line, places in hexadecimal);
comments inside that list are not kept. Media changes need no restart: press **Apply now** (`reload config`), and
players see the new media the next time they log in. If the list in `config.json` is not a list the server accepts,
the page says why and shows it empty; saving the page then replaces it.

## Logs

While the server runs under Teaserv Admin, everything it prints is saved to `logs/teaserv-YYYY-MM-DD.log` in the
server folder (one file per day; files older than 30 days are removed, a number you can change, 0 keeps all). The
Logs window shows a day's file with:

- a level filter: everything, information and above, warnings and errors, errors only;
- a search box (upper and lower case are treated the same), with the matches highlighted;
- today's log updating live.

Lines the admin program adds itself are marked `[Admin]` (server started, stop requested, settings saved, commands
you sent).

Log size: with **Network commands (raw data)** at INFO, which is the server's default, the server logs every
command of every player, and a busy day can fill hundreds of megabytes. Set it to WARNING under Settings > Logging
to keep the logs small. The Logs window loads at most the newest 20 MB of a day and says so when it leaves the
older part out.

## Statistics

The top row shows the numbers right now: players online, in lobbies, in games, open games, battles running, Caelum
players, and how long the server has been running. **Online now** lists each player with game version and where they
are. These come from the server's `stats` command, asked every 5 seconds.

Below are four graphs for the chosen period (last hour, 6 hours, 24 hours, 7 days, 30 days):

- **Players online**: online, in lobbies, in games.
- **Logins**: how many players logged in.
- **Games and battles started**.
- **Open games and running battles**.

Move the mouse over a graph to read the values at that time. A gap in a line means the server was not running (nothing
was recorded), not zero players. **Show as table** lists the same numbers and saves them as a CSV file.

The server records these numbers itself, one line per minute, in `system/stats/YYYY-MM.jsonl` (about 8 MB per month).
It does so whenever it runs, with or without Teaserv Admin. Old month files can be deleted by hand. To change the
interval or turn recording off, use Settings > Statistics (`TeaservStatsInterval` in `config.json`; 0 is off).

What is counted, and how:

| Number | Meaning |
| --- | --- |
| Players online | Connections that have logged in |
| In lobbies / in games | Logged-in players by where they are |
| Caelum players | Players whose client announced itself as Caelum at login |
| Open games | Games (rooms), not counting spectator teams |
| Battles running | Episode 3 games with a battle in progress |
| Logins | Each successful login |
| Games started | Each game created |
| Battles started | Each time an Episode 3 game goes from "no battle" to "battle in progress" |

Not counted: quests started (non-Episode 3), chat, trades, time played per player, and anything per account. Numbers
between the last recorded line and a crash are lost (at most one interval); a normal stop writes a final line.

## Accounts

The player accounts of the server: account number (also the Guild Card number), last player name, role, logins (the
GameCube serial number or Blue Burst user name; passwords and access keys are never shown) and any ban. The list is
read from `system/licenses/` in the server folder, so it is shown while the server is stopped too; **Refresh** reads
it again.

- **Register account...** makes an account with one login: a GameCube (Episode 3) serial number, access key and
  password, as the player types them into the game, or a Blue Burst user name and password. Choose its role at once.
  A serial number or user name that another account already has is refused.
- **Role of the selected account** and **Set role**: Player, Moderator, Administrator or Root. Moderators can kick,
  ban and silence; Administrators also change lobby events, announce and join any game; Root can use every command,
  including debugging and cheats anywhere (asked to confirm). The role replaces the account's permission flags; a
  shared-serial account stays shared.
- **Delete...** removes an account for good (asked to confirm). A player online with it stays connected until they
  leave.

Players who connect while the server allows unregistered users get an account by themselves; find them by their
last player name and give them a role here. Changes need the running server: the window sends the server's own
commands (`add-account`, `add-license`, `update-account`, `delete-account`), the server checks and saves them, and
they apply at once. Its answer is shown under the list, and a refusal (`FAILED: ...`) says why. The console and the log
show these commands with the password and access key replaced by stars.

The roles are also the permission levels of extension commands (`moderator`, `admin`, `root`) and of the `who`
column of the pets and wearhat lists (see CAELUM_EXT.md).

## Linux

The windows (status, console, settings, logs, statistics) and the server control work on Linux with Python 3.9+ and
Tk (`python3-tk`); the program starts `./teaserv` from the server folder the same way.

There is **no tray icon on Linux**. The status window is the main window instead, and closing it minimizes it. A
server without a desktop cannot run the program at all; there, keep starting Teaserv as before. The server still
records its statistics in `system/stats/`.

## What the server gained

These are the only changes to the server (details in the [integration map](EXTENSIONS-INTEGRATION.md)):

- `--force-shell`: run the normal command shell even when commands come through a pipe, not a terminal. Replays never
  run the shell.
- `src/TeaservStats.hh/.cc`: counts logins, games created and battles started; writes one line of statistics per
  interval to `system/stats/`; adds the shell command `stats`, which prints the current numbers as one line of JSON
  starting with `TEASERV-STATS`.
- `TeaservStatsInterval` in `config.json` (optional, default 60 seconds, 0 turns the files off).
- `src/TeaservModpackHttp.hh/.cc`: the optional built-in download server for Caelum modpacks
  (`TeaservModpackHTTPEnabled`, default off; `TeaservModpackHTTPPort`, default 80), started and stopped by
  `teaserv_stats::start` / `stop`, so it adds no call site to upstream code.
- A last save of every connected Blue Burst player when the server shuts down normally (see "Start, stop and
  restart").

Nothing a game client sees is changed. An older Teaserv or plain newserv build can still be started by the program,
but it does not read commands from it: the console line, live numbers and a clean stop are not available, and the
program says so.

## Files the program writes

| Path (in the server folder unless noted) | What |
| --- | --- |
| `logs/teaserv-YYYY-MM-DD.log` | Saved server output |
| `config-backups/` | Copies of `config.json`, the Caelum files and replaced lobby media files from before each save |
| `system/config.json`, `system/caelum-lobbies.txt`, `system/caelum-modpacks.txt` | Only when you press Save |
| `system/ep3/banners/` | Lobby media files you chose on the Episode 3 lobby media page, when you press Save |
| `system/stats/YYYY-MM.jsonl` | Written by the server, read by the Statistics window |
| `teaserv-admin-settings.json` beside this installation | The program's own settings |

## For developers

| File in `tools/admin-gui/` | Role |
| --- | --- |
| `teaserv_admin.pyw` | Entry point |
| `teaserv_admin/app.py` | Program shell: tray or status window, windows, dialogs |
| `teaserv_admin/server.py` | The server as a child process: start, `exit`, restart, output, log files, live numbers |
| `teaserv_admin/tray_win.py` | Windows tray icon through the Win32 API (ctypes) |
| `teaserv_admin/jsonc.py` | Parser for `config.json` (the same language as the server's parser) and in-place editing |
| `teaserv_admin/config_schema.py` | The fields of the Settings window: label, kind, checks, restart or reload |
| `teaserv_admin/field_editors.py`, `settings_window.py` | Settings window |
| `teaserv_admin/caelum_files.py` | Reading, checking and writing the Caelum text files |
| `teaserv_admin/lobby_media.py` | The lobby media list and the server's checks for its files (sizes, PRS, BMP to GVM) |
| `teaserv_admin/accounts_window.py` | Accounts window: reading `system/licenses`, roles, the commands it sends |
| `teaserv_admin/console_window.py`, `logs_window.py`, `stats_window.py`, `control_window.py` | The other windows |
| `teaserv_admin/stats.py` | Reading `system/stats` and grouping it for the graphs |
| `build_exe.py` | One-file build with PyInstaller |

To add a setting to the form, add a `Field` to `TOPICS` in `config_schema.py`; say `apply=RESTART` if the server
reads the key only at start (see `DataIndex::load_config_early`).

Tests (in `tools/admin-gui/tests/`):

```sh
python -m unittest discover -s tools/admin-gui/tests -p "test_*.py"   # no window, no server
python tools/admin-gui/tests/failure_check.py --server-dir DIR --server-exe FILE [--extra-path FOLDERS] [--old-exe FILE]
python tools/admin-gui/tests/tray_check.py --server-dir DIR --out DIR [--server-exe FILE] [--extra-path FOLDERS]
python tools/admin-gui/tests/hidden_desktop.py python tools/admin-gui/tests/gui_walkthrough.py --server-dir DIR --out DIR [...]
python tools/admin-gui/tests/hidden_desktop.py python tools/admin-gui/tests/lobby_media_check.py --out DIR [--server-exe FILE] [--extra-path FOLDERS]
```

The unit tests include the lobby media checks; with `TEASERV_EXE` (default `build/server-on/teaserv.exe`) and, for a
MinGW build, `TEASERV_EXTRA_PATH` set, they also compare the BMP-to-GVM encoder and the compressed sizes with the server
program's own output. `lobby_media_check.py` drives the lobby media page on a throw-away server folder (no server is
started); the walkthrough adds media through the page, applies it with `reload config`, restarts, and checks that the
server loaded the files with exactly the sizes the page showed.

`gui_walkthrough.py` drives the real windows against a real server and saves a picture of each. `hidden_desktop.py`
runs it on a separate Windows desktop, so no test window appears on screen or takes the focus. Use a test copy of a
server folder: the walkthrough changes `config.json` and puts it back at the end. `make_sample_stats.py` writes
made-up statistics (marked `"demo": 1`) for a test server nobody plays on. `linux_smoke.py` is the short Linux run
(use Xvfb for an invisible display).
