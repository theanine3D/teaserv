"""Unit tests for the parts of Teaserv Admin that need no window and no server.

    python -m unittest discover -s tools/admin-gui/tests -p "test_*.py"
"""

import json
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from teaserv_admin import caelum_files, jsonc, lobby_media, server, stats  # noqa: E402
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

    def test_lobby_media_has_its_own_page(self):
        self.assertIn("Episode3LobbyBanners", schema.form_top_level_keys())  # not under "Other settings"

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


def prs_literals(data):
    """A valid PRS stream holding `data` as literals only, ending with the end command."""
    out = bytearray()
    bits = [1] * len(data) + [0, 1]
    payload = list(data) + [0, 0]
    control_at, count, used = None, 0, 0
    for bit in bits:
        if count == 0:
            control_at = len(out)
            out.append(0)
            count = 8
        out[control_at] |= bit << (8 - count)
        count -= 1
        if bit and used < len(data):
            out.append(payload[used])
            used += 1
    out += b"\x00\x00"
    return bytes(out)


def make_bmp(width, height, depth=24, alpha=0xFF, seed=1):
    import random
    rnd = random.Random(seed)
    stride = width * (depth // 8)
    padding = (4 - stride % 4) % 4
    pixels = bytearray()
    for _ in range(abs(height)):
        for _ in range(width):
            pixels += bytes([rnd.randrange(256), rnd.randrange(256), rnd.randrange(256)])
            if depth == 32:
                pixels.append(alpha if alpha is not None else rnd.randrange(256))
        pixels += bytes(padding)
    return (b"BM" + struct.pack("<IHHI", 54 + len(pixels), 0, 0, 54) +
            struct.pack("<IiiHHIIiiII", 40, width, height, 1, depth, 0, len(pixels), 0, 0, 0, 0) + bytes(pixels))


SERVER_EXE = os.environ.get("TEASERV_EXE") or os.path.join(
    REPO, "build", "server-on", "teaserv.exe" if sys.platform == "win32" else "teaserv")
SERVER_EXTRA_PATH = os.environ.get("TEASERV_EXTRA_PATH", "")


class LobbyMediaTest(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.system = os.path.join(self.folder.name, "system")
        os.makedirs(os.path.join(self.system, "ep3", "banners"))

    def tearDown(self):
        self.folder.cleanup()

    def put(self, name, data):
        path = lobby_media.server_path(self.system, name)
        with open(path, "wb") as f:
            f.write(data)
        return path

    def check(self, entries, compressor=None):
        return lobby_media.check_entries(entries, lambda n: lobby_media.server_path(self.system, n), compressor)

    def test_list_round_trip(self):
        entries = [lobby_media.Entry(1, 0x30, "a.bmp"), lobby_media.Entry(2, 0x01000000, "m.bml.prs", "s.bml.prs")]
        text = jsonc.set_value(SAMPLE, (lobby_media.KEY,), lobby_media.format_entries(entries))
        value = jsonc.parse(text).to_python()
        self.assertEqual(lobby_media.parse_entries(value[lobby_media.KEY]), entries)
        self.assertIn('[1, 0x00000030, "a.bmp"],', text)
        self.assertEqual(value["ServerName"], "newserv")  # the rest is untouched
        self.assertEqual(lobby_media.format_entries([]), "[]")
        self.assertEqual(lobby_media.parse_entries(None), [])
        for bad in ({}, [[1, 0x30]], [[4, 1, "a.bmp"]], [[1, 0, "a.bmp"]], [[1, 0x100000000, "a.bmp"]],
                    [[True, 1, "a.bmp"]], [[1, 1, 5]], [[1, 1, "a", "b", "c"]], [[1.0, 1, "a.bmp"]]):
            with self.assertRaises(ValueError, msg=repr(bad)):
                lobby_media.parse_entries(bad)

    def test_names_places_and_kinds(self):
        self.assertEqual(lobby_media.media_kind(1, "A.BMP"), "bmp")
        self.assertEqual(lobby_media.media_kind(1, "a.gvm"), "raw")
        self.assertEqual(lobby_media.media_kind(1, "a.gvm.prs"), "prs")
        self.assertIsNone(lobby_media.media_kind(1, "a.bml"))
        self.assertIsNone(lobby_media.media_kind(2, "a.bmp"))
        self.assertEqual(lobby_media.media_kind(3, "a.bml.prs"), "prs")
        self.assertIsNone(lobby_media.media_kind(1, "a.png"))
        self.assertEqual(lobby_media.parse_locations("0x30"), 0x30)
        self.assertEqual(lobby_media.parse_locations("80000000"), 0x80000000)
        for bad in ("", "0", "xyz", "123456789"):
            with self.assertRaises(ValueError):
                lobby_media.parse_locations(bad)
        self.assertTrue(lobby_media.check_filename("C:\\x.bmp"))
        self.assertTrue(lobby_media.check_filename("/x.bmp"))
        self.assertEqual(lobby_media.check_filename("sub/x.bmp"), "")
        self.assertEqual(lobby_media.relative_name(self.system, os.path.join(self.system, "ep3", "banners", "s",
                                                                             "x.bmp")), "s/x.bmp")
        self.assertIsNone(lobby_media.relative_name(self.system, os.path.join(self.folder.name, "x.bmp")))
        self.assertEqual(len(lobby_media.PLACES), 32)

    def test_prs_sizes(self):
        data = bytes(range(256)) * 3
        stream = prs_literals(data)
        self.assertEqual(lobby_media.prs_decompressed_size(stream), len(data))
        with self.assertRaises(lobby_media.PrsError):
            lobby_media.prs_decompressed_size(stream[:-1])  # cut short
        with self.assertRaises(lobby_media.PrsError):
            lobby_media.prs_decompressed_size(stream, 100)  # over the limit
        # Short copy at the very start refers to before the data
        with self.assertRaises(lobby_media.PrsError):
            lobby_media.prs_decompressed_size(bytes([0b00000000, 0xFF]))
        # One literal, then a long copy of 9 bytes from offset -1, then the end
        self.assertEqual(lobby_media.prs_decompressed_size(bytes([0b00010101, 0x41, 0xFF, 0xFF, 0, 0])), 1 + 9)
        self.assertLessEqual(len(stream), lobby_media.prs_size_bound(len(data)))

    def test_bmp_is_encoded_like_the_server(self):
        path = self.put("pic.bmp", make_bmp(16, 16))
        m = lobby_media.measure_file(path, "pic.bmp", 1, lobby_media.PC, 0, lambda data: 123)
        self.assertTrue(m.ok, m.error)
        self.assertEqual((m.compressed, m.decompressed), (123, 0x60 + 16 * 16 * 2))
        self.assertTrue(m.gc_ok)
        w, h, rows = lobby_media.decode_bmp(make_bmp(8, -8, 32, None))  # top-down, random alpha
        gvm, transparent = lobby_media.encode_gvm(w, h, rows, "bnr3", 0x83)
        self.assertTrue(transparent)
        self.assertEqual(gvm[:4], b"GVMH")
        self.assertEqual(gvm[0x0E:0x12], b"bnr3")
        self.assertEqual(struct.unpack(">I", gvm[0x2E:0x32])[0], 0x83)
        self.assertEqual(gvm[0x2B], lobby_media.GVR_RGB5A3)
        for data, reason in ((make_bmp(16, 8), "square"), (make_bmp(4, 4), "too small"), (make_bmp(12, 12), "power"),
                             (make_bmp(8, 8)[:-5], "cut short"), (b"PNG", "not a BMP")):
            m = lobby_media.measure_file(self.put("bad.bmp", data), "bad.bmp", 1, lobby_media.PC, 0, lambda d: 1)
            self.assertIn(reason, m.error)
        sixteen = bytearray(make_bmp(8, 8))
        sixteen[28] = 16
        m = lobby_media.measure_file(self.put("bad.bmp", bytes(sixteen)), "bad.bmp", 1, lobby_media.PC, 0, None)
        self.assertIn("24-bit and 32-bit", m.error)
        m = lobby_media.measure_file(self.put("clear.bmp", make_bmp(8, 8, 32, 0)), "clear.bmp", 1, lobby_media.PC, 0,
                                     lambda d: 10)
        self.assertTrue(m.ok and any("invisible" in w for w in m.warnings), m.warnings)

    def test_limits_and_groups(self):
        big = prs_literals(bytes(range(256)) * 60)  # about 17 KB: over 0x3800, under 0x7BF0
        self.put("big.gvm.prs", big)
        self.put("small.gvm.prs", prs_literals(b"x" * 100))
        self.put("model.bml.prs", prs_literals(b"m" * 100))
        report = self.check([lobby_media.Entry(1, 0x30, "big.gvm.prs")])
        self.assertTrue(report.ok, report.errors)
        self.assertIn("Only Caelum players", report.entries[0].audience)
        report = self.check([lobby_media.Entry(1, 0x30, "big.gvm.prs", "small.gvm.prs")])
        self.assertTrue(report.ok, report.errors)
        self.assertIn("original clients the original-client file", report.entries[0].audience)
        # A fallback must fit original clients
        report = self.check([lobby_media.Entry(1, 0x30, "small.gvm.prs", "big.gvm.prs")])
        self.assertIn("the limit for original clients", report.errors[0][1])
        # Every member of a group with a fallback needs one
        report = self.check([lobby_media.Entry(1, 0x01000000, "big.gvm.prs", "small.gvm.prs"),
                             lobby_media.Entry(2, 0x01000000, "model.bml.prs")])
        self.assertTrue(report.errors and "every entry there needs one" in report.errors[0][1], report.errors)
        report = self.check([lobby_media.Entry(1, 0x01000000, "big.gvm.prs", "small.gvm.prs"),
                             lobby_media.Entry(2, 0x01000000, "model.bml.prs", "model.bml.prs")])
        self.assertTrue(report.ok, report.errors)
        # Not refused by the server, but noted
        report = self.check([lobby_media.Entry(2, 0x40, "model.bml.prs")])
        self.assertTrue(report.ok and any("needs a texture" in t for _, t in report.warnings), report.warnings)
        report = self.check([lobby_media.Entry(1, 0x00800000, "small.gvm.prs")])
        self.assertTrue(any("meant for models" in t for _, t in report.warnings), report.warnings)
        report = self.check([lobby_media.Entry(1, 1 << n, "small.gvm.prs") for n in range(9)])
        self.assertTrue(any("at most 8" in t for i, t in report.warnings if i is None))
        # Accepted at load, but refused by the extension scene baseline check
        report = self.check([lobby_media.Entry(1, 0x30, "small.gvm.prs"), lobby_media.Entry(1, 0x10, "small.gvm.prs")])
        self.assertTrue(report.ok and any("share a place (0x00000010)" in t for _, t in report.warnings),
                        report.warnings)
        report = self.check([lobby_media.Entry(1, 0x30, "small.gvm.prs"), lobby_media.Entry(1, 0x30, "big.gvm.prs")])
        self.assertTrue(report.ok and any("more than one entry of the same type" in t for _, t in report.warnings),
                        report.warnings)
        # Wrong kinds of file, missing files, oversized files
        report = self.check([lobby_media.Entry(2, 0x30, "pic.bmp"), lobby_media.Entry(1, 0x30, "missing.gvm.prs")])
        self.assertIn("cannot be used for a model", report.errors[0][1])
        self.assertIn("cannot be read", report.errors[1][1])
        self.put("huge.gvm.prs", prs_literals(b"y" * 0x7C00))
        self.assertIn("the limit for Caelum", self.check([lobby_media.Entry(1, 1, "huge.gvm.prs")]).errors[0][1])
        self.put("empty.gvm", b"")
        self.assertIn("empty", self.check([lobby_media.Entry(1, 1, "empty.gvm")]).errors[0][1])

    def test_raw_files_use_the_compressor(self):
        self.put("raw.gvm", b"\x00" * 0x40000)  # decompresses beyond original clients, compresses well
        report = self.check([lobby_media.Entry(1, 0x30, "raw.gvm")], compressor=lambda data: 0x500)
        self.assertTrue(report.ok, report.errors)
        self.assertEqual(report.entries[0].primary.compressed, 0x500)
        self.assertIn("Only Caelum players", report.entries[0].audience)
        report = self.check([lobby_media.Entry(1, 0x30, "raw.gvm")], compressor=lambda data: 0x7C00)
        self.assertIn("compresses to", report.errors[0][1])

        def unavailable(data):
            raise lobby_media.CompressorUnavailable("no server program")
        report = self.check([lobby_media.Entry(1, 0x30, "raw.gvm")], compressor=unavailable)
        self.assertTrue(report.ok and "no server program" in report.warnings[0][1], report.warnings)
        # Bitmap entries share one generated texture number per entry, counted in list order
        self.put("a.bmp", make_bmp(8, 8))
        seen = []
        self.check([lobby_media.Entry(1, 1, "a.bmp"), lobby_media.Entry(1, 2, "raw.gvm"),
                    lobby_media.Entry(1, 4, "a.bmp")], compressor=lambda data: seen.append(data[0x0E:0x12]) or 10)
        self.assertEqual([s for s in seen if s.startswith(b"bnr")], [b"bnr0", b"bnr1"])

    @unittest.skipUnless(os.path.isfile(SERVER_EXE), "no server program (set TEASERV_EXE)")
    def test_against_the_server_program(self):
        compressor = lobby_media.ServerCompressor(SERVER_EXE, SERVER_EXTRA_PATH, timeout=120)
        try:
            compressor(b"probe")
        except lobby_media.CompressorUnavailable as e:
            self.skipTest("the server program cannot run here (%s); set TEASERV_EXTRA_PATH" % e)
        # The GVM written here is the server's own encode-gvm output, byte for byte
        import subprocess
        env = dict(os.environ)
        if SERVER_EXTRA_PATH:
            env["PATH"] = SERVER_EXTRA_PATH + os.pathsep + env["PATH"]
        for name, data in (("opaque.bmp", make_bmp(32, 32)), ("alpha.bmp", make_bmp(16, 16, 32, None))):
            path = self.put(name, data)
            target = path + ".gvm"
            subprocess.run([SERVER_EXE, "encode-gvm", path, target], env=env, check=True, timeout=60,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            with open(target, "rb") as f:
                expected = f.read()
            w, h, rows = lobby_media.decode_bmp(data)
            self.assertEqual(lobby_media.encode_gvm(w, h, rows, "image.gvr", 0)[0], expected, name)
            # The compressed size is what the server's optimal compressor writes
            prs = path + ".prs"
            subprocess.run([SERVER_EXE, "compress-prs", "--optimal", target, prs], env=env, check=True, timeout=60,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            self.assertEqual(compressor(expected), os.path.getsize(prs))
            with open(prs, "rb") as f:
                self.assertEqual(lobby_media.prs_decompressed_size(f.read()), len(expected))


if __name__ == "__main__":
    unittest.main()
