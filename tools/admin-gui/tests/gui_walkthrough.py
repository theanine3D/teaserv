"""Test: drive the real program against a real server and take a picture of every window.

Run it on the hidden desktop so that no window shows up on the screen of the person using the PC:

    python hidden_desktop.py --timeout=300 python gui_walkthrough.py --server-dir DIR --out DIR
        [--server-exe FILE] [--extra-path FOLDERS]

It starts, stops and restarts the server, changes settings through the Settings window, filters the log and opens the
Statistics window. Results go to <out>/results.json and <out>/*.png. The exit code is 0 only if every check passed.
The server folder should be a test copy: its config.json is changed (and restored at the end).
"""

import argparse
import json
import os
import re
import shutil
import socket
import sys
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)

from teaserv_admin import app as app_module  # noqa: E402
from teaserv_admin import jsonc, server  # noqa: E402

import hidden_desktop  # noqa: E402
from lobby_media_check import make_bmp  # noqa: E402

MEDIA_FILES = ("gui-test-banner.bmp", "gui-test-caelum.gvm", "gui-test-fallback.bmp")


class Walkthrough:
    def __init__(self, args):
        self.args = args
        self.out = os.path.abspath(args.out)
        os.makedirs(self.out, exist_ok=True)
        self.results = []
        self.failed = False
        self.dialogs = []
        settings_path = os.path.join(self.out, "admin-settings.json")
        if os.path.exists(settings_path):
            os.remove(settings_path)
        with open(settings_path, "w", encoding="utf-8") as f:
            json.dump({"server_dir": os.path.abspath(args.server_dir), "server_exe": args.server_exe or "",
                       "extra_path": args.extra_path or "", "stats_poll_seconds": 2}, f)
        self.config_path = os.path.join(args.server_dir, "system", "config.json")
        self.config_copy = os.path.join(self.out, "config.json.before-test")
        shutil.copy2(self.config_path, self.config_copy)
        # The Caelum text files are changed by the walkthrough too and put back at the end
        self.kept_files = []
        for name in ("caelum-lobbies.txt", "caelum-modpacks.txt"):
            path = os.path.join(args.server_dir, "system", name)
            if os.path.exists(path):
                copy = os.path.join(self.out, name + ".before-test")
                shutil.copy2(path, copy)
                self.kept_files.append((copy, path))
        # Lobby media files the walkthrough copies into system/ep3/banners are removed at the end
        self.banners = os.path.join(args.server_dir, "system", "ep3", "banners")
        self.media_source = os.path.join(self.out, "media")
        os.makedirs(self.media_source, exist_ok=True)
        for name in MEDIA_FILES:
            if os.path.exists(os.path.join(self.banners, name)):
                raise RuntimeError("the test server already has %s in system/ep3/banners" % name)
        with open(os.path.join(self.media_source, MEDIA_FILES[0]), "wb") as f:
            f.write(make_bmp(64, 7))
        with open(os.path.join(self.media_source, MEDIA_FILES[1]), "wb") as f:
            # Decompresses beyond what original clients take: a Caelum-only texture with an original-client file
            f.write(make_bmp(16, 8)[54:54 + 0x800] + bytes(0x3F800))
        with open(os.path.join(self.media_source, MEDIA_FILES[2]), "wb") as f:
            f.write(make_bmp(16, 9))

        options = app_module.parse_options(["--settings", settings_path, "--no-tray"])
        app_module.prepare_process()
        self.app = app_module.App(options)
        self.app.alert = lambda title, text, kind="error": self.dialogs.append(("alert", text))
        self.app.confirm = self.confirm
        self.app.open_path = lambda path: self.dialogs.append(("open", path))
        # The window loop has to live until the last check, so the program's own exit is held back until finish()
        self.real_finish_quit = self.app.finish_quit
        self.app.finish_quit = lambda: self.dialogs.append(("program exit", ""))
        self.runner = self.steps()
        self.deadline = None
        self.waiting = None
        self.app.root.after(300, self.advance)

    def confirm(self, title, text, default_yes=True):
        self.dialogs.append(("confirm", text))
        return True

    # ----- plumbing

    def check(self, name, ok, detail=""):
        self.results.append({"check": name, "ok": bool(ok), "detail": str(detail)})
        print("%s  %s  %s" % ("PASS" if ok else "FAIL", name, detail), flush=True)
        if not ok:
            self.failed = True

    def shot(self, window, name):
        window.lift()
        window.update()
        time.sleep(0.15)
        window.update()
        path = os.path.join(self.out, name + ".png")
        colors = hidden_desktop.capture_window(int(window.wm_frame(), 16), path)
        self.check("picture " + name, colors > 3, "%d colours" % colors)

    def advance(self):
        try:
            if self.waiting is not None:
                if not self.waiting():
                    if time.time() > self.deadline:
                        raise RuntimeError("timed out waiting: %s" % self.waiting_name)
                    self.app.root.after(100, self.advance)
                    return
                self.waiting = None
            step = next(self.runner)
            if callable(step):
                self.waiting, self.waiting_name = step, getattr(step, "__doc__", "") or "condition"
                self.deadline = time.time() + 60
                self.app.root.after(50, self.advance)
            else:
                self.app.root.after(int(step), self.advance)
        except StopIteration:
            self.finish()
        except Exception:
            self.check("walkthrough ran to the end", False, traceback.format_exc())
            self.finish(emergency=True)

    def finish(self, emergency=False):
        srv = self.app.server
        if emergency and srv.is_active():
            srv.stop_and_wait(20)
        try:
            shutil.copy2(self.config_copy, self.config_path)  # put the test server's config back
            for copy, path in self.kept_files:
                shutil.copy2(copy, path)
        except OSError:
            pass
        for name in MEDIA_FILES:
            try:
                os.remove(os.path.join(self.banners, name))
            except OSError:
                pass
        with open(os.path.join(self.out, "results.json"), "w", encoding="utf-8") as f:
            json.dump({"failed": self.failed, "results": self.results, "dialogs": self.dialogs}, f, indent=2)
        try:
            self.real_finish_quit()
        except Exception:
            pass

    def state_is(self, state):
        def condition():
            return self.app.server.state == state
        condition.__doc__ = "server state %s (now %s)" % (state, self.app.server.state)
        return condition

    def log_text(self, since_seq=0):
        return "\n".join(line.text for line in self.app.server.lines if line.seq > since_seq)

    def config_value(self, *path):
        with open(self.config_path, encoding="utf-8", newline="") as f:
            node = jsonc.find(jsonc.parse(f.read()), path)
        return None if node is None else node.to_python()

    # ----- the walkthrough

    def steps(self):
        app = self.app
        srv = app.server

        # 1. Status window, server stopped
        control = app.open_window("control")
        yield 300
        self.shot(control, "01_status_stopped")

        # 2. Real start
        app.start_server()
        self.check("start: no error dialog", not self.dialogs, self.dialogs)
        yield self.state_is(server.RUNNING)
        yield lambda: srv.stats is not None
        first_pid = srv.pid
        self.check("start: server process is running", srv.proc is not None and srv.proc.poll() is None,
                   "process %s" % first_pid)
        self.check("start: shell accepts commands", srv.shell_ready)
        self.check("start: live numbers arrive", srv.stats is not None and "players" in srv.stats, srv.stats and {
            k: srv.stats[k] for k in ("players", "games", "interval")})
        self.check("start: game port is open", self.port_open(), "127.0.0.1:%d" % self.args.port)
        yield 300
        self.shot(control, "02_status_running")

        # 3. Console and a shell command
        console = app.open_window("console")
        before = srv.seq
        console.entry.insert(0, "help")
        console.send()
        yield lambda: "Shut down the server." in self.log_text(before)
        self.check("console: `help` reply shown", "reload ITEM" in console.text.get("1.0", "end"))
        console.entry.insert(0, "no-such-command")
        console.send()
        yield 800
        self.check("console: a wrong command shows the server's error", "FAILED" in self.log_text(before),
                   [t for t in self.log_text(before).split("\n") if "FAILED" in t][:1])
        self.shot(console, "03_console")

        # 4. A connection is counted (a plain TCP connection to the Episode 3 port; not a login)
        probe = socket.create_connection(("127.0.0.1", self.args.port), timeout=5)
        yield lambda: srv.stats is not None and srv.stats.get("conn", 0) >= 1
        self.check("statistics: an open connection is counted", srv.stats.get("conn") == 1, srv.stats.get("conn"))
        probe.close()
        yield lambda: srv.stats is not None and srv.stats.get("conn", 1) == 0

        # 5. Real stop
        before = srv.seq
        proc = srv.proc
        app.stop_server()
        self.check("stop: state is 'stopping' at once", srv.state == server.STOPPING)
        yield self.state_is(server.STOPPED)
        text = self.log_text(before)
        self.check("stop: the server shut down by itself", "Normal shutdown" in text and proc.returncode == 0,
                   "exit code %s" % proc.returncode)
        self.check("stop: game port is closed", not self.port_open())
        stats_file = self.newest_stats_file()
        last = self.last_stats_line(stats_file)
        self.check("stop: the last statistics line was written", last.get("ev") == "stop", last)
        yield 200
        self.shot(control, "04_status_stopped_again")

        # 6. Start again, then a real restart
        app.start_server()
        yield self.state_is(server.RUNNING)
        yield lambda: srv.shell_ready
        pid_before = srv.pid
        before = srv.seq
        app.restart_server()
        yield lambda: srv.state == server.RUNNING and srv.pid not in (None, pid_before)
        yield lambda: srv.stats is not None
        text = self.log_text(before)
        self.check("restart: old process ended cleanly, new one runs",
                   "Normal shutdown" in text and srv.pid != pid_before and self.port_open(),
                   "process %s -> %s" % (pid_before, srv.pid))

        # 7. Settings: change two values through the window and save
        settings = app.open_window("settings")
        yield 400
        self.shot(settings, "05_settings_general")
        original_name = self.config_value("ServerName")
        original_text = open(self.config_path, encoding="utf-8", newline="").read()
        general = settings.panel_named("General")
        name_editor = [e for e in general.editors if e.field.name == "ServerName"][0]

        name_editor.var.set("A name that is far too long")
        settings.save()
        yield 200
        self.check("settings: a too-long server name is refused", "Not saved" in settings.status.cget("text") and
                   self.config_value("ServerName") == original_name, settings.status.cget("text"))
        self.shot(settings, "06_settings_validation")

        name_editor.var.set("GUI test")
        statistics = settings.panel_named("Statistics")
        interval_editor = statistics.editors[0]
        interval_editor.var.set("30")
        logging_panel = settings.panel_named("Logging")
        clients_editor = [e for e in logging_panel.editors if e.field.name == "LogLevels.Clients"][0]
        clients_editor.combo.current(clients_editor.labels.index("WARNING"))
        clients_editor.touch()
        ok = settings.save()
        yield 300
        new_text = open(self.config_path, encoding="utf-8", newline="").read()
        changed = [(a, b) for a, b in zip(original_text.split("\n"), new_text.split("\n")) if a != b]
        self.check("settings: saved", ok and self.config_value("ServerName") == "GUI test" and
                   self.config_value("TeaservStatsInterval") == 30 and
                   self.config_value("LogLevels", "Clients") == "WARNING", settings.status.cget("text"))
        added = len(new_text.split("\n")) - len(original_text.split("\n"))
        self.check("settings: only the edited lines changed", len(changed) <= 4 and added == 1,
                   "%d lines differ in place, %d line added" % (len([c for c in changed]), added))
        backups = [n for n in os.listdir(app.settings.backup_dir) if n.startswith("config.json.")] \
            if os.path.isdir(app.settings.backup_dir) else []
        newest = os.path.join(app.settings.backup_dir, max(backups)) if backups else ""
        self.check("settings: backup copy holds the previous file",
                   bool(newest) and open(newest, encoding="utf-8", newline="").read() == original_text, newest)
        self.check("settings: restart note names the statistics field",
                   "Needs a server restart: Record statistics every (seconds)" in settings.status.cget("text") and
                   "Server name, Log level of \"Players\"" in settings.status.cget("text"))
        self.shot(settings, "07_settings_saved")

        # 8. Apply without restart (reload config): the server stops logging client events at INFO
        before = srv.seq
        probe = socket.create_connection(("127.0.0.1", self.args.port), timeout=5)
        yield 1200
        probe.close()
        yield 800
        chatter_before = len([t for t in self.log_text(before).split("\n") if t.startswith("I ")])
        settings.do_reload()
        yield 3000
        self.check("settings: 'Apply now' reports success", "Applied" in settings.status.cget("text"),
                   settings.status.cget("text"))
        before = srv.seq
        probe = socket.create_connection(("127.0.0.1", self.args.port), timeout=5)
        yield 1200
        probe.close()
        yield 800
        chatter_after = len([t for t in self.log_text(before).split("\n") if t.startswith("I ")])
        self.check("settings: the server read the new log level (reload)", chatter_after < chatter_before,
                   "INFO lines for one connection: %d before, %d after" % (chatter_before, chatter_after))

        # 9. Restart from the Settings window: the server reads the new statistics interval
        pid_before = srv.pid
        before = srv.seq
        settings.do_restart()
        yield lambda: srv.state == server.RUNNING and srv.pid not in (None, pid_before)
        yield lambda: srv.stats is not None
        self.check("settings: the server read the new value after the restart",
                   srv.stats.get("interval") == 30 and "Recording statistics every 30 seconds" in
                   self.log_text(before), "the server reports interval %s" % srv.stats.get("interval"))

        # 10. The other Settings pages
        for number, title in enumerate(("Network", "Lobby events", "Episode 3", "Episode 3 lobby media", "Logging",
                                        "Caelum lobbies", "Caelum modpacks", "Other settings", "Admin program")):
            panel = settings.panel_named(title)
            settings.show_panel(panel)
            if title == "Other settings":
                index = panel.keys.index("PortConfiguration")
                panel.listbox.selection_set(index)
                panel.select()
            yield 250
            self.shot(settings, "08_settings_%s" % title.lower().replace(" ", "_"))
        lobbies = settings.panel_named("Caelum lobbies")
        self.check("settings: Caelum lobbies file is read", lobbies.names() == ["LOBBY-CARD06"], lobbies.names())
        modpacks = settings.panel_named("Caelum modpacks")
        self.check("settings: Caelum modpack list is read", len(modpacks.packs()) == 1 and
                   modpacks.packs()[0][1].startswith("Caelum lobby test"), [p[1] for p in modpacks.packs()])

        # Save a second lobby and a second modpack through their pages, and refuse a wrong SHA-256
        lobbies_path = os.path.join(app.settings.system_dir, "caelum-lobbies.txt")
        modpacks_path = os.path.join(app.settings.system_dir, "caelum-modpacks.txt")
        lobbies_before = open(lobbies_path, encoding="utf-8", newline="").read()
        modpacks_before = open(modpacks_path, encoding="utf-8", newline="").read()
        lobbies.add()
        lobbies.vars[-1].set("GUI TEST LOBBY")
        modpacks.add()
        sha_var, name_var, url_var = modpacks.rows[-1]
        name_var.set("GUI test pack")
        url_var.set("https://example.com/gui-test.zip")
        sha_var.set("not a hash")
        settings.save()
        yield 200
        self.check("settings: a wrong SHA-256 is refused and nothing is written",
                   "Not saved" in settings.status.cget("text") and
                   open(lobbies_path, encoding="utf-8", newline="").read() == lobbies_before and
                   open(modpacks_path, encoding="utf-8", newline="").read() == modpacks_before,
                   settings.status.cget("text"))
        settings.show_panel(modpacks)
        self.shot(settings, "08_settings_modpack_refused")
        modpacks.rows[-1][0].set("ab" * 32)
        # Lobby media: a picture for all players, and a Caelum-only texture with an original-client picture
        media = settings.panel_named("Episode 3 lobby media")
        media.add()
        media.items[-1]["places"].set("00000030")
        media.set_file(media.items[-1], "file", os.path.join(self.media_source, MEDIA_FILES[0]))
        media.add()
        media.items[-1]["places"].set("00000800")
        media.set_file(media.items[-1], "file", os.path.join(self.media_source, MEDIA_FILES[1]))
        media.set_file(media.items[-1], "fallback", os.path.join(self.media_source, MEDIA_FILES[2]))
        yield lambda: not media.checking()
        settings.show_panel(media)
        media.scroll.show_widget(media.items[-1]["box"])
        yield 200
        self.shot(settings, "08_settings_lobby_media_added")
        ok = settings.save()
        yield 300
        media_list = self.config_value("Episode3LobbyBanners")
        self.check("settings: lobby media list and files saved", ok and media_list[-2:] == [
            [1, 0x30, MEDIA_FILES[0]], [1, 0x800, MEDIA_FILES[1], MEDIA_FILES[2]]] and all(
            os.path.isfile(os.path.join(self.banners, n)) for n in MEDIA_FILES), media_list)
        self.check("settings: lobby media can be applied without a restart",
                   "Can be applied without a restart: Episode 3 lobby media" in settings.status.cget("text"),
                   settings.status.cget("text"))
        lobbies_after = open(lobbies_path, encoding="utf-8", newline="").read()
        modpacks_after = open(modpacks_path, encoding="utf-8", newline="").read()
        self.check("settings: Caelum lobby added to the file", ok and lobbies_after ==
                   lobbies_before.rstrip("\r\n") + "\n21 GUI TEST LOBBY\n", repr(lobbies_after[-40:]))
        self.check("settings: Caelum modpack added to the file", modpacks_after == modpacks_before.rstrip("\r\n") +
                   "\n" + "ab" * 32 + "\tGUI test pack\thttps://example.com/gui-test.zip\n",
                   repr(modpacks_after[-60:]))
        self.check("settings: the report says when the Caelum changes take effect",
                   "Caelum modpacks: used for players who log in from now on" in settings.status.cget("text") and
                   "Needs a server restart: Caelum lobbies" in settings.status.cget("text"),
                   settings.status.cget("text"))
        kept = sorted(n for n in os.listdir(app.settings.backup_dir) if n.startswith("caelum-"))
        self.check("settings: backups of both Caelum files exist", any(n.startswith("caelum-lobbies.txt.") for n in
                   kept) and any(n.startswith("caelum-modpacks.txt.") for n in kept), kept[-2:])
        # "Apply now" (reload config): the running server reads the new list and its files
        before = srv.seq
        settings.do_reload()
        yield 3000
        text = self.log_text(before)
        loaded = [t for t in text.split("\n") if "Loaded Episode 3 lobby banner" in t]
        self.check("settings: the running server loaded the lobby media (reload)",
                   "Applied" in settings.status.cget("text") and
                   all(any(n in t for t in loaded) for n in MEDIA_FILES), loaded or settings.status.cget("text"))
        # The sizes the page computed (Python BMP encoder, PRS reader, the server's compressor) are the server's
        yield lambda: not media.checking() and media.report is not None
        measured = {}
        for result in media.report.entries:
            for measure in (result.primary, result.fallback):
                if measure is not None:
                    measured[measure.filename] = (measure.decompressed, measure.compressed)
        logged = {}
        for line in loaded:
            found = re.search(r"banners/(\S+) \(0x([0-9A-F]+) -> 0x([0-9A-F]+) bytes\)", line)
            if found:
                logged[found.group(1)] = (int(found.group(2), 16), int(found.group(3), 16))
        self.check("settings: the lobby media page's sizes equal the server's", len(logged) == len(MEDIA_FILES) and
                   all(measured.get(n) == logged.get(n) for n in MEDIA_FILES),
                   "page %s, server %s" % ({n: measured.get(n) for n in MEDIA_FILES}, logged))
        # The server accepts the files this window wrote: restart and look for the new lobby in its log
        pid_before = srv.pid
        before = srv.seq
        settings.do_restart()
        yield lambda: srv.state == server.RUNNING and srv.pid not in (None, pid_before)
        yield lambda: srv.stats is not None
        text = self.log_text(before)
        self.check("settings: the server created the lobby that was added",
                   "Caelum extra lobby 21 (GUI TEST LOBBY)" in text,
                   [t for t in text.split("\n") if "Caelum extra lobby" in t])
        loaded = [t for t in text.split("\n") if "Loaded Episode 3 lobby banner" in t]
        self.check("settings: the server loads the lobby media at start", all(
            any(n in t for t in loaded) for n in MEDIA_FILES), loaded)
        settings.close()

        # 11. Logs: level filter and search
        logs = app.open_window("logs")
        yield 400
        total = len(logs.lines)
        self.shot(logs, "09_logs_all")
        logs.level.current(2)
        logs.refresh()
        yield 200
        shown = logs.text.get("1.0", "end-1c").split("\n")
        shown = [line for line in shown if line]
        self.check("logs: level filter shows only warnings and errors",
                   0 < len(shown) < total and all(line[:2] in ("W ", "E ") or line.startswith("FAILED:")
                                                  for line in shown),
                   "%d of %d lines, for example: %s" % (len(shown), total, shown[:1]))
        self.shot(logs, "10_logs_warnings")
        logs.level.current(0)
        logs.search.set("normal shutdown")
        yield 500
        shown = [line for line in logs.text.get("1.0", "end-1c").split("\n") if line]
        self.check("logs: search finds the shutdown lines", len(shown) >= 3 and all(
            "Normal shutdown" in line for line in shown), "%d lines" % len(shown))
        self.shot(logs, "11_logs_search")
        logs.close()

        # 12. Statistics: live numbers and graphs
        stats_window = app.open_window("stats")
        yield 600
        buckets = stats_window.buckets
        self.check("statistics: recorded data is loaded", buckets.sample_count > 0,
                   "%d samples in the last 24 hours, peak %d players" % (buckets.sample_count,
                                                                         buckets.peak("players")))
        self.shot(stats_window, "12_statistics_24_hours")
        chart = stats_window.charts[0]
        chart.update()

        class Pointer:
            x = chart.winfo_width() // 2
            y = chart.winfo_height() // 2
        chart.hover(Pointer)
        self.check("statistics: hovering shows the values", len(chart.find_withtag("hover")) >= 5)
        self.shot(stats_window, "13_statistics_hover")
        chart.delete("hover")
        stats_window.range.current(0)
        stats_window.reload()
        yield 400
        real = [s for s in self.read_stats_lines() if "demo" not in s]
        self.check("statistics: the server recorded its own lines in this test", len(real) >= 4 and
                   any(s.get("ev") == "stop" for s in real), "%d lines written by the server" % len(real))
        self.shot(stats_window, "14_statistics_last_hour")
        stats_window.range.current(3)
        stats_window.reload()
        yield 400
        self.shot(stats_window, "15_statistics_7_days")
        from teaserv_admin import stats_window as stats_module
        table = stats_module.TableWindow(stats_window, stats_window.buckets, "Last 7 days")
        yield 300
        self.shot(table, "16_statistics_table")
        csv_path = os.path.join(self.out, "statistics.csv")
        stats_module.write_csv(csv_path, table.rows)
        self.check("statistics: table saved as CSV", os.path.getsize(csv_path) > 500,
                   "%d bytes" % os.path.getsize(csv_path))
        table.destroy()
        stats_window.close()

        # 13. Quit stops the server cleanly
        proc = srv.proc
        before = srv.seq
        text_holder = srv.lines
        app.quit()
        self.check("quit: asks before stopping the server", any(
            kind == "confirm" and "Stop the server and quit?" in text for kind, text in self.dialogs))
        while proc.poll() is None:
            yield 200
        self.check("quit: the server shut down by itself", proc.returncode == 0 and any(
            "Normal shutdown" in line.text for line in text_holder if line.seq > before),
            "exit code %s" % proc.returncode)
        yield 500
        self.check("quit: the program ends after the server has stopped", ("program exit", "") in self.dialogs)
        self.check("no unexpected error dialog", not [d for d in self.dialogs if d[0] == "alert"],
                   [d for d in self.dialogs if d[0] == "alert"])

    def port_open(self):
        try:
            socket.create_connection(("127.0.0.1", self.args.port), timeout=2).close()
            return True
        except OSError:
            return False

    def newest_stats_file(self):
        folder = self.app.settings.stats_dir
        names = sorted(n for n in os.listdir(folder) if n.endswith(".jsonl"))
        return os.path.join(folder, names[-1])

    def read_stats_lines(self):
        lines = []
        with open(self.newest_stats_file(), encoding="utf-8") as f:
            for line in f:
                if line.strip():
                    lines.append(json.loads(line))
        return lines

    def last_stats_line(self, path):
        with open(path, encoding="utf-8") as f:
            lines = [line for line in f if line.strip()]
        return json.loads(lines[-1])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server-dir", required=True)
    parser.add_argument("--server-exe", default="")
    parser.add_argument("--extra-path", default="")
    parser.add_argument("--out", required=True)
    parser.add_argument("--port", type=int, default=9103, help="a game port from the test server's config")
    args = parser.parse_args()
    walk = Walkthrough(args)
    walk.app.run()
    return 1 if walk.failed else 0


if __name__ == "__main__":
    sys.exit(main())
