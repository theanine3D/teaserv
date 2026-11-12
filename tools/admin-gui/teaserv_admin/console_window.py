"""Console window: the server's live output and a line to send shell commands."""

import tkinter as tk
from tkinter import ttk

from . import server, ui

MAX_SHOWN = 6000


class ConsoleWindow(tk.Toplevel):
    def __init__(self, app):
        super().__init__(app.root)
        self.app = app
        self.title("Teaserv - Console")
        ui.center(self, 980, 600)
        self.minsize(560, 320)
        self.history = []
        self.history_index = 0
        self.shown = 0

        top = ttk.Frame(self, padding=(8, 8, 8, 4))
        top.pack(fill="x")
        self.dot = ui.StatusDot(top)
        self.dot.pack(side="left", padx=(0, 6))
        self.state_label = ttk.Label(top, text="")
        self.state_label.pack(side="left")
        buttons, self.update_buttons = ui.server_buttons(top, app)
        buttons.pack(side="right")
        self.follow = tk.BooleanVar(value=True)
        ttk.Checkbutton(top, text="Follow new lines", variable=self.follow).pack(side="right", padx=12)

        bottom = ttk.Frame(self, padding=(8, 4, 8, 8))
        bottom.pack(side="bottom", fill="x")
        ttk.Label(bottom, text="Command:").pack(side="left")
        self.entry = ttk.Entry(bottom, font=ui.mono_font())
        self.entry.pack(side="left", fill="x", expand=True, padx=6)
        self.entry.bind("<Return>", self.send)
        self.entry.bind("<Up>", lambda e: self.recall(-1))
        self.entry.bind("<Down>", lambda e: self.recall(1))
        self.send_button = ttk.Button(bottom, text="Send", command=self.send)
        self.send_button.pack(side="left")
        ttk.Button(bottom, text="List commands", command=lambda: self.run("help")).pack(side="left", padx=(6, 0))
        self.message = ttk.Label(self, text="", style="Warn.TLabel", padding=(8, 0))
        self.message.pack(side="bottom", fill="x")

        middle = ttk.Frame(self, padding=(8, 0))
        middle.pack(fill="both", expand=True)
        self.text = tk.Text(middle, wrap="none", font=ui.mono_font(), state="disabled", background="#ffffff",
                            foreground=ui.INK, undo=False)
        ybar = ttk.Scrollbar(middle, orient="vertical", command=self.text.yview)
        xbar = ttk.Scrollbar(middle, orient="horizontal", command=self.text.xview)
        self.text.configure(yscrollcommand=ybar.set, xscrollcommand=xbar.set)
        ybar.pack(side="right", fill="y")
        xbar.pack(side="bottom", fill="x")
        self.text.pack(fill="both", expand=True)
        self.text.tag_configure("W", foreground=ui.WARNING_TEXT)
        self.text.tag_configure("E", foreground=ui.ERROR_TEXT)
        self.text.tag_configure("D", foreground="#7a7974")
        self.text.tag_configure("admin", foreground="#1c5cab")
        self.text.tag_configure("cmd", foreground="#1c5cab", font=ui.mono_font() + ("bold",))
        self.text.tag_configure("out", foreground="#0b0b0b", background="#f3f6fb")

        self.add_lines(list(app.server.lines)[-MAX_SHOWN:])
        app.server.line_listeners.append(self.add_lines)
        app.server.state_listeners.append(self.on_state)
        self.on_state(app.server.state, "")
        self.protocol("WM_DELETE_WINDOW", self.close)
        self.entry.focus_set()

    def close(self):
        self.app.server.line_listeners.remove(self.add_lines)
        self.app.server.state_listeners.remove(self.on_state)
        self.app.window_closed("console")
        self.destroy()

    def on_state(self, state, detail):
        self.dot.set_state(state)
        self.state_label.configure(text=self.app.state_text())
        self.update_buttons(state)
        can_send = state == server.RUNNING
        self.send_button.configure(state="normal" if can_send else "disabled")

    def add_lines(self, lines):
        if not lines:
            return
        self.text.configure(state="normal")
        for line in lines:
            if line.kind == "log":
                tags = (line.level,) if line.level in ("W", "E", "D") else ()
            else:
                tags = (line.kind,) if line.level == "I" else (line.level,)
            self.text.insert("end", line.text + "\n", tags)
        self.shown += len(lines)
        if self.shown > MAX_SHOWN:
            extra = self.shown - MAX_SHOWN
            self.text.delete("1.0", "%d.0" % (extra + 1))
            self.shown = MAX_SHOWN
        self.text.configure(state="disabled")
        if self.follow.get():
            self.text.see("end")

    def run(self, command):
        problem = self.app.server.send_command(command)
        self.message.configure(text=problem)
        return not problem

    def send(self, event=None):
        command = self.entry.get().strip()
        if not command:
            return
        if self.run(command):
            self.history.append(command)
            self.history_index = len(self.history)
            self.entry.delete(0, "end")

    def recall(self, step):
        if not self.history:
            return "break"
        self.history_index = max(0, min(len(self.history), self.history_index + step))
        self.entry.delete(0, "end")
        if self.history_index < len(self.history):
            self.entry.insert(0, self.history[self.history_index])
        return "break"
