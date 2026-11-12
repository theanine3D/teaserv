"""Small status window: server state, Start / Stop / Restart and the other windows.

On Windows it opens with a left click on the tray icon. Where there is no tray (Linux), it is the program's main
window.
"""

import tkinter as tk
from tkinter import ttk

from . import server, ui


class ControlWindow(tk.Toplevel):
    def __init__(self, app):
        super().__init__(app.root)
        self.app = app
        self.title("Teaserv Admin")
        self.resizable(False, False)

        body = ttk.Frame(self, padding=14)
        body.pack(fill="both", expand=True)
        head = ttk.Frame(body)
        head.pack(fill="x")
        self.dot = ui.StatusDot(head, size=18)
        self.dot.pack(side="left", padx=(0, 8))
        self.state_label = ttk.Label(head, text="", style="Title.TLabel")
        self.state_label.pack(side="left")
        self.detail = ttk.Label(body, text="", style="Help.TLabel", wraplength=400, justify="left")
        self.detail.pack(anchor="w", pady=(6, 10))

        buttons, self.update_buttons = ui.server_buttons(body, app)
        buttons.pack(anchor="w")
        ttk.Separator(body).pack(fill="x", pady=12)
        windows = ttk.Frame(body)
        windows.pack(anchor="w")
        for column, (label, name) in enumerate((("Console", "console"), ("Settings", "settings"), ("Logs", "logs"),
                                                 ("Statistics", "stats"))):
            ttk.Button(windows, text=label, width=11, command=lambda n=name: app.open_window(n)).grid(
                row=0, column=column, padx=(0, 6))
        ttk.Separator(body).pack(fill="x", pady=12)
        foot = ttk.Frame(body)
        foot.pack(fill="x")
        ttk.Button(foot, text="Quit", command=app.quit).pack(side="right")
        if app.tray is not None:
            ttk.Label(foot, text="Closing this window keeps the program in the tray.", style="Help.TLabel").pack(
                side="left")

        app.server.state_listeners.append(self.on_state)
        app.server.stats_listeners.append(self.on_stats)
        self.protocol("WM_DELETE_WINDOW", self.close)
        self.refresh()
        ui.place(self)

    def close(self):
        if self.app.tray is None:
            # No tray: this is the main window, so it is minimized, not closed
            self.iconify()
            return
        self.app.server.state_listeners.remove(self.on_state)
        self.app.server.stats_listeners.remove(self.on_stats)
        self.app.window_closed("control")
        self.destroy()

    def on_state(self, state, detail):
        self.refresh()

    def on_stats(self, live):
        self.refresh()

    def refresh(self):
        srv = self.app.server
        self.dot.set_state(srv.state)
        self.state_label.configure(text=server.STATE_TEXT[srv.state])
        self.update_buttons(srv.state)
        self.detail.configure(text=self.app.detail_text())
