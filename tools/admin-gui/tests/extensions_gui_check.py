"""Test: the Extensions page of the Settings window, end to end, with a real server checking the result.

Run it on the hidden desktop so that no window shows up on the screen of the person using the PC:

    python hidden_desktop.py --timeout=240 python extensions_gui_check.py --server-exe FILE --data DIR --out DIR
        [--extra-path FOLDERS]

--data is a folder with a complete server "system" folder (tables, maps, ...). The check works on a fresh copy in
--out: it copies the data there, puts the repository's bundled extensions and config.example.json in, and never
touches --data. It turns extensions on through the page, edits settings (including a refused value), adds a package
from a ZIP, removes one, saves, and then runs the server with "extensions check" to prove the server accepts what the
page wrote. Results go to <out>/results.json and <out>/*.png; the exit code is 0 only if every check passed.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
import traceback
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)

from tkinter import messagebox  # noqa: E402

from teaserv_admin import app as app_module  # noqa: E402
from teaserv_admin import jsonc  # noqa: E402

import hidden_desktop  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
SAMPLE = {
    "id": "gui-sample", "name": "GUI sample", "description": "Added by the Extensions page check.",
    "version": "1.0", "api_version": 1, "entry": "main.lua", "permissions": ["commands", "chat.reply"],
    "settings": {"greeting": {"type": "string", "label": "Greeting", "default": "Hello", "max_bytes": 20}},
}


class Check:
    def __init__(self, args):
        self.args = args
        self.out = os.path.abspath(args.out)
        self.results = []
        self.failed = False
        self.dialogs = []
        # A fresh server folder: the data, the repository's extensions and example config, no accounts
        self.server_dir = os.path.join(self.out, "server")
        if os.path.exists(self.server_dir):
            shutil.rmtree(self.server_dir)
        system = os.path.join(self.server_dir, "system")
        shutil.copytree(os.path.join(args.data, "system"), system,
                        ignore=shutil.ignore_patterns("licenses", "players", "teams", "extensions", "extensions-state",
                                                      "config.json", "*.key", "caelum-server-keys.txt",
                                                      "caelum-modpacks*", "caelum-lobbies.txt"))
        os.makedirs(os.path.join(system, "caelum-modpacks"), exist_ok=True)
        shutil.copytree(os.path.join(REPO, "system", "extensions"), os.path.join(system, "extensions"))
        shutil.copy2(os.path.join(REPO, "system", "config.example.json"), os.path.join(system, "config.json"))
        self.config_path = os.path.join(system, "config.json")
        self.backups = os.path.join(self.server_dir, "config-backups")
        self.zip_path = os.path.join(self.out, "gui-sample.zip")
        with zipfile.ZipFile(self.zip_path, "w") as z:
            z.writestr("gui-sample/extension.json", json.dumps(SAMPLE))
            z.writestr("gui-sample/main.lua",
                       "local greeting = extensions.setting('greeting')\n"
                       "extensions.command('guisample', {}, function() extensions.reply(greeting) end)\n")
        settings_path = os.path.join(self.out, "admin-settings.json")
        with open(settings_path, "w", encoding="utf-8") as f:
            json.dump({"server_dir": self.server_dir, "server_exe": args.server_exe,
                       "extra_path": args.extra_path or ""}, f)
        messagebox.askyesno = self.yes  # permission and removal questions
        options = app_module.parse_options(["--settings", settings_path, "--no-tray"])
        app_module.prepare_process()
        self.app = app_module.App(options)
        self.app.alert = lambda title, text, kind="error": self.dialogs.append(("alert", text))
        self.app.confirm = lambda title, text, default_yes=True: self.yes(title, text)
        self.app.open_path = lambda path: self.dialogs.append(("open", path))
        self.real_finish_quit = self.app.finish_quit
        self.app.finish_quit = lambda: None
        self.app.root.after(300, self.run)

    def yes(self, title, text, **kw):
        self.dialogs.append((title, text))
        return True

    def check(self, name, ok, detail=""):
        self.results.append({"check": name, "ok": bool(ok), "detail": str(detail)})
        print("%s  %s  %s" % ("PASS" if ok else "FAIL", name, detail), flush=True)
        if not ok:
            self.failed = True

    def shot(self, window, name):
        window.lift()
        window.update()
        time.sleep(0.2)
        window.update()
        path = os.path.join(self.out, name + ".png")
        colors = hidden_desktop.capture_window(int(window.wm_frame(), 16), path)
        self.check("picture " + name, colors > 3, "%d colours" % colors)

    def save(self, settings):
        """Save; while the Caelum modpacks page is still hashing its ZIPs after a load, wait and try again."""
        deadline = time.time() + 30
        while True:
            result = settings.save()
            if result is not False or "wait for the modpack hashes" not in settings.status.cget("text") or \
                    time.time() > deadline:
                return result
            for _ in range(5):
                settings.update()
                time.sleep(0.1)

    def config(self):
        with open(self.config_path, encoding="utf-8") as f:
            return jsonc.parse(f.read()).to_python()

    def run(self):
        try:
            self.steps()
        except Exception:
            self.check("check ran to the end", False, traceback.format_exc())
        finally:
            self.finish()

    def steps(self):
        settings = self.app.open_window("settings")
        settings.update()
        panel = settings.panel_named("Extensions")
        settings.show_panel(panel)
        settings.update()
        ids = list(panel.tree.get_children())
        self.check("bundled packages listed", len(ids) >= 14 and "chat-tools" in ids, ids)
        self.check("all off at first", all(panel.tree.set(i, "on") == "" for i in ids))
        self.shot(settings, "extensions-1-list")

        # Turn on the host and two packages (permission question answered yes)
        panel.enabled_var.set(True)
        panel.touch_host()
        for package_id in ("chat-tools", "player-of-the-period"):
            panel.tree.selection_set(package_id)
            panel.show_selected()
            panel.state[package_id]["var"].set(True)
            panel.toggle(package_id)
        self.check("permission question asked", sum("Allow this extension" in t for t, _ in self.dialogs) == 2)
        state = panel.state["player-of-the-period"]
        editors = {e.spec.name: e for e in state["editors"]}
        period = editors["period"]
        self.check("choice setting shown as a list", period.field.kind == "choice" and
                   "Month" in period.labels, period.labels)
        period.combo.current(period.labels.index("Month"))
        period.touch()
        prize = editors["prize"]
        prize.var.set("2000000000")  # above max 1000000000
        settings.update()
        self.shot(settings, "extensions-2-host")
        panel.scroll.show_widget(period.frame)
        settings.update()
        self.shot(settings, "extensions-2-settings")
        saved = self.save(settings)
        self.check("value out of range refused", saved is False and "Prize" in settings.status.cget("text"),
                   settings.status.cget("text"))
        prize.var.set("5000")
        title = editors["title"]
        title.var.set("x" * 61)  # max_bytes 60
        self.check("too long text refused", self.save(settings) is False, settings.status.cget("text"))
        title.var.set("Champion")
        chat = {e.spec.name: e for e in panel.state["chat-tools"]["editors"]}["shout_cooldown_ms"]
        chat.var.set("5000")
        self.check("valid settings saved", self.save(settings) is True, settings.status.cget("text"))
        value = self.config().get("Extensions", {})
        entries = {e["id"]: e for e in value.get("Packages", [])}
        self.check("config.json has the extensions", value.get("Enabled") is True and
                   set(entries) == {"chat-tools", "player-of-the-period"}, value)
        self.check("settings written", entries.get("player-of-the-period", {}).get("Settings") ==
                   {"period": "month", "prize": 5000, "title": "Champion"} and
                   entries.get("chat-tools", {}).get("Settings") == {"shout_cooldown_ms": 5000},
                   {k: e.get("Settings") for k, e in entries.items()})
        self.check("grants are the requested permissions",
                   entries["chat-tools"]["grants"] == ["commands", "events", "timers", "storage", "chat.reply",
                                                        "chat.broadcast", "chat.whisper"], entries["chat-tools"])

        # Use the default again: the value leaves config.json
        panel = settings.panel_named("Extensions")
        settings.show_panel(panel)
        editors = {e.spec.name: e for e in panel.state["player-of-the-period"]["editors"]}
        editors["prize"].use_default()
        self.check("default restored", self.save(settings) is True, settings.status.cget("text"))
        entries = {e["id"]: e for e in self.config()["Extensions"]["Packages"]}
        self.check("defaulted value removed", "prize" not in entries["player-of-the-period"].get("Settings", {}),
                   entries["player-of-the-period"])

        # Add from a ZIP, turn it on; remove game-audit (folder moves to the backup folder on save)
        panel = settings.panel_named("Extensions")
        panel.add(self.zip_path)
        self.check("package added from a ZIP", "gui-sample" in panel.packages and os.path.isfile(
            os.path.join(self.server_dir, "system", "extensions", "gui-sample", "main.lua")),
            settings.status.cget("text"))
        panel.state["gui-sample"]["var"].set(True)
        panel.toggle("gui-sample")
        panel.remove("game-audit")
        settings.show_panel(panel)
        panel.select("gui-sample")
        settings.update()
        panel.scroll.show_widget(panel.state["gui-sample"]["frame"])
        settings.update()
        self.shot(settings, "extensions-3-added")
        self.check("save after add and remove", self.save(settings) is True, settings.status.cget("text"))
        removed = os.path.join(self.backups, "removed-extensions")
        self.check("removed folder moved to the backup folder",
                   not os.path.exists(os.path.join(self.server_dir, "system", "extensions", "game-audit")) and
                   any(n.startswith("game-audit.") for n in os.listdir(removed)) if os.path.isdir(removed) else False)
        ids = [e["id"] for e in self.config()["Extensions"]["Packages"]]
        self.check("added package turned on", "gui-sample" in ids, ids)
        settings.close()

    def server_check(self):
        """Run the server on the saved config and ask it to validate the extensions."""
        env = dict(os.environ)
        if self.args.extra_path:
            env["PATH"] = self.args.extra_path + os.pathsep + env.get("PATH", "")
        # Free ports: nothing must clash with a server that may already be running on this PC
        text = open(self.config_path, encoding="utf-8").read()
        root = jsonc.parse(text)
        for path, value in ((("DNSServerPort",), "0"), (("IPStackListen",), "[]"), (("PPPStackListen",), "[]"),
                            (("PPPRawListen",), "[]"), (("PortConfiguration",), '{"gc-eu11": [19201, "gc", '
                                                                               '"game_server"]}')):
            if jsonc.find(root, path) is not None:
                text = jsonc.set_value(text, path, value)
                root = jsonc.parse(text)
        text = jsonc.ensure_group(text, ("CaelumVoice",))
        text = jsonc.set_value(text, ("CaelumVoice", "TurnEnabled"), "false")
        text = jsonc.set_value(text, ("TeaservModpackHTTPEnabled",), "false")
        check_config = os.path.join(self.out, "config.server-check.json")
        with open(check_config, "w", encoding="utf-8") as f:
            f.write(text)
        process = subprocess.Popen([self.args.server_exe, "--config=" + check_config, "--force-shell"],
                                   cwd=self.server_dir,
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env,
                                   creationflags=0x08000000)
        try:
            output, _ = process.communicate(b"extensions check\nextensions list\nexit\n", timeout=120)
        except subprocess.TimeoutExpired:
            process.kill()
            output, _ = process.communicate()
        output = output.decode("utf-8", "replace")
        with open(os.path.join(self.out, "server-check.log"), "w", encoding="utf-8") as f:
            f.write(output)
        self.check("server accepts the saved extensions", "Validation passed" in output and "FAILED" not in output,
                   [line for line in output.splitlines() if "Validation" in line or "FAILED" in line][-3:])
        for package_id in ("chat-tools", "player-of-the-period", "gui-sample"):
            self.check("server runs " + package_id, package_id in output)

    def finish(self):
        try:
            self.server_check()
        except Exception:
            self.check("server check ran", False, traceback.format_exc())
        with open(os.path.join(self.out, "results.json"), "w", encoding="utf-8") as f:
            json.dump({"failed": self.failed, "results": self.results, "dialogs": self.dialogs}, f, indent=2)
        try:
            self.real_finish_quit()
        except Exception:
            pass
        self.app.root.after(100, self.app.root.destroy)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server-exe", required=True)
    parser.add_argument("--data", required=True, help="folder with a complete server system folder (not changed)")
    parser.add_argument("--out", required=True)
    parser.add_argument("--extra-path", default="")
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)
    check = Check(args)
    try:
        check.app.root.mainloop()
    except Exception:
        traceback.print_exc()
    print("RESULT: %s" % ("FAILED" if check.failed else "PASSED"), flush=True)
    sys.exit(1 if check.failed else 0)


if __name__ == "__main__":
    main()
