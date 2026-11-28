"""Lua extension packages for the Extensions page: finding, checking, installing and removing packages, and the
"Extensions" object of system/config.json.

The checks follow the server (src/Extensions/Manager.cc stage_config/read_settings and Settings.cc), so that a page
the admin can save is also one the server accepts; the server stays the final judge when it loads the packages.
Nothing here runs package code. Settings live in config.json (Extensions.Packages[].Settings); a package declares
them, with labels, descriptions, defaults, ranges and choices, in its extension.json.
"""

import datetime
import json
import os
import re
import shutil
import stat
import tempfile
import zipfile

from . import jsonc

FOLDER = "extensions"  # system/extensions/<id>/
ID_RE = re.compile(r"^[a-z0-9_-]{1,64}$")
MAX_ENABLED = 16
MAX_MANIFEST = 16384
MAX_SOURCE = 256 * 1024
MAX_PACKAGE_BYTES = 64 * 1024 * 1024  # a whole package folder, for adding (the server limits each file itself)
MAX_PACKAGE_FILES = 2000
MAX_SETTINGS = 64
MAX_SETTINGS_BYTES = 16 * 1024
INT64_MIN, INT64_MAX = -(2 ** 63), 2 ** 63 - 1

# Grants the server knows (Manager.cc known_grants), with what they let a package do
GRANTS = {
    "commands": "add chat commands",
    "commands.read": "see the list of extension commands",
    "events": "react to players joining, leaving and playing",
    "timers": "run code later for a player",
    "storage": "keep its own saved data",
    "chat.reply": "answer the player who used its command",
    "chat.broadcast": "send messages to everyone in a lobby or game",
    "chat.whisper": "send private messages to players",
    "chat.announce": "send announcements to the whole server",
    "scene.media": "show pictures and models in Episode 3 lobbies",
    "scene.event": "change lobby events (decorations)",
    "lobby.read": "see who is in a lobby and where players stand",
    "schedule": "run code at set times",
    "players.store": "keep data for each player account",
    "boards": "keep score boards",
    "economy.meseta": "give players Meseta (within the Economy limits)",
    "caelum.scene": "show objects and play music in Caelum-only lobbies",
}

TIME_ZONES = [("local", "This PC's time zone"), ("UTC", "UTC")]
ECONOMY = [  # key, label, default, min, max (ManagerPlayers.cc read_player_settings)
    ("MaxGrant", "Most Meseta one extension may give at once", 100000, 1, 1000000000),
    ("DailyLimitPerPackage", "Most Meseta one extension may give per day", 10000000, 1, 1000000000000),
    ("BalanceCap", "Extensions never raise a player's Meseta above", 9999999, 1, 0xFFFFFFFF),
]


class Choice:
    def __init__(self, value, label):
        self.value = value
        self.label = label


class SettingSpec:
    """One declared setting (extension.json "settings"), with the server's rules for its values."""

    def __init__(self, name, definition):
        if not isinstance(name, str) or not ID_RE.match(name):
            raise ValueError("setting name %r is not valid (1-64 lowercase letters, digits, - or _)" % (name,))
        if not isinstance(definition, dict):
            raise ValueError("setting %s must be an object" % name)
        self.name = name
        self.type = definition.get("type")
        if self.type not in ("boolean", "integer", "string"):
            raise ValueError("setting %s has an unsupported type" % name)
        self.label = name
        self.description = ""
        self.default = None
        self.has_default = False
        self.required = False
        self.min, self.max = INT64_MIN, INT64_MAX
        self.max_bytes = 1024
        self.choices = []
        for key, value in definition.items():
            if key == "type":
                continue
            if key == "default":
                self.default, self.has_default = value, True
            elif key == "required":
                if not isinstance(value, bool):
                    raise ValueError("setting %s: required must be true or false" % name)
                self.required = value
            elif key in ("min", "max") and self.type == "integer":
                if not is_int(value):
                    raise ValueError("setting %s: %s must be a whole number" % (name, key))
                setattr(self, key, value)
            elif key == "max_bytes" and self.type == "string":
                if not is_int(value) or not 1 <= value <= 1024:
                    raise ValueError("setting %s: max_bytes must be 1..1024" % name)
                self.max_bytes = value
            elif key in ("label", "description"):
                check_text(value, 64 if key == "label" else 512, "setting %s: %s" % (name, key))
                setattr(self, key, value)
            elif key == "choices" and self.type in ("integer", "string"):
                if not isinstance(value, list) or not value:
                    raise ValueError("setting %s: choices must be a non-empty list" % name)
                for entry in value:
                    if isinstance(entry, dict):
                        if set(entry) - {"value", "label"} or "value" not in entry:
                            raise ValueError("setting %s: a choice has only value and label" % name)
                        label = entry.get("label", "")
                        check_text(label, 64, "setting %s: choice label" % name)
                        self.choices.append(Choice(entry["value"], label or str(entry["value"])))
                    else:
                        self.choices.append(Choice(entry, str(entry)))
            else:
                raise ValueError("setting %s: unknown or inapplicable option %s" % (name, key))
        if self.min > self.max:
            raise ValueError("setting %s: min is larger than max" % name)
        if len(self.choices) > 64:
            raise ValueError("setting %s: at most 64 choices" % name)
        seen = []
        for choice in self.choices:
            self.check_basic(choice.value)
            if any(same(choice.value, other) for other in seen):
                raise ValueError("setting %s: a choice is listed twice" % name)
            seen.append(choice.value)
        if self.has_default:
            self.check(self.default)

    def check_basic(self, value):
        if self.type == "boolean":
            ok = isinstance(value, bool)
        elif self.type == "integer":
            ok = is_int(value)
        else:
            ok = isinstance(value, str)
        if not ok:
            raise ValueError("%s must be %s" % (self.label, {"boolean": "on or off", "integer": "a whole number",
                                                              "string": "text"}[self.type]))
        if self.type == "integer" and not self.min <= value <= self.max:
            raise ValueError("%s must be between %d and %d" % (self.label, self.min, self.max))
        if self.type == "string":
            if len(value.encode("utf-8")) > self.max_bytes:
                raise ValueError("%s is too long: at most %d bytes (now %d)" % (
                    self.label, self.max_bytes, len(value.encode("utf-8"))))
            if "\0" in value:
                raise ValueError("%s must not contain a NUL character" % self.label)

    def check(self, value):
        """Raise ValueError (a message for the admin) unless the server accepts `value` for this setting."""
        self.check_basic(value)
        if self.choices and not any(same(value, choice.value) for choice in self.choices):
            raise ValueError("%s must be one of: %s" % (self.label, ", ".join(c.label for c in self.choices)))
        return value

    def choice_label(self, value):
        for choice in self.choices:
            if same(choice.value, value):
                return choice.label
        return str(value)

    def describe_default(self):
        if not self.has_default:
            return "no default (required)" if self.required else "no default"
        if self.type == "boolean":
            return "on" if self.default else "off"
        if self.choices:
            return self.choice_label(self.default)
        return '"%s"' % self.default if self.type == "string" else str(self.default)


class Package:
    """One folder in system/extensions. `error` is set (and the rest may be partial) when it cannot be used."""

    def __init__(self, id, folder):
        self.id = id
        self.folder = folder
        self.manifest = {}
        self.name = id
        self.description = ""
        self.version = ""
        self.permissions = []
        self.dependencies = []
        self.settings = []  # SettingSpec, in manifest order
        self.error = ""

    def setting(self, name):
        for spec in self.settings:
            if spec.name == name:
                return spec
        return None


def is_int(value):
    return isinstance(value, int) and not isinstance(value, bool)


def same(a, b):
    """Equal values of the same JSON type (True is not 1)."""
    return type(a) is type(b) and a == b


def check_text(value, limit, what):
    if not isinstance(value, str):
        raise ValueError("%s must be text" % what)
    if len(value.encode("utf-8")) > limit:
        raise ValueError("%s is longer than %d bytes" % (what, limit))
    if any(ord(ch) < 0x20 and ch != "\n" for ch in value):
        raise ValueError("%s contains control characters" % what)


def contained(root, relative):
    """Path of `relative` inside `root`, or ValueError if it would leave it (like the server's contained_path)."""
    if not isinstance(relative, str) or not relative or os.path.isabs(relative) or "\\" in relative or \
            ":" in relative or "\0" in relative:
        raise ValueError("path %r must be a plain relative path inside the package" % (relative,))
    parts = relative.split("/")
    if any(part in ("", ".", "..") for part in parts):
        raise ValueError("path %r must stay inside the package" % (relative,))
    return os.path.join(root, *parts)


def parse_manifest(text, folder_name=None):
    """(manifest dict, [SettingSpec]) for extension.json text, or ValueError explaining the first problem."""
    if len(text.encode("utf-8")) > MAX_MANIFEST:
        raise ValueError("extension.json is larger than 16 KiB")
    try:
        manifest = json.loads(text)
    except ValueError as e:
        raise ValueError("extension.json is not valid JSON: %s" % e)
    if not isinstance(manifest, dict):
        raise ValueError("extension.json must contain an object")
    package_id = manifest.get("id")
    if not isinstance(package_id, str) or not ID_RE.match(package_id):
        raise ValueError("the id must be 1-64 lowercase letters, digits, - or _")
    if folder_name is not None and folder_name != package_id:
        raise ValueError("the folder is named %s but the id is %s" % (folder_name, package_id))
    if manifest.get("api_version") != 1 or isinstance(manifest.get("api_version"), bool):
        raise ValueError("api_version must be 1 (this server supports extension API 1 only)")
    if not isinstance(manifest.get("version"), str):
        raise ValueError("version must be text")
    entry = manifest.get("entry")
    if not isinstance(entry, str) or not entry.endswith(".lua"):
        raise ValueError("entry must name a .lua file")
    contained(".", entry)
    permissions = manifest.get("permissions")
    if not isinstance(permissions, list) or not all(isinstance(p, str) for p in permissions):
        raise ValueError("permissions must be a list of names")
    unknown = [p for p in permissions if p not in GRANTS]
    if unknown:
        raise ValueError("unknown permission %s" % unknown[0])
    if len(set(permissions)) != len(permissions):
        raise ValueError("a permission is listed twice")
    dependencies = manifest.get("dependencies", [])
    if not isinstance(dependencies, list) or not all(isinstance(d, str) and ID_RE.match(d) for d in dependencies):
        raise ValueError("dependencies must be a list of package ids")
    for key, limit in (("name", 64), ("description", 512)):
        if key in manifest:
            check_text(manifest[key], limit, key)
    definitions = manifest.get("settings", {})
    if not isinstance(definitions, dict):
        raise ValueError("settings must be an object")
    if len(definitions) > MAX_SETTINGS:
        raise ValueError("at most 64 settings")
    specs = [SettingSpec(name, definition) for name, definition in definitions.items()]
    return manifest, specs


def load_package(folder, match_folder_name=True):
    """A Package for a folder. With match_folder_name (installed packages) the id must equal the folder's name, as
    the server requires; a package that is about to be added may come from a folder with any name."""
    package = Package(os.path.basename(folder), folder)
    try:
        with open(os.path.join(folder, "extension.json"), encoding="utf-8") as f:
            text = f.read(MAX_MANIFEST + 1)
        manifest, specs = parse_manifest(text, package.id if match_folder_name else None)
        package.id = manifest["id"]
        source = contained(folder, manifest["entry"])
        if not os.path.isfile(source):
            raise ValueError("the entry script %s is missing" % manifest["entry"])
        if os.path.getsize(source) > MAX_SOURCE:
            raise ValueError("the entry script is larger than 256 KiB")
    except (OSError, UnicodeDecodeError) as e:
        package.error = "cannot read extension.json: %s" % e
        return package
    except ValueError as e:
        package.error = str(e)
        return package
    package.manifest = manifest
    package.name = manifest.get("name") or package.id
    package.description = manifest.get("description", "")
    package.version = manifest["version"]
    package.permissions = list(manifest["permissions"])
    package.dependencies = list(manifest.get("dependencies", []))
    package.settings = specs
    return package


def packages_dir(system_dir):
    return os.path.join(system_dir, FOLDER)


def discover(system_dir):
    """Every package folder in system/extensions, sorted by id (folders with no extension.json are skipped)."""
    root = packages_dir(system_dir)
    found = []
    try:
        names = sorted(os.listdir(root))
    except OSError:
        return found
    for name in names:
        folder = os.path.join(root, name)
        if name.startswith(".") or not os.path.isdir(folder) or os.path.islink(folder):
            continue
        if not os.path.isfile(os.path.join(folder, "extension.json")):
            continue
        found.append(load_package(folder))
    return found


# ----- settings values

def check_overrides(package, overrides):
    """Problems (messages) with a package's Settings object as the server would see them; [] when it is accepted."""
    problems = []
    if not isinstance(overrides, dict):
        return ["Settings must be an object"]
    if len(overrides) > MAX_SETTINGS:
        problems.append("at most 64 settings")
    total = 0
    for name, value in overrides.items():
        spec = package.setting(name)
        if spec is None:
            problems.append("%s is not a setting of this extension" % name)
            continue
        try:
            spec.check(value)
        except ValueError as e:
            problems.append(str(e))
    for spec in package.settings:
        value = overrides.get(spec.name, spec.default if spec.has_default else None)
        if spec.name not in overrides and not spec.has_default and spec.required:
            problems.append("%s must be set" % spec.label)
        total += len(spec.name.encode("utf-8"))
        if spec.name in overrides or spec.has_default:
            total += len(value.encode("utf-8")) if isinstance(value, str) else 8
    if total > MAX_SETTINGS_BYTES:
        problems.append("the settings of this extension are larger than 16 KiB together")
    return problems


# ----- the Extensions object of config.json

class ExtensionsConfig:
    """The parts of config.json's "Extensions" object the page edits. Other keys are kept as they are."""

    def __init__(self, value=None):
        value = value if isinstance(value, dict) else {}
        self.original = value
        self.enabled = value.get("Enabled", False) is True
        self.time_zone = value.get("TimeZone", "local")
        self.economy = dict(value.get("Economy", {})) if isinstance(value.get("Economy", {}), dict) else {}
        scenes = value.get("Scenes", {})
        self.scenes = dict(scenes) if isinstance(scenes, dict) else {}
        packages = value.get("Packages", [])
        self.entries = [dict(e) for e in packages if isinstance(e, dict)] if isinstance(packages, list) else []

    def entry(self, package_id):
        for entry in self.entries:
            if entry.get("id") == package_id:
                return entry
        return None

    def to_value(self):
        out = dict(self.original)
        out["Enabled"] = self.enabled
        if self.time_zone != "local" or "TimeZone" in out:
            out["TimeZone"] = self.time_zone
        if self.economy or "Economy" in out:
            out["Economy"] = self.economy
        if self.scenes or "Scenes" in out:
            out["Scenes"] = self.scenes
        out["Packages"] = self.entries
        return out


def order_entries(entries, packages_by_id):
    """Entries reordered so that each package comes after its dependencies (stable); ValueError if impossible."""
    remaining = list(entries)
    done, ordered = set(), []
    enabled = {e.get("id") for e in entries}
    for entry in entries:
        package = packages_by_id.get(entry.get("id"))
        for dependency in (package.dependencies if package else []):
            if dependency not in enabled:
                raise ValueError("%s needs the extension %s, which is not enabled" % (entry.get("id"), dependency))
    while remaining:
        for index, entry in enumerate(remaining):
            package = packages_by_id.get(entry.get("id"))
            if all(d in done for d in (package.dependencies if package else [])):
                ordered.append(entry)
                done.add(entry.get("id"))
                del remaining[index]
                break
        else:
            raise ValueError("the dependencies of %s form a loop" % ", ".join(e.get("id") for e in remaining))
    return ordered


def pretty(value, level=1, newline="\n"):
    """Multi-line text for a JSON value, indented for a key `level` groups deep (scalars via jsonc.serialize)."""
    pad = "  " * (level + 1)
    end = "  " * level
    if isinstance(value, dict):
        if not value:
            return "{}"
        items = ["%s%s: %s" % (pad, jsonc.serialize(str(k)), pretty(v, level + 1, newline)) for k, v in value.items()]
        return "{" + newline + ("," + newline).join(items) + newline + end + "}"
    if isinstance(value, list):
        if not value:
            return "[]"
        if all(not isinstance(v, (dict, list)) for v in value):
            return jsonc.serialize(value)  # short lists such as grants on one line
        items = ["%s%s" % (pad, pretty(v, level + 1, newline)) for v in value]
        return "[" + newline + ("," + newline).join(items) + newline + end + "]"
    return jsonc.serialize(value)


# ----- adding and removing packages

def _check_tree(root):
    """Refuse links, too many files or too many bytes in a package folder that is about to be copied."""
    count = total = 0
    for folder, dirs, files in os.walk(root):
        for name in dirs + files:
            info = os.lstat(os.path.join(folder, name))
            # Symbolic links, and on Windows also junctions (reparse points)
            if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
                raise ValueError("the package contains a link (%s); links are not allowed" % name)
        for name in files:
            count += 1
            total += os.path.getsize(os.path.join(folder, name))
        if count > MAX_PACKAGE_FILES or total > MAX_PACKAGE_BYTES:
            raise ValueError("the package is too large (at most %d files and %d MiB)" % (
                MAX_PACKAGE_FILES, MAX_PACKAGE_BYTES // (1024 * 1024)))


def _package_root(folder):
    """The folder holding extension.json: `folder` itself, or its only subfolder."""
    if os.path.isfile(os.path.join(folder, "extension.json")):
        return folder
    entries = [e for e in os.listdir(folder) if not e.startswith(".") and e != "__MACOSX"]
    if len(entries) == 1 and os.path.isdir(os.path.join(folder, entries[0])) and \
            os.path.isfile(os.path.join(folder, entries[0], "extension.json")):
        return os.path.join(folder, entries[0])
    raise ValueError("no extension.json found (it must be in the chosen folder or ZIP, or in its only folder)")


def read_new_package(source):
    """Check a folder or ZIP that the admin wants to add. Returns (Package, cleanup function, root folder)."""
    temp = None

    def cleanup():
        if temp:
            shutil.rmtree(temp, ignore_errors=True)

    try:
        if os.path.isdir(source):
            root = _package_root(source)
        elif zipfile.is_zipfile(source):
            temp = tempfile.mkdtemp(prefix="teaserv-extension-")
            extract_zip(source, temp)
            root = _package_root(temp)
        else:
            raise ValueError("choose a package folder or a ZIP file")
        _check_tree(root)
        # The source folder may have any name; the installed copy is named after the id
        package = load_package(root, match_folder_name=False)
        if package.error:
            raise ValueError(package.error)
        return package, cleanup, root
    except (OSError, UnicodeDecodeError, zipfile.BadZipFile) as e:
        cleanup()
        raise ValueError("cannot read the package: %s" % e)
    except ValueError:
        cleanup()
        raise


def extract_zip(zip_path, target):
    """Extract a ZIP into `target`, refusing entries that would land outside it, links, and oversized archives."""
    with zipfile.ZipFile(zip_path) as archive:
        infos = archive.infolist()
        if len(infos) > MAX_PACKAGE_FILES:
            raise ValueError("the ZIP has too many files")
        if sum(i.file_size for i in infos) > MAX_PACKAGE_BYTES:
            raise ValueError("the ZIP is too large when extracted")
        base = os.path.realpath(target)
        for info in infos:
            name = info.filename.replace("\\", "/")
            if name.startswith("/") or ":" in name or any(p == ".." for p in name.split("/")):
                raise ValueError("the ZIP contains an unsafe path: %s" % info.filename)
            mode = (info.external_attr >> 16) & 0xFFFF
            if stat.S_ISLNK(mode):
                raise ValueError("the ZIP contains a link: %s" % info.filename)
            path = os.path.realpath(os.path.join(target, *[p for p in name.split("/") if p]))
            if path != base and not path.startswith(base + os.sep):
                raise ValueError("the ZIP contains an unsafe path: %s" % info.filename)
            if name.endswith("/"):
                os.makedirs(path, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with archive.open(info) as src, open(path, "wb") as dst:
                shutil.copyfileobj(src, dst)


def install(system_dir, package, root, backup_dir, replace=False):
    """Copy a checked package into system/extensions/<id>. An existing copy is moved to the backup folder first
    (only with replace=True). Returns the backup path of the replaced copy, or ""."""
    target_root = packages_dir(system_dir)
    os.makedirs(target_root, exist_ok=True)
    target = os.path.join(target_root, package.id)
    if os.path.realpath(root) == os.path.realpath(target):
        raise ValueError("this package is already installed from this folder")
    if os.path.exists(target) and not replace:
        raise FileExistsError(target)
    staging = tempfile.mkdtemp(prefix=".incoming-%s-" % package.id, dir=target_root)
    try:
        copy = os.path.join(staging, package.id)
        shutil.copytree(root, copy, symlinks=False)
        backup = ""
        if os.path.exists(target):
            backup = move_to_backup(target, backup_dir)
        os.replace(copy, target)
        return backup
    finally:
        shutil.rmtree(staging, ignore_errors=True)


def move_to_backup(folder, backup_dir):
    """Move a package folder into <backup>/removed-extensions/<id>.<time> (it can be moved back by hand)."""
    removed = os.path.join(backup_dir, "removed-extensions")
    os.makedirs(removed, exist_ok=True)
    stamp = datetime.datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
    target = os.path.join(removed, "%s.%s" % (os.path.basename(folder), stamp))
    number = 1
    while os.path.exists(target):
        number += 1
        target = os.path.join(removed, "%s.%s_%d" % (os.path.basename(folder), stamp, number))
    shutil.move(folder, target)
    return target
