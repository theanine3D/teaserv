"""Statistics window: who is online now, and graphs of player activity over time."""

import csv
import datetime
import time
import tkinter as tk
from tkinter import filedialog, ttk

from . import server, stats, ui

TILES = [
    ("players", "Players online"),
    ("lobby", "In lobbies"),
    ("game", "In games"),
    ("games", "Open games"),
    ("battles", "Battles running"),
    ("caelum", "Caelum players"),
]

CHARTS = [
    ("Players online", "line", [("players", "Online", "levels"), ("lobby", "In lobbies", "levels"),
                                 ("game", "In games", "levels")]),
    ("Logins", "bars", [("logins", "Logins", "counts")]),
    ("Games and battles started", "bars", [("new_games", "Games", "counts"), ("new_battles", "Battles", "counts")]),
    ("Open games and running battles", "line", [("games", "Open games", "levels"),
                                                 ("battles", "Battles running", "levels")]),
]

TABLE_COLUMNS = [
    ("time", "Time", None), ("players", "Players", "levels"), ("lobby", "In lobbies", "levels"),
    ("game", "In games", "levels"), ("games", "Open games", "levels"), ("battles", "Battles running", "levels"),
    ("logins", "Logins", "counts"), ("new_games", "Games started", "counts"),
    ("new_battles", "Battles started", "counts"),
]


def time_label(t, span):
    moment = datetime.datetime.fromtimestamp(t)
    if span <= 86400:
        return moment.strftime("%H:%M")
    if span <= 7 * 86400:
        return moment.strftime("%a %H:%M")
    return moment.strftime("%d %b")


def full_time_label(t):
    return datetime.datetime.fromtimestamp(t).strftime("%Y-%m-%d %H:%M")


class Chart(tk.Canvas):
    LEFT, RIGHT, TOP, BOTTOM = 40, 14, 34, 24

    def __init__(self, parent, title, kind, series):
        super().__init__(parent, background=ui.SURFACE, highlightthickness=1, highlightbackground=ui.GRID,
                         width=420, height=210)
        self.title = title
        self.kind = kind
        # Colours follow the series' place in the fixed palette order
        self.series = [(key, label, source, ui.SERIES[i]) for i, (key, label, source) in enumerate(series)]
        self.buckets = None
        self.bind("<Configure>", lambda e: self.draw())
        self.bind("<Motion>", self.hover)
        self.bind("<Leave>", lambda e: self.delete("hover"))

    def set_data(self, buckets):
        self.buckets = buckets
        self.draw()

    def values(self, key, source):
        return (self.buckets.levels if source == "levels" else self.buckets.counts)[key]

    def plot_box(self):
        return self.LEFT, self.TOP, self.winfo_width() - self.RIGHT, self.winfo_height() - self.BOTTOM

    def draw(self):
        self.delete("all")
        width, height = self.winfo_width(), self.winfo_height()
        if width < 120 or height < 100:
            return
        x0, y0, x1, y1 = self.plot_box()
        self.create_text(10, 8, anchor="nw", text=self.title, fill=ui.INK, font=("TkDefaultFont", 10, "bold"))
        if len(self.series) > 1:
            x = width - self.RIGHT
            for key, label, source, color in reversed(self.series):
                item = self.create_text(x, 16, anchor="e", text=label, fill=ui.INK_SECONDARY)
                x = self.bbox(item)[0] - 6
                self.create_rectangle(x - 10, 11, x, 21, fill=color, outline="")
                x -= 24
        buckets = self.buckets
        if buckets is None:
            return
        top = 0
        for key, label, source, color in self.series:
            top = max([top] + [v for v in self.values(key, source) if v is not None])
        scale_top = stats.nice_ceiling(top)
        self.scale_top = scale_top

        # Grid and axes: quiet, behind the data
        for fraction in (0, 0.5, 1):
            y = y1 - (y1 - y0) * fraction
            self.create_line(x0, y, x1, y, fill=ui.GRID)
            self.create_text(x0 - 6, y, anchor="e", text="%d" % round(scale_top * fraction), fill=ui.INK_SECONDARY)
        span = buckets.until - buckets.since
        for step in range(5):
            t = buckets.since + span * step / 4
            x = x0 + (x1 - x0) * step / 4
            anchor = "w" if step == 0 else "e" if step == 4 else "center"
            self.create_text(x, y1 + 5, anchor="n" + (anchor[0] if anchor != "center" else ""),
                             text=time_label(t, span), fill=ui.INK_SECONDARY)

        if buckets.sample_count == 0:
            self.create_text((x0 + x1) / 2, (y0 + y1) / 2, text="Nothing was recorded in this period",
                             fill=ui.INK_SECONDARY)
            return

        size = buckets.size
        bucket_width = (x1 - x0) / size

        def y_of(value):
            return y1 - (y1 - y0) * (value / scale_top)

        if self.kind == "line":
            for key, label, source, color in self.series:
                values = self.values(key, source)
                run = []
                for index in range(size + 1):
                    value = values[index] if index < size else None
                    if value is not None:
                        run.append((x0 + (index + 0.5) * bucket_width, y_of(value)))
                        continue
                    # A gap: the server recorded nothing here
                    if len(run) == 1:
                        px, py = run[0]
                        self.create_oval(px - 2, py - 2, px + 2, py + 2, fill=color, outline="")
                    elif len(run) > 1:
                        self.create_line(*[c for point in run for c in point], fill=color, width=2,
                                         joinstyle="round", capstyle="round")
                    run = []
        else:
            count = len(self.series)
            gap = 2 if bucket_width >= 6 * count else 1 if bucket_width >= 3 * count else 0
            bar = max(1.0, (bucket_width - gap * (count + 1)) / count)
            for number, (key, label, source, color) in enumerate(self.series):
                values = self.values(key, source)
                for index in range(size):
                    value = values[index]
                    if not value:
                        continue
                    left = x0 + index * bucket_width + gap + number * (bar + gap)
                    self.create_rectangle(left, y_of(value), left + bar, y1, fill=color, outline="")
        self.create_line(x0, y1, x1, y1, fill="#b9b8b2")

    def hover(self, event):
        self.delete("hover")
        buckets = self.buckets
        if buckets is None or buckets.sample_count == 0:
            return
        x0, y0, x1, y1 = self.plot_box()
        if not (x0 <= event.x <= x1 and y0 - 6 <= event.y <= y1 + 6):
            return
        bucket_width = (x1 - x0) / buckets.size
        index = min(buckets.size - 1, max(0, int((event.x - x0) / bucket_width)))
        x = x0 + (index + 0.5) * bucket_width
        self.create_line(x, y0, x, y1, fill="#8d8c86", tags="hover")
        rows = []
        for key, label, source, color in self.series:
            value = self.values(key, source)[index]
            rows.append((color, "%s: %s" % (label, "not recorded" if value is None else "%d" % value)))
        start = buckets.time_of(index)
        heading = "%s - %s" % (full_time_label(start), datetime.datetime.fromtimestamp(
            start + buckets.step).strftime("%H:%M"))
        box_width = max([len(heading) * 6.4] + [len(text) * 6.4 + 18 for _, text in rows]) + 16
        box_height = 22 + 16 * len(rows)
        left = x + 10 if x + 10 + box_width < self.winfo_width() else x - 10 - box_width
        top = max(2, min(event.y - box_height / 2, self.winfo_height() - box_height - 2))
        self.create_rectangle(left, top, left + box_width, top + box_height, fill="#ffffff", outline="#b9b8b2",
                              tags="hover")
        self.create_text(left + 8, top + 5, anchor="nw", text=heading, fill=ui.INK_SECONDARY, tags="hover")
        for number, (color, text) in enumerate(rows):
            y = top + 22 + 16 * number
            self.create_rectangle(left + 8, y + 3, left + 18, y + 13, fill=color, outline="", tags="hover")
            self.create_text(left + 24, y, anchor="nw", text=text, fill=ui.INK, tags="hover")


class StatsWindow(tk.Toplevel):
    def __init__(self, app):
        super().__init__(app.root)
        self.app = app
        self.title("Teaserv - Statistics")
        ui.center(self, 960, 780)
        self.minsize(720, 560)
        self.buckets = None
        self.refresh_job = None

        tiles = ttk.Frame(self, padding=(8, 8, 8, 0))
        tiles.pack(fill="x")
        self.tile_values = {}
        for column, (key, name) in enumerate(TILES + [("uptime", "Running for")]):
            tile = ttk.Frame(tiles, style="Tile.TFrame", padding=(10, 6), relief="solid", borderwidth=1)
            tile.grid(row=0, column=column, sticky="nsew", padx=(0, 6))
            tiles.columnconfigure(column, weight=1, uniform="tile")
            value = ttk.Label(tile, text="-", style="TileValue.TLabel")
            value.pack(anchor="w")
            ttk.Label(tile, text=name, style="TileName.TLabel").pack(anchor="w")
            self.tile_values[key] = value
        self.live_note = ttk.Label(self, text="", style="Help.TLabel", padding=(8, 2))
        self.live_note.pack(fill="x")

        filters = ttk.Frame(self, padding=(8, 2, 8, 6))
        filters.pack(fill="x")
        ttk.Label(filters, text="Period:").pack(side="left")
        self.range = ttk.Combobox(filters, state="readonly", width=16, values=[name for name, _, _ in stats.RANGES])
        self.range.current(2)
        self.range.pack(side="left", padx=(4, 12))
        self.range.bind("<<ComboboxSelected>>", lambda e: self.reload())
        ttk.Button(filters, text="Refresh", command=self.reload).pack(side="left")
        ttk.Button(filters, text="Show as table", command=self.show_table).pack(side="left", padx=6)
        self.summary = ttk.Label(filters, text="", style="Help.TLabel")
        self.summary.pack(side="left", padx=8)

        online = ttk.LabelFrame(self, text="Online now", padding=6)
        online.pack(side="bottom", fill="x", padx=8, pady=(4, 8))
        columns = ("name", "version", "where", "account")
        self.online = ttk.Treeview(online, columns=columns, show="headings", height=5)
        for column, heading, width in zip(columns, ("Player", "Game version", "Where", "Account"),
                                          (220, 140, 300, 120)):
            self.online.heading(column, text=heading)
            self.online.column(column, width=width, anchor="w")
        bar = ttk.Scrollbar(online, orient="vertical", command=self.online.yview)
        self.online.configure(yscrollcommand=bar.set)
        bar.pack(side="right", fill="y")
        self.online.pack(fill="x")

        grid = ttk.Frame(self, padding=(8, 0))
        grid.pack(fill="both", expand=True)
        self.charts = []
        for number, (title, kind, series) in enumerate(CHARTS):
            chart = Chart(grid, title, kind, series)
            chart.grid(row=number // 2, column=number % 2, sticky="nsew", padx=(0, 6), pady=(0, 6))
            self.charts.append(chart)
        for index in range(2):
            grid.columnconfigure(index, weight=1, uniform="chart")
            grid.rowconfigure(index, weight=1, uniform="chart")

        app.server.stats_listeners.append(self.on_live)
        app.server.state_listeners.append(self.on_state)
        self.protocol("WM_DELETE_WINDOW", self.close)
        self.on_live(app.server.stats)
        self.reload()

    def close(self):
        if self.refresh_job:
            self.after_cancel(self.refresh_job)
        self.app.server.stats_listeners.remove(self.on_live)
        self.app.server.state_listeners.remove(self.on_state)
        self.app.window_closed("stats")
        self.destroy()

    # ----- live numbers

    def on_state(self, state, detail):
        self.on_live(self.app.server.stats)

    def on_live(self, live):
        srv = self.app.server
        for key, _ in TILES:
            self.tile_values[key].configure(text="-" if live is None else "%d" % live.get(key, 0))
        self.tile_values["uptime"].configure(
            text=server.format_duration(srv.uptime_seconds()) if srv.is_active() else "-")
        for row in self.online.get_children():
            self.online.delete(row)
        if live is None:
            if srv.state == server.RUNNING and srv.stats_supported is False:
                note = "This server build does not report live numbers."
            elif srv.state == server.RUNNING:
                note = "Waiting for the server's numbers..."
            else:
                note = "The server is not running. The graphs show what was recorded earlier."
            self.live_note.configure(text=note)
            return
        self.live_note.configure(text="Live numbers, updated every %d seconds. Since the server started: %d logins, "
                                 "%d games, %d battles." % (
                                     max(1, self.app.settings["stats_poll_seconds"]), live.get("total_logins", 0),
                                     live.get("total_games", 0), live.get("total_battles", 0)))
        places = {}
        for lobby in live.get("lobbies", []):
            places[lobby["id"]] = lobby.get("name") or "Lobby %s" % lobby.get("block", "?")
        for game in live.get("game_list", []):
            places[game["id"]] = "Game: %s%s" % (game.get("name") or "(no name)",
                                                  " (battle)" if game.get("battle") else "")
        for client in live.get("clients", []):
            if client.get("account") is None:
                where = "Logging in"
            elif client.get("lobby") is None:
                where = "Menu"
            else:
                where = places.get(client["lobby"], "Lobby" if not client.get("in_game") else "Game")
            version = client.get("version", "")
            if client.get("caelum"):
                version += " (Caelum)"
            account = client.get("account")
            self.online.insert("", "end", values=(client.get("name") or "(no name yet)", version, where,
                                                  "" if account is None else "%08X" % account))

    # ----- recorded data

    def reload(self):
        if self.refresh_job:
            self.after_cancel(self.refresh_job)
        name, seconds, step = stats.RANGES[self.range.current()]
        self.buckets = stats.load_buckets(self.app.settings.stats_dir, seconds, step, time.time())
        for chart in self.charts:
            chart.set_data(self.buckets)
        b = self.buckets
        if b.sample_count:
            self.summary.configure(text="%s: %d logins, %d games and %d battles started; most players at once: %d"
                                   % (name, b.total("logins"), b.total("new_games"), b.total("new_battles"),
                                      b.peak("players")))
        else:
            self.summary.configure(text="%s: nothing recorded. The server records while it runs." % name)
        self.refresh_job = self.after(30000, self.reload)

    def show_table(self):
        TableWindow(self, self.buckets, stats.RANGES[self.range.current()][0])


class TableWindow(tk.Toplevel):
    def __init__(self, parent, buckets, range_name):
        super().__init__(parent)
        self.title("Teaserv - Statistics table (%s)" % range_name)
        ui.center(self, 1010, 480)
        self.buckets = buckets
        top = ttk.Frame(self, padding=8)
        top.pack(fill="x")
        ttk.Label(top, text="One row per time step. Player and game numbers are the highest of the step. Empty cells: "
                  "the server recorded nothing then.",
                  style="Help.TLabel").pack(side="left")
        ttk.Button(top, text="Save as CSV...", command=self.save).pack(side="right")
        frame = ttk.Frame(self, padding=(8, 0, 8, 8))
        frame.pack(fill="both", expand=True)
        columns = [key for key, _, _ in TABLE_COLUMNS]
        tree = ttk.Treeview(frame, columns=columns, show="headings")
        for key, heading, _ in TABLE_COLUMNS:
            tree.heading(key, text=heading)
            tree.column(key, width=140 if key == "time" else 102, anchor="w" if key == "time" else "e")
        bar = ttk.Scrollbar(frame, orient="vertical", command=tree.yview)
        tree.configure(yscrollcommand=bar.set)
        bar.pack(side="right", fill="y")
        tree.pack(fill="both", expand=True)
        self.rows = self.make_rows()
        for row in reversed(self.rows):
            tree.insert("", "end", values=row)

    def make_rows(self):
        b = self.buckets
        rows = []
        for index in range(b.size):
            row = [full_time_label(b.time_of(index))]
            recorded = b.levels["players"][index] is not None
            for key, _, source in TABLE_COLUMNS[1:]:
                if source == "levels":
                    value = b.levels[key][index]
                    row.append("" if value is None else value)
                else:
                    row.append(b.counts[key][index] if recorded else "")
            rows.append(row)
        return rows

    def save(self):
        path = filedialog.asksaveasfilename(parent=self, defaultextension=".csv",
                                            filetypes=[("CSV table", "*.csv")], initialfile="teaserv-statistics.csv")
        if not path:
            return
        write_csv(path, self.rows)


def write_csv(path, rows):
    with open(path, "w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow([heading for _, heading, _ in TABLE_COLUMNS])
        writer.writerows(rows)
