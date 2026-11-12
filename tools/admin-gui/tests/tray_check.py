"""Test (Windows): the tray icon with a real server, without opening any window.

    python tray_check.py --server-dir DIR --out DIR [--server-exe FILE] [--extra-path FOLDERS]

Runs on the normal desktop, because the tray belongs to it. It adds the real icon for about 20 seconds, feeds the menu
choices Start, Restart, Stop and Quit to the program the way a click on the menu does, and checks the icon state after
each. It opens no window, shows no notification and never takes the focus. What it cannot do is click: opening the
menu with the mouse stays a check for a person.
"""

import argparse
import ctypes
import json
import os
import sys
import time
import traceback
from ctypes import wintypes

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

from teaserv_admin import app as app_module  # noqa: E402
from teaserv_admin import server, tray_win  # noqa: E402


class NOTIFYICONIDENTIFIER(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.DWORD), ("hWnd", wintypes.HWND), ("uID", wintypes.UINT),
                ("guidItem", tray_win.GUID)]


def icon_known_to_shell(hwnd):
    """Does Windows have a place for this program's tray icon? (It may be among the hidden icons.)"""
    identifier = NOTIFYICONIDENTIFIER(ctypes.sizeof(NOTIFYICONIDENTIFIER), hwnd, 1)
    rect = wintypes.RECT()
    get_rect = tray_win.shell32.Shell_NotifyIconGetRect
    get_rect.argtypes = [ctypes.POINTER(NOTIFYICONIDENTIFIER), ctypes.POINTER(wintypes.RECT)]
    get_rect.restype = ctypes.c_long
    return get_rect(ctypes.byref(identifier), ctypes.byref(rect)) == 0, (rect.left, rect.top, rect.right,
                                                                         rect.bottom)


def menu_labels(tray):
    user32 = tray_win.user32
    user32.GetMenuItemCount.argtypes = [wintypes.HMENU]
    user32.GetMenuStringW.argtypes = [wintypes.HMENU, wintypes.UINT, wintypes.LPWSTR, ctypes.c_int, wintypes.UINT]
    user32.GetMenuState.argtypes = [wintypes.HMENU, wintypes.UINT, wintypes.UINT]
    menu = tray.build_menu()
    labels = []
    for position in range(user32.GetMenuItemCount(menu)):
        text = ctypes.create_unicode_buffer(200)
        user32.GetMenuStringW(menu, position, text, 200, 0x400)  # by position
        state = user32.GetMenuState(menu, position, 0x400)
        if text.value:
            labels.append((text.value, not state & tray_win.MF_GRAYED))
    user32.DestroyMenu(menu)
    return labels


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server-dir", required=True)
    parser.add_argument("--server-exe", default="")
    parser.add_argument("--extra-path", default="")
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)
    settings_path = os.path.join(args.out, "admin-settings-tray.json")
    with open(settings_path, "w", encoding="utf-8") as f:
        json.dump({"server_dir": os.path.abspath(args.server_dir), "server_exe": args.server_exe,
                   "extra_path": args.extra_path, "stats_poll_seconds": 2}, f)

    user32 = ctypes.WinDLL("user32")
    user32.GetForegroundWindow.restype = wintypes.HWND
    focus_taken = []
    focus_checks = [0]
    started = time.time()

    def watch_focus():
        hwnd = user32.GetForegroundWindow()
        owner = wintypes.DWORD(0)
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        focus_checks[0] += 1
        if owner.value == os.getpid():
            name = ctypes.create_unicode_buffer(100)
            user32.GetClassNameW(hwnd, name, 100)
            focus_taken.append("%.1f s: %s" % (time.time() - started, name.value))

    results = []
    dialogs = []
    balloons = []

    def check(name, ok, detail=""):
        results.append({"check": name, "ok": bool(ok), "detail": str(detail)})
        print("%s  %s  %s" % ("PASS" if ok else "FAIL", name, detail), flush=True)

    app_module.prepare_process()
    app = app_module.App(app_module.parse_options(["--settings", settings_path]))
    app.alert = lambda title, text, kind="error": dialogs.append(text)
    app.confirm = lambda title, text, default_yes=True: dialogs.append(text) or True
    app.notify = lambda title, text, kind="info": balloons.append(text)
    srv = app.server
    real_finish = app.finish_quit
    ended = []
    app.finish_quit = lambda: ended.append(True)

    def steps():
        tray = app.tray
        check("tray: icon added to the notification area", tray is not None and tray.ok)
        if tray is None:
            return
        check("tray: no window was opened", not app.windows, list(app.windows))
        known, rect = icon_known_to_shell(tray.hwnd)
        check("tray: Windows reports a place for the icon", known, rect)
        check("tray: icon shows 'stopped'", tray.icon_name == "stopped", tray.tooltip)
        labels = menu_labels(tray)
        check("tray: menu has every entry", [text for text, _ in labels] == [
            "Server is stopped", "Start the server", "Stop the server", "Restart the server", "View console",
            "Settings", "Logs", "Statistics", "Quit"], [text for text, _ in labels])
        check("tray: while stopped, only Start can be chosen of the three server entries",
              dict(labels)["Start the server"] and not dict(labels)["Stop the server"] and
              not dict(labels)["Restart the server"])

        tray.events.put(("menu", app_module.MENU_START))
        yield lambda: srv.state == server.RUNNING and srv.stats is not None
        yield 300
        check("tray menu Start: server runs, icon shows 'running'", tray.icon_name == "running", tray.tooltip)
        labels = dict(menu_labels(tray))
        check("tray: while running, Stop and Restart can be chosen, Start cannot",
              labels["Stop the server"] and labels["Restart the server"] and not labels["Start the server"])

        pid = srv.pid
        tray.events.put(("menu", app_module.MENU_RESTART))
        yield lambda: srv.state == server.RUNNING and srv.pid not in (None, pid)
        check("tray menu Restart: a new server process runs", True, "process %s -> %s" % (pid, srv.pid))

        proc = srv.proc
        tray.events.put(("menu", app_module.MENU_STOP))
        yield lambda: srv.state == server.STOPPED
        yield 300
        check("tray menu Stop: server ended by itself, icon shows 'stopped'",
              proc.returncode == 0 and tray.icon_name == "stopped", "exit code %s; %s" % (proc.returncode,
                                                                                          tray.tooltip))
        tray.events.put(("menu", app_module.MENU_QUIT))
        yield lambda: bool(ended)
        check("tray menu Quit: the program ends", True)
        check("tray: notifications the program would have shown", len(balloons) == 2, balloons)
        check("tray: no error dialog", not dialogs, dialogs)
        # The person at the PC may switch windows during the test; what must not happen is that the focus is, at
        # any moment, in a window of this program (watched every 50 ms from before the program started)
        check("tray: the program never took the focus", not focus_taken,
              "%d checks; %s" % (focus_checks[0], focus_taken[:3] or "never in a window of this program"))

    runner = steps()
    state = {"waiting": None, "deadline": 0}

    def watch_loop():
        watch_focus()
        if not ended:
            app.root.after(50, watch_loop)

    watch_focus()
    app.root.after(1, watch_loop)

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
                state["waiting"], state["deadline"] = step, time.time() + 60
                app.root.after(50, advance)
            else:
                app.root.after(int(step), advance)
        except StopIteration:
            finish()
        except Exception:
            check("tray check ran to the end", False, traceback.format_exc())
            finish()

    def finish():
        if srv.is_active():
            srv.stop_and_wait(20)
        with open(os.path.join(args.out, "tray-results.json"), "w", encoding="utf-8") as f:
            json.dump(results, f, indent=2)
        real_finish()

    app.root.after(500, advance)
    app.run()
    return 0 if all(r["ok"] for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
