"""Unit tests for the parts of Teaserv Admin that need no window and no server.

    python -m unittest discover -s tools/admin-gui/tests -p "test_*.py"
"""

import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from teaserv_admin import caelum_files, jsonc, server, stats  # noqa: E402
from teaserv_admin import config_schema as schema  # noqa: E402
from teaserv_admin.logs_window import filter_lines, read_log_file  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
EXAMPLE = os.path.join(REPO, "system", "config.example.json")

SAMPLE = """{
  // A comment
  "ServerName": "newserv", // trailing comment
  "Flags": 0x0042,
  "LogLevels": {
    // inner comment
    "Clients": "INFO",
  },
  "Events": [
    "none", // Lobby 1
    "xmas", // Lobby 2
  ],
  "Ping": 30000000,
}
"""


class JsoncTest(unittest.TestCase):
    def test_parse_extensions(self):
        root = jsonc.parse(SAMPLE)
        value = root.to_python()
        self.assertEqual(value["Flags"], 0x42)
        self.assertTrue(jsonc.find(root, ("Flags",)).hex)
        self.assertEqual(value["LogLevels"], {"Clients": "INFO"})
        self.assertEqual(value["Events"], ["none", "xmas"])

    def test_replace_keeps_everything_else(self):
        text = jsonc.set_value(SAMPLE, ("ServerName",), jsonc.serialize("Tea"))
        self.assertEqual(text, SAMPLE.replace('"newserv"', '"Tea"'))
        text = jsonc.set_value(SAMPLE, ("Events", 1), '"val"')
        self.assertEqual(text, SAMPLE.replace('"xmas"', '"val"'))
        text = jsonc.set_value(SAMPLE, ("Flags",), jsonc.serialize(0x4A, hex_int=True))
        self.assertEqual(text, SAMPLE.replace("0x0042", "0x004A"))

    def test_add_and_remove(self):
        text = jsonc.set_value(SAMPLE, ("TeaservStatsInterval",), "30")
        self.assertEqual(jsonc.parse(text).to_python()["TeaservStatsInterval"], 30)
        self.assertEqual(jsonc.remove_value(text, ("TeaservStatsInterval",)), SAMPLE)
        nested = jsonc.set_value(SAMPLE, ("LogLevels", "Lobbies"), '"DEBUG"')
        self.assertEqual(jsonc.parse(nested).to_python()["LogLevels"], {"Clients": "INFO", "Lobbies": "DEBUG"})
        self.assertEqual(jsonc.remove_value(nested, ("LogLevels", "Lobbies")), SAMPLE)
        self.assertEqual(jsonc.parse(jsonc.set_value('{"a": 1}', ("b",), "2")).to_python(), {"a": 1, "b": 2})
        self.assertEqual(jsonc.parse(jsonc.set_value("{}", ("b",), "2")).to_python(), {"b": 2})
        self.assertEqual(jsonc.parse(jsonc.remove_value('{"a": 1, "b": 2}', ("b",))).to_python(), {"a": 1})

    def test_crlf_is_kept(self):
        crlf = SAMPLE.replace("\n", "\r\n")
        text = jsonc.set_value(crlf, ("NewKey",), "1")
        self.assertEqual(text.count("\r\n"), text.count("\n"))
        self.assertEqual(jsonc.remove_value(text, ("NewKey",)), crlf)

    def test_errors_name_the_place(self):
        for bad in ('{"a": 1 "b": 2}', '{"a": /* x */ 1}', '{"a": "x}', '{"a": 1} x', '{"a": 1e}', '{a: 1}'):
            with self.assertRaises(jsonc.JsoncError) as raised:
                jsonc.parse(bad)
            self.assertIn("line 1", str(raised.exception))

    def test_serialize_round_trip(self):
        for value in ["plain", 'quote " and \\ slash', "line\nbreak\ttab", "\x01", 5, -3, 1.5, True, None,
                      [1, "a"], {"k": [2]}]:
            self.assertEqual(jsonc.parse(jsonc.serialize(value)).to_python(), value)

    @unittest.skipUnless(os.path.exists(EXAMPLE), "config.example.json is not here")
    def test_example_config(self):
        with open(EXAMPLE, encoding="utf-8", newline="") as f:
            text = f.read()
        root = jsonc.parse(text)
        value = root.to_python()
        # Every field of the form fits the value the example file has for it
        for field in schema.all_fields():
            node = jsonc.find(root, field.path)
            if node is None:
                self.assertTrue(field.optional, field.name)
                continue
            self.assertTrue(field.accepts(node.to_python()), field.name)
        changed = jsonc.set_value(text, ("ServerName",), '"x"')
        self.assertEqual(len(changed), len(text) - len(value["ServerName"]) + 1)


class SchemaTest(unittest.TestCase):
    def field(self, name):
        return [f for f in schema.all_fields() if f.name == name][0]

    def test_checks(self):
        with self.assertRaises(ValueError):
            self.field("ServerName").check("x" * 17)
        self.assertEqual(self.field("ClientPingInterval").check(30), 30000000)
        with self.assertRaises(ValueError):
            self.field("DNSServerPort").check(70000)
        with self.assertRaises(ValueError):
            self.field("LocalAddress").check("300.1.1.1")
        self.assertEqual(self.field("LocalAddress").check("en0"), "en0")
        self.assertEqual(self.field("HTTPListen").check(["8080", "127.0.0.1:81"]), [8080, "127.0.0.1:81"])
        with self.assertRaises(ValueError):
            self.field("BannedIPV4Ranges").check(["1.2.3"])
        with self.assertRaises(ValueError):
            self.field("Episode3DefeatPlayerMeseta").check([1, 2, 3])
        with self.assertRaises(ValueError):
            self.field("CardAuctionSize").check([8, 6])


class CaelumFilesTest(unittest.TestCase):
    def test_lobbies(self):
        header, names = caelum_files.parse_lobbies("# extra lobbies\n20 LOBBY-CARD06\n21\n")
        self.assertEqual((header, names), (["# extra lobbies"], ["LOBBY-CARD06", ""]))
        self.assertEqual(caelum_files.format_lobbies(header, names), "# extra lobbies\n20 LOBBY-CARD06\n21\n")
        with self.assertRaises(ValueError):
            caelum_files.parse_lobbies("21 X\n")
        with self.assertRaises(ValueError):
            caelum_files.format_lobbies([], ["x" * 33])

    def test_modpacks(self):
        sha = "ab" * 32
        text = "CMOD1\n%s\tPack\thttps://example.com/p.zip\n" % sha
        packs = caelum_files.parse_modpacks(text)
        self.assertEqual(packs, [(sha, "Pack", "https://example.com/p.zip")])
        self.assertEqual(caelum_files.format_modpacks(packs), text)
        for bad in ([("zz", "Pack", "https://x/p.zip")], [(sha, "", "https://x/p.zip")],
                    [(sha, "Pack", "ftp://x/p.zip")], [(sha, "A", "https://x/a"), (sha, "B", "https://x/b")]):
            with self.assertRaises(ValueError):
                caelum_files.check_modpacks(bad)
        with self.assertRaises(ValueError):
            caelum_files.parse_modpacks("%s\tPack\thttps://example.com/p.zip\n" % sha)


class StatsTest(unittest.TestCase):
    def test_buckets(self):
        with tempfile.TemporaryDirectory() as folder:
            now = 1794514000
            lines = [
                {"t": now - 3000, "ev": "start", "players": 0, "logins": 0, "new_games": 0, "new_battles": 0},
                {"t": now - 2940, "ev": "sample", "players": 2, "lobby": 2, "logins": 2, "new_games": 0},
                {"t": now - 2880, "ev": "sample", "players": 3, "lobby": 1, "game": 2, "logins": 1, "new_games": 1,
                 "new_battles": 1},
            ]
            import datetime
            month = datetime.datetime.utcfromtimestamp(now).strftime("%Y-%m")
            with open(os.path.join(folder, month + ".jsonl"), "w") as f:
                f.write("\n".join(json.dumps(line) for line in lines) + "\n{\"t\": 17945")  # last line cut short
            buckets = stats.load_buckets(folder, 3600, 60, now)
            self.assertEqual(buckets.sample_count, 3)
            self.assertEqual(buckets.total("logins"), 3)
            self.assertEqual(buckets.total("new_battles"), 1)
            self.assertEqual(buckets.peak("players"), 3)
            self.assertEqual(len(buckets.restarts), 1)
            self.assertIn(None, buckets.levels["players"])  # nothing recorded: a gap, not zero
        self.assertEqual(stats.load_buckets(os.path.join(folder, "missing"), 3600, 60, now).sample_count, 0)

    def test_nice_ceiling(self):
        self.assertEqual([stats.nice_ceiling(v) for v in (0, 3, 5, 13, 47, 120)], [4, 4, 5, 20, 50, 200])


class LogTest(unittest.TestCase):
    def test_levels_and_filter(self):
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "teaserv-2026-11-12.log")
            with open(path, "w", encoding="utf-8") as f:
                f.write("I 1 2026-11-12 10:00:00 - Ready\n"
                        "W 1 2026-11-12 10:00:01 - Something odd\n"
                        "  continued detail\n"
                        "I 1 2026-11-12 10:00:02 - [Admin] > nope\n"
                        "FAILED: no such command; try 'help'\n"
                        "E 1 2026-11-12 10:00:03 - Broken\n")
            lines = read_log_file(path)
        self.assertEqual([level for level, _ in lines], ["I", "W", "W", "I", "E", "E"])
        self.assertEqual(len(filter_lines(lines, 2, "")), 4)
        self.assertEqual(len(filter_lines(lines, 3, "")), 2)
        self.assertEqual([text for _, text in filter_lines(lines, 0, "READY")], ["I 1 2026-11-12 10:00:00 - Ready"])
        self.assertEqual(server.split_log_line(lines[0][1]), ("I", "2026-11-12 10:00:00", "Ready"))

    def test_exit_code_and_duration(self):
        self.assertEqual(server.format_duration(59), "59 s")
        self.assertEqual(server.format_duration(3700), "1 h 1 min")
        self.assertEqual(server.format_duration(90000), "1 d 1 h")


if __name__ == "__main__":
    unittest.main()
