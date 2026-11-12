"""Test helper: write made-up statistics for the last days, in the format the server records.

    python make_sample_stats.py <stats folder> [days]

The numbers are invented (a daily rhythm with an evening peak) so that the Statistics window has something to draw on
a test server nobody plays on. Every line carries "demo": 1. Never run it against a real server's system/stats.
"""

import datetime
import json
import math
import os
import random
import sys
import time


def make(stats_dir, days=8, now=None, end_gap=600):
    rng = random.Random(7)
    now = int(now or time.time())
    start = now - days * 86400
    os.makedirs(stats_dir, exist_ok=True)
    files = {}
    players = 0
    games = 0
    battles = 0
    t = start - start % 60
    while t < now - end_gap:
        local = time.localtime(t)
        hour = local.tm_hour + local.tm_min / 60.0
        # Target: few players at night, most around 21:00; weekends busier
        target = 1.5 + 11 * math.exp(-((hour - 21) % 24 if hour > 9 else hour + 3) ** 2 / 18.0)
        if local.tm_wday >= 5:
            target *= 1.4
        logins = 0
        if players < target and rng.random() < 0.25:
            step = rng.randint(1, 2)
            players += step
            logins = step
        elif players > target and rng.random() < 0.2:
            players = max(0, players - rng.randint(1, 2))
        new_games = 0
        if games < players // 3 and rng.random() < 0.3:
            games += 1
            new_games = 1
        elif games > players // 3 and rng.random() < 0.3:
            games -= 1
        in_game = min(players, games * 2 + (1 if games and players % 2 else 0))
        new_battles = 0
        if battles < games and rng.random() < 0.15:
            battles += 1
            new_battles = 1
        elif battles and rng.random() < 0.08:
            battles -= 1
        battles = min(battles, games)
        sample = {
            "t": t, "ev": "sample", "up": t - start, "conn": players + (1 if rng.random() < 0.1 else 0),
            "players": players, "lobby": players - in_game, "game": in_game, "ep3": players,
            "caelum": players * 2 // 3, "games": games, "battles": battles, "logins": logins,
            "new_games": new_games, "new_battles": new_battles, "demo": 1,
        }
        month = datetime.datetime.utcfromtimestamp(t).strftime("%Y-%m")
        files.setdefault(month, []).append(json.dumps(sample))
        t += 60
    for month, lines in files.items():
        path = os.path.join(stats_dir, month + ".jsonl")
        existing = ""
        if os.path.exists(path):
            with open(path, encoding="utf-8") as f:
                existing = "".join(line for line in f if '"demo"' not in line)
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(lines) + "\n" + existing)
    return sum(len(lines) for lines in files.values())


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    count = make(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 8)
    print("wrote %d demo samples to %s" % (count, sys.argv[1]))
