# Teaserv Admin

Teaserv Admin is a small program for the person who runs a Teaserv server. It sits in the system tray, starts and
stops the server, shows its console and logs, edits its settings in labelled fields, and draws graphs of player
activity. The server itself stays the same console program; Teaserv Admin runs it in the background and talks to its
normal command shell.

It lives in `tools/admin-gui/`. Upstream files carry only the few small hooks listed in the
[integration map](EXTENSIONS-INTEGRATION.md).

## Starting it

You need the server folder: the folder that holds `system/` (with `system/config.json`) and the server program
(`teaserv.exe` on Windows, `teaserv` on Linux). The server must be a build that contains this feature (`--force-shell`
and the `stats` command; see "What the server gained" below).

- **With Python 3.9 or newer installed** (tested with 3.10 and 3.13; with Tk, which the Windows installer includes): double-click
  `tools/admin-gui/teaserv_admin.pyw`, or run `pythonw tools/admin-gui/teaserv_admin.pyw`. No other packages are
  needed.
- **Without Python**: build one file once with `python tools/admin-gui/build_exe.py` (needs PyInstaller). Copy the
  result, `Teaserv Admin.exe`, next to `teaserv.exe` and double-click it.

At the first start the program looks for the server folder next to itself, above itself and in the current folder. If
it finds none, it opens **Settings > Admin program**, where you choose the folder. Options:

| Option | Meaning |
| --- | --- |
| `--server-dir FOLDER` | Use this server folder |
| `--start` | Start the server right away |
| `--show` | Open the status window at start |
| `--no-tray` | Do not use the tray; show the status window instead |
| `--settings FILE` | Keep the program's own settings in this file (default: `%APPDATA%\TeaservAdmin\settings.json`, on Linux `~/.config/teaserv-admin/settings.json`) |

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
**Logs**, **Statistics**, **Quit**. A left click opens the small status window with the same actions as buttons.

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
  Fields that need a restart are marked "needs a server restart": the game, DNS and other listening ports, and the
  statistics interval. The status window keeps a note of saved settings that still wait for a restart.
- **Caelum lobbies** edits `system/caelum-lobbies.txt` (extra card lobbies 20 to 63) as a list of names. Needs a
  restart.
- **Caelum modpacks** edits `system/caelum-modpacks.txt` (the mod list players must have): name, download address
  and SHA-256 for each pack, with a button that takes the SHA-256 from a ZIP file. The list is used for players who
  log in after saving. See [Caelum modpacks](CAELUM_MODPACKS.md) for server modpacks and card mods; the key and
  identity files are not edited here.
- **Other settings** is the fallback for every key that has no field: choose a key and edit its text exactly as it is
  in the file (for example `PortConfiguration`, the proxy destinations, quest and item tables, the `Extensions`
  group). The text is checked for mistakes before it is saved. A button opens `config.json` in your text editor.
- **Admin program** holds the program's own settings: server folder, server program, folders with DLLs, start the
  server with the program, the stop time limit, how long logs are kept, how often live numbers are fetched.

If `config.json` has a mistake that makes it unreadable, the window says where (line and column) and shows no
fields until it is fixed.

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
- A last save of every connected Blue Burst player when the server shuts down normally (see "Start, stop and
  restart").

Nothing a game client sees is changed. An older Teaserv or plain newserv build can still be started by the program,
but it does not read commands from it: the console line, live numbers and a clean stop are not available, and the
program says so.

## Files the program writes

| Path (in the server folder unless noted) | What |
| --- | --- |
| `logs/teaserv-YYYY-MM-DD.log` | Saved server output |
| `config-backups/` | Copies of `config.json` and the Caelum files from before each save |
| `system/config.json`, `system/caelum-lobbies.txt`, `system/caelum-modpacks.txt` | Only when you press Save |
| `system/stats/YYYY-MM.jsonl` | Written by the server, read by the Statistics window |
| `%APPDATA%\TeaservAdmin\settings.json` (Linux: `~/.config/teaserv-admin/settings.json`) | The program's own settings |

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
```

`gui_walkthrough.py` drives the real windows against a real server and saves a picture of each. `hidden_desktop.py`
runs it on a separate Windows desktop, so no test window appears on screen or takes the focus. Use a test copy of a
server folder: the walkthrough changes `config.json` and puts it back at the end. `make_sample_stats.py` writes
made-up statistics (marked `"demo": 1`) for a test server nobody plays on. `linux_smoke.py` is the short Linux run
(use Xvfb for an invisible display).
