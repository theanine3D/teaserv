"""Test (Linux): the program without a tray, on an X display given by DISPLAY (use Xvfb to keep it invisible).

    DISPLAY=:77 python3 linux_smoke.py --server-dir DIR --out DIR [--server-exe FILE] [--extra-path FOLDERS]

Opens every window, starts the server, sends a command, stops the server and quits. Pictures go to <out>/linux_*.png
(needs Pillow with X support). No network checks: under WSL the server may be a Windows program whose ports the
Linux side cannot reach.
"""

import argparse
import json
import os
import sys
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

from teaserv_admin import app as app_module  # noqa: E402
from teaserv_admin import server  # noqa: E402


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server-dir", required=True)
    parser.add_argument("--server-exe", default="")
    parser.add_argument("--extra-path", default="")
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)
    settings_path = os.path.join(args.out, "admin-settings-linux.json")
    with open(settings_path, "w", encoding="utf-8") as f:
        json.dump({"server_dir": os.path.abspath(args.server_dir), "server_exe": args.server_exe,
                   "extra_path": args.extra_path, "stats_poll_seconds": 2}, f)

    results = []
    dialogs = []

    def check(name, ok, detail=""):
        results.append({"check": name, "ok": bool(ok), "detail": str(detail)})
        print("%s  %s  %s" % ("PASS" if ok else "FAIL", name, detail), flush=True)

    app = app_module.App(app_module.parse_options(["--settings", settings_path]))
    app.alert = lambda title, text, kind="error": dialogs.append(text)
    app.confirm = lambda title, text, default_yes=True: dialogs.append(text) or True
    app.open_path = lambda path: None
    srv = app.server
    ended = []
    real_finish = app.finish_quit
    app.finish_quit = lambda: ended.append(True)

    def shot(window, name):
        from PIL import ImageGrab
        window.lift()
        window.update()
        time.sleep(0.2)
        window.update()
        box = (window.winfo_rootx(), window.winfo_rooty(), window.winfo_rootx() + window.winfo_width(),
               window.winfo_rooty() + window.winfo_height())
        image = ImageGrab.grab(bbox=box, xdisplay=os.environ["DISPLAY"])
        image.save(os.path.join(args.out, "linux_%s.png" % name))
        check("picture " + name, len(set(image.resize((32, 32)).getdata())) > 3, "%dx%d" % image.size)

    def steps():
        check("no tray on this system: the status window is the main window",
              app.tray is None and "control" in app.windows)
        control = app.windows["control"]
        yield 300
        shot(control, "status_stopped")
        app.start_server()
        yield lambda: srv.state == server.RUNNING and srv.stats is not None
        check("start: the server runs and reports numbers", srv.shell_ready, "process %s" % srv.pid)
        yield 300
        shot(control, "status_running")
        console = app.open_window("console")
        before = srv.seq
        console.entry.insert(0, "help")
        console.send()
        yield lambda: any("Shut down the server." in line.text for line in srv.lines if line.seq > before)
        check("console: command reply shown", True)
        shot(console, "console")
        console.close()
        for name in ("settings", "logs", "stats"):
            window = app.open_window(name)
            yield 500
            shot(window, name)
            window.close()
        proc = srv.proc
        app.quit()
        while proc.poll() is None:
            yield 200
        yield lambda: bool(ended)
        check("quit: the server shut down by itself, then the program ends", proc.returncode == 0 and any(
            "Normal shutdown" in line.text for line in srv.lines), "exit code %s" % proc.returncode)
        check("no error dialog", not [d for d in dialogs if "Stop the server and quit?" not in d], dialogs)

    runner = steps()
    state = {"waiting": None, "deadline": 0}

    def advance():
        try:
            if state["waiting"] is not None:
                if not state["waiting"]():
                    if time.time() > state["deadline"]:
                        raise RuntimeError("timed out")
                    app.root.after(100, advance)
                    return
                state["waiting"] = None
            step = next(runner)
            if callable(step):
                state["waiting"], state["deadline"] = step, time.time() + 90
                app.root.after(50, advance)
            else:
                app.root.after(int(step), advance)
        except StopIteration:
            finish()
        except Exception:
            check("smoke test ran to the end", False, traceback.format_exc())
            finish()

    def finish():
        if srv.is_active():
            srv.stop_and_wait(20)
        with open(os.path.join(args.out, "linux-results.json"), "w", encoding="utf-8") as f:
            json.dump(results, f, indent=2)
        real_finish()

    app.root.after(500, advance)
    app.run()
    return 0 if all(r["ok"] for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
