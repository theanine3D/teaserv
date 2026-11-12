"""Reads the statistics the server records in system/stats/YYYY-MM.jsonl and groups them for the graphs.

Each line is one sample: the counts at that moment (players, lobby, game, games, battles, ...) and what happened since
the previous line (logins, new_games, new_battles). See src/TeaservStats.cc.
"""

import datetime
import json
import os
import re

RANGES = [
    ("Last hour", 3600, 60),
    ("Last 6 hours", 6 * 3600, 300),
    ("Last 24 hours", 24 * 3600, 900),
    ("Last 7 days", 7 * 86400, 3600),
    ("Last 30 days", 30 * 86400, 6 * 3600),
]

LEVEL_KEYS = ("players", "lobby", "game", "games", "battles", "caelum", "ep3", "conn")
COUNT_KEYS = ("logins", "new_games", "new_battles")


def month_files(stats_dir, since, until):
    """Paths of the month files that can hold samples between two times (seconds, UTC months)."""
    if not os.path.isdir(stats_dir):
        return []
    first = datetime.datetime.utcfromtimestamp(since).strftime("%Y-%m")
    last = datetime.datetime.utcfromtimestamp(until).strftime("%Y-%m")
    found = []
    for name in sorted(os.listdir(stats_dir)):
        match = re.match(r"^(\d{4}-\d\d)\.jsonl$", name)
        if match and first <= match.group(1) <= last:
            found.append(os.path.join(stats_dir, name))
    return found


def read_samples(stats_dir, since, until):
    samples = []
    for path in month_files(stats_dir, since, until):
        try:
            with open(path, encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        sample = json.loads(line)
                    except ValueError:
                        continue  # a line cut short by a crash
                    t = sample.get("t")
                    if isinstance(t, (int, float)) and since <= t <= until:
                        samples.append(sample)
        except OSError:
            continue
    samples.sort(key=lambda s: s["t"])
    return samples


class Buckets:
    """Samples grouped into equal time steps.

    levels[key][i]: the highest value seen in step i, or None when the server recorded nothing then (it was off).
    counts[key][i]: the sum of the events in step i (0 when nothing was recorded).
    """

    def __init__(self, since, until, step):
        self.since = since
        self.until = until
        self.step = step
        self.size = max(1, int((until - since + step - 1) // step))
        self.levels = {key: [None] * self.size for key in LEVEL_KEYS}
        self.counts = {key: [0] * self.size for key in COUNT_KEYS}
        self.restarts = []  # times of "start" lines
        self.sample_count = 0

    def add(self, sample):
        index = int((sample["t"] - self.since) // self.step)
        if not 0 <= index < self.size:
            return
        self.sample_count += 1
        if sample.get("ev") == "start":
            self.restarts.append(sample["t"])
        for key in LEVEL_KEYS:
            value = sample.get(key)
            if isinstance(value, (int, float)):
                current = self.levels[key][index]
                self.levels[key][index] = value if current is None else max(current, value)
        for key in COUNT_KEYS:
            value = sample.get(key)
            if isinstance(value, (int, float)):
                self.counts[key][index] += value

    def time_of(self, index):
        return self.since + index * self.step

    def total(self, key):
        return sum(self.counts[key])

    def peak(self, key):
        values = [v for v in self.levels[key] if v is not None]
        return max(values) if values else 0


def load_buckets(stats_dir, seconds, step, now):
    until = (int(now) // step + 1) * step
    since = until - seconds
    buckets = Buckets(since, until, step)
    for sample in read_samples(stats_dir, since, until):
        buckets.add(sample)
    return buckets


def nice_ceiling(value):
    """A round number at or above value, for the top of a graph's scale."""
    if value <= 4:
        return 4
    magnitude = 1
    while magnitude * 10 < value:
        magnitude *= 10
    for factor in (1, 2, 4, 5, 8, 10):
        if factor * magnitude >= value:
            return factor * magnitude
    return 10 * magnitude
