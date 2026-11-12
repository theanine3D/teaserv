"""Logs window: the saved server logs (one file per day) with a level filter and text search."""

import datetime
import os
import re
import tkinter as tk
from tkinter import ttk

from . import server, ui

LEVEL_CHOICES = [
    ("Everything", 0),
    ("Information and above", 1),
    ("Warnings and errors", 2),
    ("Errors only", 3),
]
MAX_SHOWN = 20000
MAX_BYTES = 20 * 1024 * 1024


def read_log_file(path):
    """[(level, text)] for a saved log. Lines without a level of their own take the level of the line above."""
    lines = []
    level = "I"
    try:
        with open(path, "rb") as binary:
            # A day with raw network commands logged can be very large: only its newest part is loaded
            binary.seek(0, os.SEEK_END)
            size = binary.tell()
            start = max(0, size - MAX_BYTES)
            binary.seek(start)
            if start:
                binary.readline()  # the rest of a line that was cut in two
            data = binary.read()
        if start:
            lines.append(("W", "(Only the newest %d MB of this day's log are shown. The whole file: %s)"
                          % (MAX_BYTES // (1024 * 1024), path)))
        text_lines = data.decode("utf-8", "replace").split("\n")
        if text_lines and text_lines[-1] == "":
            text_lines.pop()  # the file ends with a line end
        for raw in text_lines:
            text = raw.rstrip("\r")
            line_level = server.line_level(text, level)
            if server.split_log_line(text):
                level = line_level
            lines.append((line_level, text))
    except OSError:
        pass
    return lines


def filter_lines(lines, minimum, needle):
    """Lines at or above a level (0 = all) that contain the search text (upper/lower case ignored)."""
    needle = needle.lower()
    return [(level, text) for level, text in lines
            if server.LEVEL_ORDER.get(level, 1) >= minimum and (not needle or needle in text.lower())]


class LogsWindow(tk.Toplevel):
    def __init__(self, app):
        super().__init__(app.root)
        self.app = app
        self.title("Teaserv - Logs")
        ui.center(self, 1000, 620)
        self.minsize(600, 320)
        self.lines = []
        self.files = []
        self.search_job = None

        top = ttk.Frame(self, padding=8)
        top.pack(fill="x")
        ttk.Label(top, text="Day:").pack(side="left")
        self.day = ttk.Combobox(top, state="readonly", width=22)
        self.day.pack(side="left", padx=(4, 12))
        self.day.bind("<<ComboboxSelected>>", lambda e: self.load())
        ttk.Label(top, text="Show:").pack(side="left")
        self.level = ttk.Combobox(top, state="readonly", width=24, values=[name for name, _ in LEVEL_CHOICES])
        self.level.current(0)
        self.level.pack(side="left", padx=(4, 12))
        self.level.bind("<<ComboboxSelected>>", lambda e: self.refresh())
        ttk.Label(top, text="Search:").pack(side="left")
        self.search = tk.StringVar()
        entry = ttk.Entry(top, textvariable=self.search, width=28)
        entry.pack(side="left", padx=(4, 4))
        self.search.trace_add("write", lambda *a: self.search_changed())
        ttk.Button(top, text="Clear", command=lambda: self.search.set(""), width=6).pack(side="left")
        ttk.Button(top, text="Open folder", command=self.open_folder).pack(side="right")
        ttk.Button(top, text="Reload", command=self.reload_files).pack(side="right", padx=6)

        self.count = ttk.Label(self, text="", style="Help.TLabel", padding=(8, 0, 8, 6))
        self.count.pack(side="bottom", fill="x")

        middle = ttk.Frame(self, padding=(8, 0))
        middle.pack(fill="both", expand=True)
        self.text = tk.Text(middle, wrap="none", font=ui.mono_font(), state="disabled", background="#ffffff",
                            foreground=ui.INK)
        ybar = ttk.Scrollbar(middle, orient="vertical", command=self.text.yview)
        xbar = ttk.Scrollbar(middle, orient="horizontal", command=self.text.xview)
        self.text.configure(yscrollcommand=ybar.set, xscrollcommand=xbar.set)
        ybar.pack(side="right", fill="y")
        xbar.pack(side="bottom", fill="x")
        self.text.pack(fill="both", expand=True)
        self.text.tag_configure("W", foreground=ui.WARNING_TEXT)
        self.text.tag_configure("E", foreground=ui.ERROR_TEXT)
        self.text.tag_configure("D", foreground="#7a7974")
        self.text.tag_configure("hit", background="#ffe9a8")

        app.server.line_listeners.append(self.on_new_lines)
        self.protocol("WM_DELETE_WINDOW", self.close)
        self.reload_files()

    def close(self):
        self.app.server.line_listeners.remove(self.on_new_lines)
        self.app.window_closed("logs")
        self.destroy()

    # ----- files

    def reload_files(self):
        log_dir = self.app.settings.log_dir
        names = []
        if os.path.isdir(log_dir):
            names = sorted((n for n in os.listdir(log_dir) if re.match(r"^teaserv-\d{4}-\d\d-\d\d\.log$", n)),
                           reverse=True)
        today = "teaserv-%s.log" % datetime.date.today().isoformat()
        if today not in names:
            names.insert(0, today)
        self.files = names
        labels = []
        for name in names:
            day = name[len("teaserv-"):-len(".log")]
            labels.append(day + (" (today, live)" if name == today else ""))
        selected = self.day.current()
        self.day.configure(values=labels)
        self.day.current(selected if 0 <= selected < len(labels) else 0)
        self.load()

    def is_today(self):
        return self.files and self.files[self.day.current()] == "teaserv-%s.log" % datetime.date.today().isoformat()

    def load(self):
        path = os.path.join(self.app.settings.log_dir, self.files[self.day.current()])
        self.lines = read_log_file(path)
        self.refresh()

    def open_folder(self):
        self.app.open_path(self.app.settings.log_dir)

    # ----- filter

    def minimum(self):
        return LEVEL_CHOICES[self.level.current()][1]

    def search_changed(self):
        if self.search_job:
            self.after_cancel(self.search_job)
        self.search_job = self.after(250, self.refresh)

    def refresh(self):
        self.search_job = None
        needle = self.search.get().strip()
        matching = filter_lines(self.lines, self.minimum(), needle)
        shown = matching[-MAX_SHOWN:]
        self.text.configure(state="normal")
        self.text.delete("1.0", "end")
        for level, text in shown:
            self.text.insert("end", text + "\n", (level,) if level in ("W", "E", "D") else ())
        self.highlight(needle, "1.0")
        self.text.configure(state="disabled")
        self.text.see("end")
        note = "Showing %d of %d lines" % (len(shown), len(self.lines))
        if len(matching) > len(shown):
            note += " (the newest %d that match; narrow the search to see older ones)" % MAX_SHOWN
        if not self.lines:
            note = "No log for this day yet. The log is written while the server runs under this program."
        self.shown_count = len(shown)
        self.count.configure(text=note)

    def highlight(self, needle, start):
        if not needle:
            return
        position = start
        while True:
            position = self.text.search(needle, position, stopindex="end", nocase=True)
            if not position:
                break
            end = "%s+%dc" % (position, len(needle))
            self.text.tag_add("hit", position, end)
            position = end

    def on_new_lines(self, new_lines):
        """Live lines for today's log."""
        if not self.is_today():
            return
        added = [(line.level, line.text) for line in new_lines]
        self.lines.extend(added)
        matching = filter_lines(added, self.minimum(), self.search.get().strip())
        if not matching:
            self.count.configure(text="Showing %d of %d lines" % (self.shown_count, len(self.lines)))
            return
        at_end = self.text.yview()[1] > 0.999
        self.text.configure(state="normal")
        start = self.text.index("end-1c")
        for level, text in matching:
            self.text.insert("end", text + "\n", (level,) if level in ("W", "E", "D") else ())
        self.highlight(self.search.get().strip(), start)
        self.text.configure(state="disabled")
        if at_end:
            self.text.see("end")
        self.shown_count += len(matching)
        self.count.configure(text="Showing %d of %d lines" % (self.shown_count, len(self.lines)))
