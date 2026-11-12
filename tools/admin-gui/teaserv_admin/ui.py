"""Shared window pieces: colours, fonts, a scrolling frame and the status dot."""

import sys
import tkinter as tk
from tkinter import ttk

from . import server

# Chart and status colours (validated categorical order; status colours are kept apart from series colours)
SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK_SECONDARY = "#52514e"
GRID = "#e6e5e0"
SERIES = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100"]
STATE_COLORS = {
    server.RUNNING: "#0ca30c",
    server.STOPPED: "#80868e",
    server.STARTING: "#db9a04",
    server.STOPPING: "#db9a04",
    server.FAILED: "#d03b3b",
}
ERROR_TEXT = "#b3261e"
WARNING_TEXT = "#8a5a00"
NOTE_TEXT = "#52514e"


def setup_style(root):
    style = ttk.Style(root)
    if sys.platform != "win32":
        try:
            style.theme_use("clam")
        except tk.TclError:
            pass
    style.configure("Title.TLabel", font=("Segoe UI", 11, "bold") if sys.platform == "win32" else ("TkDefaultFont", 11, "bold"))
    style.configure("Field.TLabel", font=("Segoe UI", 9, "bold") if sys.platform == "win32" else ("TkDefaultFont", 9, "bold"))
    style.configure("Help.TLabel", foreground=NOTE_TEXT)
    style.configure("Error.TLabel", foreground=ERROR_TEXT)
    style.configure("Warn.TLabel", foreground=WARNING_TEXT)
    style.configure("Tile.TFrame", background="#ffffff")
    style.configure("TileValue.TLabel", background="#ffffff", font=("Segoe UI", 18) if sys.platform == "win32" else ("TkDefaultFont", 18))
    style.configure("TileName.TLabel", background="#ffffff", foreground=NOTE_TEXT)
    return style


def mono_font():
    return ("Consolas", 10) if sys.platform == "win32" else ("TkFixedFont", 10)


class StatusDot(tk.Canvas):
    """A small coloured disc that shows the server state."""

    def __init__(self, parent, size=14, **kw):
        background = kw.pop("background", None) or ttk.Style().lookup("TFrame", "background") or "#f0f0f0"
        super().__init__(parent, width=size, height=size, highlightthickness=0, background=background, **kw)
        self.dot = self.create_oval(1, 1, size - 1, size - 1, outline="", fill=STATE_COLORS[server.STOPPED])

    def set_state(self, state):
        self.itemconfigure(self.dot, fill=STATE_COLORS.get(state, STATE_COLORS[server.STOPPED]))


class ScrollFrame(ttk.Frame):
    """A frame whose content (self.body) scrolls vertically."""

    def __init__(self, parent, **kw):
        super().__init__(parent, **kw)
        background = ttk.Style().lookup("TFrame", "background") or "#f0f0f0"
        self.canvas = tk.Canvas(self, highlightthickness=0, background=background)
        self.bar = ttk.Scrollbar(self, orient="vertical", command=self.canvas.yview)
        self.canvas.configure(yscrollcommand=self.bar.set)
        self.bar.pack(side="right", fill="y")
        self.canvas.pack(side="left", fill="both", expand=True)
        self.body = ttk.Frame(self.canvas)
        self.window = self.canvas.create_window(0, 0, anchor="nw", window=self.body)
        self.body.bind("<Configure>", lambda e: self.canvas.configure(scrollregion=self.canvas.bbox("all")))
        self.canvas.bind("<Configure>", lambda e: self.canvas.itemconfigure(self.window, width=e.width))
        self.canvas.bind("<Enter>", lambda e: self.canvas.bind_all("<MouseWheel>", self._wheel))
        self.canvas.bind("<Leave>", lambda e: self.canvas.unbind_all("<MouseWheel>"))
        self.canvas.bind("<Button-4>", lambda e: self.canvas.yview_scroll(-3, "units"))
        self.canvas.bind("<Button-5>", lambda e: self.canvas.yview_scroll(3, "units"))

    def _wheel(self, event):
        if self.body.winfo_height() > self.canvas.winfo_height():
            self.canvas.yview_scroll(-1 if event.delta > 0 else 1, "units")

    def to_top(self):
        self.canvas.yview_moveto(0)

    def show_widget(self, widget):
        """Scroll so that a widget inside the body is visible."""
        self.update_idletasks()
        total = max(1, self.body.winfo_height())
        y = widget.winfo_rooty() - self.body.winfo_rooty()
        self.canvas.yview_moveto(max(0, (y - 40) / total))


def center(window, width, height):
    window.update_idletasks()
    x = max(0, (window.winfo_screenwidth() - width) // 2)
    y = max(0, (window.winfo_screenheight() - height) // 3)
    window.geometry("%dx%d+%d+%d" % (width, height, x, y))


def place(window):
    """Put a window that sizes itself to its content in the upper middle of the screen."""
    window.update_idletasks()
    x = max(0, (window.winfo_screenwidth() - window.winfo_reqwidth()) // 2)
    y = max(0, (window.winfo_screenheight() - window.winfo_reqheight()) // 3)
    window.geometry("+%d+%d" % (x, y))


def server_buttons(parent, app):
    """Start / Stop / Restart buttons that follow the server state. Returns (frame, update function)."""
    frame = ttk.Frame(parent)
    start = ttk.Button(frame, text="Start", command=app.start_server, width=9)
    stop = ttk.Button(frame, text="Stop", command=app.stop_server, width=9)
    restart = ttk.Button(frame, text="Restart", command=app.restart_server, width=9)
    for button in (start, stop, restart):
        button.pack(side="left", padx=(0, 6))

    def update(state):
        active = state in (server.STARTING, server.RUNNING)
        start.configure(state="disabled" if state in (server.STARTING, server.RUNNING, server.STOPPING) else "normal")
        stop.configure(state="normal" if active else "disabled")
        restart.configure(state="normal" if active else "disabled")

    return frame, update
