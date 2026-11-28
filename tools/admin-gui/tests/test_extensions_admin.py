"""Unit tests for the Extensions page logic (teaserv_admin/extensions_admin.py); no window and no server.

    python -m unittest discover -s tools/admin-gui/tests -p "test_*.py"
"""

import json
import os
import shutil
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from teaserv_admin import extensions_admin as ext  # noqa: E402
from teaserv_admin import jsonc  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
BUNDLED = os.path.join(REPO, "system", "extensions")

MANIFEST = {
    "id": "sample", "name": "Sample", "description": "A test package.", "version": "1.0", "api_version": 1,
    "entry": "main.lua", "permissions": ["commands", "chat.reply"], "dependencies": [],
    "settings": {
        "count": {"type": "integer", "label": "Count", "default": 7, "min": 0, "max": 10,
                  "choices": [7, {"value": 8, "label": "Eight"}, 9]},
        "text": {"type": "string", "default": "hi", "max_bytes": 4},
        "flag": {"type": "boolean", "default": False},
        "needed": {"type": "string", "required": True},
    },
}


def write_package(folder, manifest=None, script="extensions.command('x', {}, function() end)\n"):
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, "extension.json"), "w", encoding="utf-8") as f:
        json.dump(manifest or MANIFEST, f)
    with open(os.path.join(folder, "main.lua"), "w", encoding="utf-8") as f:
        f.write(script)


class ManifestTest(unittest.TestCase):
    @unittest.skipUnless(os.path.isdir(BUNDLED), "system/extensions is not here")
    def test_bundled_packages(self):
        packages = ext.discover(os.path.dirname(BUNDLED))
        self.assertGreaterEqual(len(packages), 14)
        for package in packages:
            self.assertEqual(package.error, "", package.id)
            self.assertNotEqual(package.name, package.id, "%s has no display name" % package.id)
            self.assertTrue(package.description, package.id)
            for spec in package.settings:
                self.assertNotEqual(spec.label, spec.name, "%s.%s has no label" % (package.id, spec.name))
                if spec.has_default:
                    spec.check(spec.default)
            with open(os.path.join(package.folder, "extension.json"), "rb") as f:
                self.assertLessEqual(len(f.read()), ext.MAX_MANIFEST, package.id)
        by_id = {p.id: p for p in packages}
        self.assertEqual([c.value for c in by_id["player-of-the-period"].setting("period").choices], ["week", "month"])
        self.assertEqual(by_id["lobby-decor"].setting("spot").default, "teleporter-west")
        self.assertIs(by_id["lobby-decor"].setting("show_on_join").default, True)

    @unittest.skipUnless(os.path.isdir(BUNDLED), "system/extensions is not here")
    def test_bundled_scripts_read_only_declared_settings(self):
        import re
        for package in ext.discover(os.path.dirname(BUNDLED)):
            with open(os.path.join(package.folder, package.manifest["entry"]), encoding="utf-8") as f:
                used = set(re.findall(r'extensions\.setting\("([a-z0-9_-]+)"\)', f.read()))
            declared = {spec.name for spec in package.settings}
            self.assertLessEqual(used, declared, package.id)

    def test_parse_and_reject(self):
        manifest, specs = ext.parse_manifest(json.dumps(MANIFEST), "sample")
        self.assertEqual([s.name for s in specs], ["count", "text", "flag", "needed"])
        self.assertEqual(specs[0].choice_label(8), "Eight")
        bad = [
            ({"id": "Bad Id"}, "id"), ({"api_version": 2}, "api_version"), ({"entry": "../x.lua"}, "inside"),
            ({"entry": "main.py"}, ".lua"), ({"permissions": ["root"]}, "unknown permission"),
            ({"name": "x" * 65}, "name"), ({"description": "bad\x01"}, "control"),
        ]
        for change, message in bad:
            with self.assertRaises(ValueError) as raised:
                ext.parse_manifest(json.dumps(dict(MANIFEST, **change)), "sample")
            self.assertIn(message, str(raised.exception))
        with self.assertRaises(ValueError):
            ext.parse_manifest(json.dumps(MANIFEST), "other-folder")
        for definition in [
                {"type": "integer", "default": 7, "min": 0, "max": 10, "choices": [7, 11]},  # out of range
                {"type": "integer", "default": 7, "choices": [7, 7]},  # duplicate
                {"type": "integer", "default": 7, "choices": []},
                {"type": "integer", "default": 6, "choices": [7, 8]},  # default not a choice
                {"type": "integer", "choices": [7, "8"]},  # wrong type
                {"type": "integer", "choices": [True]},  # booleans are not integers
                {"type": "boolean", "choices": [True]},
                {"type": "integer", "choices": [{"value": 1, "note": "x"}]},
                {"type": "string", "label": "x" * 65}, {"type": "string", "max_bytes": 1025},
                {"type": "integer", "min": 5, "max": 1}, {"type": "boolean", "min": 0},
                {"type": "string", "unknown": 1}, {"type": "float"}]:
            with self.assertRaises(ValueError, msg=definition):
                ext.SettingSpec("s", definition)

    def test_values(self):
        _, specs = ext.parse_manifest(json.dumps(MANIFEST))
        count, text, flag, needed = specs
        self.assertEqual(count.check(8), 8)
        for bad in (10, 11, True, "8", 8.0):
            with self.assertRaises(ValueError):
                count.check(bad)
        text.check("\u00e9\u00e9")  # 4 bytes
        with self.assertRaises(ValueError):
            text.check("\u00e9\u00e9\u00e9")  # 6 bytes: limits are bytes, as on the server
        with self.assertRaises(ValueError):
            text.check("a\0")
        with self.assertRaises(ValueError):
            flag.check(0)
        package = ext.Package("sample", "")
        package.settings = specs
        self.assertEqual(ext.check_overrides(package, {"needed": "x"}), [])
        self.assertTrue(any("must be set" in p for p in ext.check_overrides(package, {})))
        self.assertTrue(any("not a setting" in p for p in ext.check_overrides(package, {"needed": "x", "other": 1})))
        self.assertTrue(ext.check_overrides(package, {"needed": "x", "count": 10}))


class ConfigTest(unittest.TestCase):
    def package(self, package_id, dependencies=()):
        package = ext.Package(package_id, "")
        package.dependencies = list(dependencies)
        return package

    def test_round_trip_keeps_other_keys(self):
        value = {"Enabled": True, "Custom": 1, "Packages": [{"id": "a", "grants": ["commands"], "Extra": "kept"}]}
        config = ext.ExtensionsConfig(value)
        self.assertEqual(config.to_value(), value)
        config.enabled = False
        config.economy["MaxGrant"] = 5
        out = config.to_value()
        self.assertEqual(out["Custom"], 1)
        self.assertEqual(out["Economy"], {"MaxGrant": 5})
        self.assertEqual(out["Packages"][0]["Extra"], "kept")

    def test_pretty_writes_a_readable_group(self):
        value = {"Enabled": True, "Packages": [{"id": "a", "grants": ["commands", "chat.reply"],
                                                "Settings": {"text": 'quote " and \\', "n": 3}}]}
        base = '{\r\n  "ServerName": "x",\r\n}\r\n'
        text = jsonc.set_value(base, ("Extensions",), ext.pretty(value, 1, "\r\n"))
        self.assertEqual(jsonc.parse(text).to_python()["Extensions"], value)
        self.assertEqual(text.count("\r\n"), text.count("\n"))
        self.assertIn('    "Packages": [\r\n      {\r\n        "id": "a",', text)
        self.assertIn('"grants": ["commands", "chat.reply"]', text)  # short lists stay on one line

    def test_order_entries(self):
        packages = {"a": self.package("a"), "b": self.package("b", ["a"]), "c": self.package("c", ["b"])}
        ordered = ext.order_entries([{"id": "c"}, {"id": "b"}, {"id": "a"}], packages)
        self.assertEqual([e["id"] for e in ordered], ["a", "b", "c"])
        self.assertEqual([e["id"] for e in ext.order_entries([{"id": "a"}, {"id": "b"}], packages)], ["a", "b"])
        with self.assertRaises(ValueError):
            ext.order_entries([{"id": "c"}], packages)  # b is not enabled
        loop = {"x": self.package("x", ["y"]), "y": self.package("y", ["x"])}
        with self.assertRaises(ValueError):
            ext.order_entries([{"id": "x"}, {"id": "y"}], loop)


class InstallTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.mkdtemp(prefix="teaserv-ext-test-")
        self.system = os.path.join(self.temp, "system")
        self.backups = os.path.join(self.temp, "backups")
        os.makedirs(os.path.join(self.system, "extensions"))

    def tearDown(self):
        shutil.rmtree(self.temp, ignore_errors=True)

    def test_folder_with_any_name(self):
        source = os.path.join(self.temp, "downloaded copy")
        write_package(source)
        package, cleanup, root = ext.read_new_package(source)
        try:
            self.assertEqual(package.id, "sample")
            self.assertEqual(ext.install(self.system, package, root, self.backups), "")
        finally:
            cleanup()
        installed = ext.discover(self.system)
        self.assertEqual([(p.id, p.error) for p in installed], [("sample", "")])
        with self.assertRaises(FileExistsError):
            ext.install(self.system, package, root, self.backups)
        backup = ext.install(self.system, package, root, self.backups, replace=True)
        self.assertTrue(os.path.isfile(os.path.join(backup, "extension.json")))
        self.assertEqual([n for n in os.listdir(os.path.join(self.system, "extensions"))], ["sample"])  # no leftovers

    def test_zip_with_one_folder(self):
        source = os.path.join(self.temp, "src", "sample-1.0")
        write_package(source)
        archive = os.path.join(self.temp, "sample.zip")
        with zipfile.ZipFile(archive, "w") as z:
            for name in ("extension.json", "main.lua"):
                z.write(os.path.join(source, name), "sample-1.0/" + name)
        package, cleanup, root = ext.read_new_package(archive)
        try:
            ext.install(self.system, package, root, self.backups)
        finally:
            cleanup()
        self.assertFalse(os.path.exists(root))  # the extracted copy is gone
        self.assertTrue(os.path.isfile(os.path.join(self.system, "extensions", "sample", "main.lua")))

    def test_unsafe_zip_and_bad_packages(self):
        for name in ("../evil.txt", "/abs.txt", "C:/x.txt", "a/../../b.txt"):
            archive = os.path.join(self.temp, "bad.zip")
            with zipfile.ZipFile(archive, "w") as z:
                z.writestr("extension.json", json.dumps(MANIFEST))
                z.writestr("main.lua", "")
                z.writestr(name, "x")
            with self.assertRaises(ValueError, msg=name):
                ext.read_new_package(archive)
        self.assertFalse(os.path.exists(os.path.join(self.temp, "evil.txt")))
        missing_script = os.path.join(self.temp, "noscript")
        write_package(missing_script)
        os.remove(os.path.join(missing_script, "main.lua"))
        with self.assertRaises(ValueError):
            ext.read_new_package(missing_script)
        empty = os.path.join(self.temp, "empty")
        os.makedirs(empty)
        with self.assertRaises(ValueError):
            ext.read_new_package(empty)
        not_a_package = os.path.join(self.temp, "file.txt")
        with open(not_a_package, "w") as f:
            f.write("x")
        with self.assertRaises(ValueError):
            ext.read_new_package(not_a_package)

    def test_links_are_refused(self):
        source = os.path.join(self.temp, "linked")
        write_package(source)
        try:
            os.symlink(self.system, os.path.join(source, "link"), target_is_directory=True)
        except (OSError, NotImplementedError):
            self.skipTest("this system does not allow creating links")
        with self.assertRaises(ValueError):
            ext.read_new_package(source)

    def test_remove_moves_to_backup(self):
        folder = os.path.join(self.system, "extensions", "sample")
        write_package(folder)
        target = ext.move_to_backup(folder, self.backups)
        self.assertFalse(os.path.exists(folder))
        self.assertTrue(os.path.isfile(os.path.join(target, "extension.json")))
        write_package(folder)
        self.assertNotEqual(ext.move_to_backup(folder, self.backups), target)  # a second removal gets its own name


if __name__ == "__main__":
    unittest.main()
