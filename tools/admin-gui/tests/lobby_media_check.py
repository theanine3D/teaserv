"""Test: the "Episode 3 lobby media" page of the Settings window, on a throw-away server folder (no server is started).

Run it on the hidden desktop so that no window shows up on the screen of the person using the PC:

    python hidden_desktop.py --timeout=300 python lobby_media_check.py --out DIR [--server-exe FILE]
        [--extra-path FOLDERS]

The server program is only used to learn the compressed size of files the server compresses itself; without it the
page still works and says that size is not known. It builds <out>/server/system from config.example.json, drives the
page (list, add, choose places, refuse an oversized original-client file, save, replace a file, remove an entry, a
broken list), and checks the files the page writes. Results go to <out>/results.json and <out>/*.png. The exit code
is 0 only if every check passed. The server's own acceptance of a saved list is checked by gui_walkthrough.py, which
restarts a real server.
"""

import argparse
import json
import os
import random
import shutil
import struct
import sys
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)

from teaserv_admin import app as app_module  # noqa: E402
from teaserv_admin import jsonc, lobby_media  # noqa: E402

import hidden_desktop  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))


def make_bmp(size, seed, depth=24):
    rnd = random.Random(seed)
    pixel_bytes = depth // 8
    pixels = bytes(rnd.randrange(256) if depth == 24 or i % 4 != 3 else 0xFF
                   for i in range(size * size * pixel_bytes))
    return (b"BM" + struct.pack("<IHHI", 54 + len(pixels), 0, 0, 54) +
            struct.pack("<IiiHHIIiiII", 40, size, size, 1, depth, 0, len(pixels), 0, 0, 0, 0) + pixels)


class Check:
    def __init__(self, args):
        self.args = args
        self.out = os.path.abspath(args.out)
        self.server_dir = os.path.join(self.out, "server")
        if os.path.isdir(self.server_dir):
            shutil.rmtree(self.server_dir)
        self.system = os.path.join(self.server_dir, "system")
        self.banners = os.path.join(self.system, "ep3", "banners")
        self.outside = os.path.join(self.out, "picked")
        os.makedirs(self.banners)
        os.makedirs(self.outside, exist_ok=True)
        self.results = []
        self.failed = False
        self.dialogs = []
        self.answers = []

        # A server folder whose config.json is the example file with one entry in the list
        with open(os.path.join(REPO, "system", "config.example.json"), encoding="utf-8", newline="") as f:
            example = f.read()
        self.config_path = os.path.join(self.system, "config.json")
        text = jsonc.set_value(example, (lobby_media.KEY,), '[\n    [1, 0x00000003, "existing.bmp"],\n  ]')
        with open(self.config_path, "w", encoding="utf-8", newline="") as f:
            f.write(text)
        self.write(os.path.join(self.banners, "existing.bmp"), make_bmp(32, 1))
        self.write(os.path.join(self.outside, "new_banner.bmp"), make_bmp(64, 2))
        self.write(os.path.join(self.outside, "small_fallback.bmp"), make_bmp(16, 3))
        self.write(os.path.join(self.outside, "existing.bmp"), make_bmp(32, 4))  # same name, other picture
        rnd = random.Random(5)
        # Decompresses beyond what original clients take (0x36FFF), compresses well enough for Caelum
        self.write(os.path.join(self.outside, "pc_only.gvm"),
                   bytes(rnd.randrange(256) for _ in range(0x1000)) + bytes(0x3F000))

        settings_path = os.path.join(self.out, "admin-settings.json")
        with open(settings_path, "w", encoding="utf-8") as f:
            json.dump({"server_dir": self.server_dir, "server_exe": args.server_exe or "",
                       "extra_path": args.extra_path or ""}, f)
        options = app_module.parse_options(["--settings", settings_path, "--no-tray"])
        app_module.prepare_process()
        self.app = app_module.App(options)
        self.app.alert = lambda title, text, kind="error": self.dialogs.append(("alert", text))
        self.app.confirm = self.confirm
        self.app.open_path = lambda path: self.dialogs.append(("open", path))
        self.runner = self.steps()
        self.deadline = None
        self.waiting = None
        self.app.root.after(300, self.advance)

    @staticmethod
    def write(path, data):
        with open(path, "wb") as f:
            f.write(data)

    def confirm(self, title, text, default_yes=True):
        answer = self.answers.pop(0) if self.answers else False
        self.dialogs.append(("confirm", text, answer))
        return answer

    # ----- plumbing (as in gui_walkthrough.py)

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
                self.deadline = time.time() + 120
                self.app.root.after(50, self.advance)
            else:
                self.app.root.after(int(step), self.advance)
        except StopIteration:
            self.finish()
        except Exception:
            self.check("check ran to the end", False, traceback.format_exc())
            self.finish()

    def finish(self):
        with open(os.path.join(self.out, "results.json"), "w", encoding="utf-8") as f:
            json.dump({"failed": self.failed, "results": self.results, "dialogs": self.dialogs}, f, indent=2)
        self.app.finish_quit()

    def config_text(self):
        with open(self.config_path, encoding="utf-8", newline="") as f:
            return f.read()

    def config_list(self):
        return jsonc.find(jsonc.parse(self.config_text()), (lobby_media.KEY,)).to_python()

    def checked(self, panel):
        def condition():
            return not panel.checking()
        condition.__doc__ = "the lobby media page to finish checking"
        return condition

    # ----- the check

    def steps(self):
        settings = self.app.open_window("settings")
        yield 300
        panel = settings.panel_named("Episode 3 lobby media")
        settings.show_panel(panel)
        yield self.checked(panel)
        yield 100
        self.check("page: lists the entry from config.json", [e.as_tuple() for e in panel.entries()] ==
                   [(1, 3, "existing.bmp", None)], panel.entries())
        info = panel.items[0]["info"].cget("text")
        self.check("page: shows sizes and who sees the entry", "decompressed" in info and "All players" in info, info)
        self.check("page: the server accepts the list", "accepts" in panel.check_label.cget("text"),
                   panel.check_label.cget("text"))
        other = settings.panel_named("Other settings")
        self.check("other settings: the list is not offered twice", lobby_media.KEY not in other.keys)
        self.shot(settings, "lm_01_list")

        # Add an entry from a file outside the server folder, choose places with the tick boxes
        before = self.config_text()
        panel.add()
        item = panel.items[-1]
        ok = panel.set_file(item, "file", os.path.join(self.outside, "new_banner.bmp"))
        panel.toggle_places(item, item["box"])
        for (bit, _), var in zip(lobby_media.PLACES, item["place_vars"]):
            var.set(bit in (0x10, 0x20))
        panel.places_ticked(item)
        self.check("add: file chosen and places ticked", ok and item["places"].get() == "00000030" and
                   "copied in when saved" in item["file_label"].cget("text"), item["places"].get())
        yield self.checked(panel)
        yield 100
        panel.scroll.show_widget(item["box"])
        self.shot(settings, "lm_02_added_with_places")

        # A Caelum-only file with an original-client file that is too large: refused, nothing written
        panel.add()
        big = panel.items[-1]
        big["places"].set("00000800")
        panel.set_file(big, "file", os.path.join(self.outside, "pc_only.gvm"))
        panel.set_file(big, "fallback", os.path.join(self.outside, "pc_only.gvm"))
        yield self.checked(panel)
        saved = settings.save()
        yield 100
        status = settings.status.cget("text")
        self.check("refuse: an oversized original-client file is not saved",
                   not saved and "Not saved" in status and "original clients" in status and
                   self.config_text() == before and not os.path.exists(os.path.join(self.banners, "pc_only.gvm")),
                   status)
        errors = panel.items[-1]["errors"].cget("text")
        self.check("refuse: the entry names the problem", "decompresses to" in errors and
                   "limit for original clients" in errors, errors)
        panel.scroll.show_widget(panel.items[-1]["box"])
        self.shot(settings, "lm_03_refused")

        # A small original-client file instead: saved
        panel.set_file(panel.items[-1], "fallback", os.path.join(self.outside, "small_fallback.bmp"))
        yield self.checked(panel)
        audience = panel.items[-1]["info"].cget("text")
        compressed_known = self.args.server_exe and os.path.isfile(self.args.server_exe)
        if compressed_known:
            pc_line = panel.items[-1]["info"].cget("text").split("\n")[0]
            self.check("save: the compressed size comes from the server program", "compressed" in pc_line and
                       "not known" not in pc_line, pc_line)
        saved = settings.save()
        yield 100
        text = self.config_text()
        value = self.config_list()
        self.check("save: the list is written", saved and value == [
            [1, 3, "existing.bmp"], [1, 0x30, "new_banner.bmp"], [1, 0x800, "pc_only.gvm", "small_fallback.bmp"]],
            value)
        def without_list(config):
            node = jsonc.find(jsonc.parse(config), (lobby_media.KEY,))
            return config[:node.start] + config[node.end:]
        self.check("save: everything outside the list is unchanged", without_list(text) == without_list(before),
                   "%d characters" % len(without_list(text)))
        self.check("save: the chosen files are copied into the banners folder", all(
            os.path.isfile(os.path.join(self.banners, n)) for n in ("new_banner.bmp", "pc_only.gvm",
                                                                      "small_fallback.bmp")))
        status = settings.status.cget("text")
        self.check("save: the report says it applies without a restart",
                   "config-backups" in status and
                   ("Can be applied without a restart: Episode 3 lobby media" in status or
                    "The server uses the new values when it starts" in status), status)
        backups = os.listdir(self.app.settings.backup_dir)
        self.check("save: config.json backup kept", any(n.startswith("config.json.") for n in backups), backups)
        yield self.checked(panel)
        self.shot(settings, "lm_04_saved")

        # Replace the picture of the first entry with another file of the same name: asks, keeps a backup
        old = open(os.path.join(self.banners, "existing.bmp"), "rb").read()
        self.answers.append(True)
        ok = panel.set_file(panel.items[0], "file", os.path.join(self.outside, "existing.bmp"))
        self.check("replace: asks before replacing a different file of the same name",
                   ok and self.dialogs and self.dialogs[-1][0] == "confirm" and "entry 1" in self.dialogs[-1][1],
                   self.dialogs[-1:])
        yield self.checked(panel)
        saved = settings.save()
        yield 100
        new = open(os.path.join(self.banners, "existing.bmp"), "rb").read()
        kept = [n for n in os.listdir(self.app.settings.backup_dir) if n.startswith("existing.bmp.")]
        kept_data = open(os.path.join(self.app.settings.backup_dir, kept[0]), "rb").read() if kept else b""
        self.check("replace: the new file is in place and the old one is kept",
                   saved and new == open(os.path.join(self.outside, "existing.bmp"), "rb").read() and
                   kept_data == old, kept)
        self.check("replace: config.json is not rewritten for a file-only change",
                   len([n for n in os.listdir(self.app.settings.backup_dir) if n.startswith("config.json.")]) ==
                   len([n for n in backups if n.startswith("config.json.")]))
        asked = len(self.dialogs)
        ok = panel.set_file(panel.items[0], "file", os.path.join(self.outside, "existing.bmp"))
        self.check("replace: the same file again needs no question", ok and len(self.dialogs) == asked and
                   not panel.pending, self.dialogs[asked:])

        # Move and remove
        panel.move(2, -1)
        self.check("move: entries change places", [e.filename for e in panel.entries()] ==
                   ["existing.bmp", "pc_only.gvm", "new_banner.bmp"])
        panel.remove(1)
        yield self.checked(panel)
        saved = settings.save()
        yield 100
        value = self.config_list()
        self.check("remove: the entry leaves the list, its file stays", saved and value == [
            [1, 3, "existing.bmp"], [1, 0x30, "new_banner.bmp"]] and
            os.path.isfile(os.path.join(self.banners, "pc_only.gvm")), value)
        yield self.checked(panel)
        unused = panel.unused_label.cget("text")
        self.check("remove: the page names files no entry uses", "pc_only.gvm" in unused and
                   "small_fallback.bmp" in unused, unused)
        self.shot(settings, "lm_05_removed")

        # Wrong places and an entry without a file are refused
        panel.items[1]["places"].set("xyz")
        saved = settings.save()
        self.check("places: a wrong number is refused", not saved and "hexadecimal" in settings.status.cget("text"),
                   settings.status.cget("text"))
        panel.items[1]["places"].set("30")
        panel.add()
        saved = settings.save()
        self.check("add: an entry without a file is refused", not saved and "no file" in settings.status.cget("text"),
                   settings.status.cget("text"))
        settings.load()  # Undo my changes
        yield self.checked(panel)

        # A list the server would refuse is shown empty with the reason, and Save replaces it
        broken = jsonc.set_value(self.config_text(), (lobby_media.KEY,), "5")
        with open(self.config_path, "w", encoding="utf-8", newline="") as f:
            f.write(broken)
        settings.load()
        yield self.checked(panel)
        self.check("broken: the page explains and shows no entries", not panel.items and
                   "has a problem" in panel.error.cget("text"), panel.error.cget("text"))
        self.shot(settings, "lm_06_broken_list")
        panel.add()
        panel.set_file(panel.items[0], "file", os.path.join(self.banners, "new_banner.bmp"))
        yield self.checked(panel)
        saved = settings.save()
        yield 100
        self.check("broken: saving writes a correct list", saved and self.config_list() ==
                   [[1, 0x30, "new_banner.bmp"]], self.config_list())
        settings.close()
        self.check("no unexpected error dialog", not [d for d in self.dialogs if d[0] == "alert"],
                   [d for d in self.dialogs if d[0] == "alert"])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", required=True)
    parser.add_argument("--server-exe", default="")
    parser.add_argument("--extra-path", default="")
    args = parser.parse_args()
    check = Check(args)
    check.app.run()
    return 1 if check.failed else 0


if __name__ == "__main__":
    sys.exit(main())
