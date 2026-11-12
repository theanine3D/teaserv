"""The program itself: tray icon (Windows) or control window (elsewhere), the server process and the windows."""

import argparse
import hashlib
import os
import queue
import subprocess
import sys
import tkinter as tk
from tkinter import messagebox

from . import server, ui
from .appsettings import AppSettings, IS_WINDOWS, guess_server_dir

MENU_START, MENU_STOP, MENU_RESTART, MENU_CONSOLE, MENU_SETTINGS, MENU_LOGS, MENU_STATS, MENU_QUIT, MENU_STATUS = \
    range(1, 10)

TRAY_ICON = {
    server.RUNNING: "running",
    server.STOPPED: "stopped",
    server.STARTING: "busy",
    server.STOPPING: "busy",
    server.FAILED: "failed",
}


class App:
    def __init__(self, options):
        self.options = options
        self.settings = AppSettings(options.settings)
        if options.server_dir:
            self.settings["server_dir"] = os.path.abspath(options.server_dir)
        elif not self.settings.server_dir:
            guessed = guess_server_dir()
            if guessed:
                self.settings["server_dir"] = guessed
                self.settings.save()

        # Tk activates the first window it creates even when that window is hidden. A tray program must not pull the
        # keyboard away from whatever the person is doing, so activation is blocked until the hidden root exists.
        with NoActivation():
            self.root = tk.Tk()
            self.root.withdraw()
            self.root.title("Teaserv Admin")
            self.root.update()
        self.alive = True
        ui.setup_style(self.root)
        self.server = server.ServerProcess(self.settings)
        self.windows = {}
        self.pending_restart = []
        self.quitting = False
        self.failure_detail = ""
        self.tray = None
        if IS_WINDOWS and not options.no_tray:
            from .tray_win import TrayIcon
            tray = TrayIcon("Teaserv Admin", on_session_end=lambda: self.server.stop_and_wait(8))
            if tray.ok:
                self.tray = tray
            else:
                tray.close()

        self.server.state_listeners.append(self.on_state)
        self.server.stats_listeners.append(self.on_stats)
        self.server.remove_old_logs()
        self.update_tray()
        self.root.after(100, self.tick)

        if self.tray is None or options.show:
            self.open_window("control")
        problems = self.settings.problems()
        if problems:
            self.open_window("settings")
        elif options.start or self.settings["start_server_on_launch"]:
            self.start_server()

    # ----- messages (tests replace these)

    def alert(self, title, text, kind="error"):
        show = {"error": messagebox.showerror, "warning": messagebox.showwarning, "info": messagebox.showinfo}[kind]
        show(title, text, parent=self.dialog_parent())

    def confirm(self, title, text, default_yes=True):
        return messagebox.askyesno(title, text, parent=self.dialog_parent(),
                                   default=messagebox.YES if default_yes else messagebox.NO)

    def dialog_parent(self):
        for name in ("settings", "console", "control", "stats", "logs"):
            window = self.windows.get(name)
            if window is not None and window.winfo_viewable():
                return window
        return self.root

    def notify(self, title, text, kind="info"):
        if self.tray is not None:
            self.tray.notify(title, text, kind)

    # ----- text

    def players_text(self):
        live = self.server.stats
        if live is None:
            return ""
        count = live.get("players", 0)
        return "%d player%s online" % (count, "" if count == 1 else "s")

    def state_text(self):
        text = server.STATE_TEXT[self.server.state]
        players = self.players_text() if self.server.state == server.RUNNING else ""
        return text + (" - " + players if players else "")

    def detail_text(self):
        srv = self.server
        lines = []
        problems = self.settings.problems()
        if problems and not srv.is_active():
            lines.extend(problems)
        if srv.state == server.RUNNING:
            live = srv.stats
            if live is not None:
                lines.append("%s, %d open game%s. Running for %s." % (
                    self.players_text(), live.get("games", 0), "" if live.get("games", 0) == 1 else "s",
                    server.format_duration(srv.uptime_seconds())))
            else:
                lines.append("Running for %s." % server.format_duration(srv.uptime_seconds()))
        elif srv.state == server.STARTING:
            lines.append("Loading game data. This takes a few seconds.")
        elif srv.state == server.STOPPING:
            lines.append("Waiting for the server to finish and close.")
        elif srv.state == server.FAILED and self.failure_detail:
            lines.append(self.failure_detail)
        if self.pending_restart and srv.is_active():
            lines.append("Saved settings that need a restart: %s." % ", ".join(self.pending_restart))
        lines.append("Server folder: %s" % (self.settings.server_dir or "(not set)"))
        return "\n".join(lines)

    # ----- server actions

    def start_server(self):
        problem = self.server.start()
        if problem:
            self.alert("Teaserv Admin", "The server could not be started.\n\n%s" % problem)

    def players_online(self):
        live = self.server.stats
        return live.get("players", 0) if live else 0

    def stop_server(self):
        count = self.players_online()
        if count and not self.confirm("Stop the server", "%s. They will be disconnected.\n\nStop the server?"
                                      % self.players_text(), default_yes=False):
            return
        self.server.stop()

    def restart_server(self):
        count = self.players_online()
        if count and not self.confirm("Restart the server", "%s. They will be disconnected and have to log in "
                                      "again.\n\nRestart the server?" % self.players_text(), default_yes=False):
            return
        problem = self.server.restart()
        if problem:
            self.alert("Teaserv Admin", "The server could not be started.\n\n%s" % problem)

    def add_pending_restart(self, names):
        for name in names:
            if name not in self.pending_restart:
                self.pending_restart.append(name)
        self.refresh_control()

    def refresh_control(self):
        control = self.windows.get("control")
        if control is not None:
            control.refresh()

    def on_state(self, state, detail):
        if state == server.STARTING:
            self.pending_restart = []
            self.failure_detail = ""
        elif state == server.RUNNING:
            self.notify("Teaserv", "The server is running.")
        elif state == server.FAILED:
            self.failure_detail = detail
            self.notify("Teaserv", "The server stopped with an error.", "error")
            if not self.quitting:
                self.root.after(10, lambda: self.alert("Teaserv Admin", "The server stopped with an error.\n\n%s\n\n"
                                                       "The Console and the Logs window show more." % detail))
        elif state == server.STOPPING and detail == "timeout":
            self.root.after(10, self.ask_force_stop)
        self.update_tray()
        if self.quitting and state in (server.STOPPED, server.FAILED):
            self.root.after(10, self.finish_quit)

    def ask_force_stop(self):
        if self.server.state != server.STOPPING:
            return
        force = self.confirm(
            "The server is still stopping",
            "The server has not closed after %d seconds. It may still be saving.\n\n"
            "Close it by force? Files that are being written at that moment can be damaged.\n\n"
            "Yes: close it by force.\nNo: keep waiting (you are asked again later)."
            % self.settings["stop_timeout_seconds"], default_yes=False)
        if self.server.state != server.STOPPING:
            return
        if force:
            self.server.force_stop()
        else:
            import time
            self.server.stop_requested_at = time.time()
            self.server.stop_timeout_reported = False

    def on_stats(self, live):
        self.update_tray()

    # ----- tray

    def update_tray(self):
        if self.tray is None:
            return
        state = self.server.state
        active = state in (server.STARTING, server.RUNNING)
        text = self.state_text()
        if text.startswith("Server is "):
            text = "Teaserv: " + text[len("Server is "):]
        self.tray.set_state(TRAY_ICON[state], text)
        self.tray.set_menu([
            (MENU_STATUS, self.state_text(), False, False),
            None,
            (MENU_START, "Start the server", state in (server.STOPPED, server.FAILED), False),
            (MENU_STOP, "Stop the server", active, False),
            (MENU_RESTART, "Restart the server", active, False),
            None,
            (MENU_CONSOLE, "View console", True, True),
            (MENU_SETTINGS, "Settings", True, False),
            (MENU_LOGS, "Logs", True, False),
            (MENU_STATS, "Statistics", True, False),
            None,
            (MENU_QUIT, "Quit", True, False),
        ])

    def handle_tray(self, kind, value):
        if kind == "click":
            self.open_window("control")
        elif kind == "menu":
            actions = {
                MENU_START: self.start_server, MENU_STOP: self.stop_server, MENU_RESTART: self.restart_server,
                MENU_CONSOLE: lambda: self.open_window("console"),
                MENU_SETTINGS: lambda: self.open_window("settings"),
                MENU_LOGS: lambda: self.open_window("logs"), MENU_STATS: lambda: self.open_window("stats"),
                MENU_QUIT: self.quit,
            }
            action = actions.get(value)
            if action:
                action()

    # ----- windows

    def open_window(self, name):
        window = self.windows.get(name)
        if window is not None:
            window.deiconify()
            window.lift()
            window.focus_force()
            return window
        if name == "console":
            from .console_window import ConsoleWindow as Window
        elif name == "settings":
            from .settings_window import SettingsWindow as Window
        elif name == "logs":
            from .logs_window import LogsWindow as Window
        elif name == "stats":
            from .stats_window import StatsWindow as Window
        else:
            from .control_window import ControlWindow as Window
        window = Window(self)
        self.windows[name] = window
        # Opened on the admin's request (tray menu, button, program start): bring it to the front
        window.lift()
        window.focus_force()
        return window

    def window_closed(self, name):
        self.windows.pop(name, None)

    def open_path(self, path):
        """Show a file or folder with the system's own program."""
        if os.path.isdir(os.path.dirname(path)) and not os.path.exists(path):
            path = os.path.dirname(path)
        try:
            if IS_WINDOWS:
                os.startfile(path)
            else:
                subprocess.Popen(["xdg-open", path])
        except OSError as e:
            self.alert("Teaserv Admin", "Cannot open %s\n\n%s" % (path, e))

    # ----- main loop

    def tick(self):
        try:
            self.server.poll()
            while self.tray is not None:  # Quit from the menu ends the tray inside this loop
                try:
                    kind, value = self.tray.events.get_nowait()
                except queue.Empty:
                    break
                self.handle_tray(kind, value)
        finally:
            if self.alive:
                self.root.after(100, self.tick)

    def quit(self):
        if self.server.is_active():
            if self.server.state != server.STOPPING:
                text = "The server is running. Quitting stops it."
                if self.players_online():
                    text += " %s and will be disconnected." % self.players_text().capitalize().replace(
                        " online", " are online")
                if not self.confirm("Quit Teaserv Admin", text + "\n\nStop the server and quit?",
                                    default_yes=not self.players_online()):
                    return
                self.server.stop()
            self.quitting = True
            return
        self.finish_quit()

    def finish_quit(self):
        if self.tray is not None:
            self.tray.close()
            self.tray = None
        self.server.close()
        self.alive = False
        self.root.destroy()

    def run(self):
        self.root.mainloop()


class NoActivation:
    """While active, no window of this thread can become the active window (Windows; elsewhere it does nothing).

    Uses a thread-local CBT hook: Windows asks it before activating a window, and a non-zero answer refuses.
    """

    HCBT_ACTIVATE = 5
    WH_CBT = 5

    def __enter__(self):
        self.hook = None
        if not IS_WINDOWS:
            return self
        import ctypes
        from ctypes import wintypes
        user32 = ctypes.WinDLL("user32", use_last_error=True)
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        result_type = ctypes.c_ssize_t
        self.prototype = ctypes.WINFUNCTYPE(result_type, ctypes.c_int, wintypes.WPARAM, wintypes.LPARAM)
        user32.CallNextHookEx.argtypes = [wintypes.HANDLE, ctypes.c_int, wintypes.WPARAM, wintypes.LPARAM]
        user32.CallNextHookEx.restype = result_type
        user32.SetWindowsHookExW.argtypes = [ctypes.c_int, self.prototype, wintypes.HINSTANCE, wintypes.DWORD]
        user32.SetWindowsHookExW.restype = wintypes.HANDLE
        user32.UnhookWindowsHookEx.argtypes = [wintypes.HANDLE]
        kernel32.GetCurrentThreadId.restype = wintypes.DWORD

        def procedure(code, wparam, lparam):
            if code == self.HCBT_ACTIVATE:
                return 1
            return user32.CallNextHookEx(None, code, wparam, lparam)

        self.user32 = user32
        self.procedure = self.prototype(procedure)  # keep a reference while the hook is installed
        self.hook = user32.SetWindowsHookExW(self.WH_CBT, self.procedure, None, kernel32.GetCurrentThreadId())
        return self

    def __exit__(self, *exc):
        if self.hook:
            self.user32.UnhookWindowsHookEx(self.hook)
            self.hook = None
        return False


def single_instance(settings_path):
    """True when no other copy of the program uses the same settings file."""
    name = "TeaservAdmin-" + hashlib.sha1(os.path.abspath(settings_path).lower().encode("utf-8")).hexdigest()[:16]
    if IS_WINDOWS:
        import ctypes
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.CreateMutexW.restype = ctypes.c_void_p
        handle = kernel32.CreateMutexW(None, False, name)
        single_instance.handle = handle  # held until the program ends
        return ctypes.get_last_error() != 183  # ERROR_ALREADY_EXISTS
    import fcntl
    lock_path = os.path.join(os.path.dirname(os.path.abspath(settings_path)) or ".", name + ".lock")
    os.makedirs(os.path.dirname(lock_path), exist_ok=True)
    single_instance.handle = open(lock_path, "w")
    try:
        fcntl.flock(single_instance.handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        return True
    except OSError:
        return False


def prepare_process():
    if not IS_WINDOWS:
        return
    import ctypes
    try:
        # No "DLL not found" box from Windows when the server cannot start: the reason goes to the log instead.
        # The server process inherits this.
        ctypes.windll.kernel32.SetErrorMode(0x0001 | 0x0002 | 0x8000)
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except (OSError, AttributeError):
        pass


def parse_options(argv=None):
    parser = argparse.ArgumentParser(prog="teaserv-admin", description="Tray program for a Teaserv server.")
    parser.add_argument("--server-dir", help="server folder (the one that holds the system folder)")
    parser.add_argument("--settings", help="settings file of this program (default: in your user profile)")
    parser.add_argument("--start", action="store_true", help="start the server right away")
    parser.add_argument("--show", action="store_true", help="open the status window at start")
    parser.add_argument("--no-tray", action="store_true", help="do not use the tray; show the status window")
    return parser.parse_args(argv)


def main(argv=None):
    options = parse_options(argv)
    prepare_process()
    settings_path = options.settings or AppSettings().path
    if not single_instance(settings_path):
        root = tk.Tk()
        root.withdraw()
        messagebox.showinfo("Teaserv Admin", "Teaserv Admin is already running. Look for its icon in the tray "
                            "(the arrow next to the clock shows hidden icons).")
        return 1
    App(options).run()
    return 0
