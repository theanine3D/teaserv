"""The admin program's own settings (where the server is, how to start it). Not the server's configuration."""

import json
import os
import sys

IS_WINDOWS = sys.platform == "win32"
EXE_NAME = "teaserv.exe" if IS_WINDOWS else "teaserv"

DEFAULTS = {
    "server_dir": "",  # folder that holds the system folder
    "server_exe": "",  # empty: teaserv(.exe) inside server_dir
    "extra_path": "",  # folders with DLLs the server needs, added to PATH for the server only
    "start_server_on_launch": False,
    "stop_timeout_seconds": 30,
    "log_keep_days": 30,
    "stats_poll_seconds": 5,
}


def default_settings_path():
    if IS_WINDOWS:
        base = os.environ.get("APPDATA") or os.path.expanduser("~")
        return os.path.join(base, "TeaservAdmin", "settings.json")
    base = os.environ.get("XDG_CONFIG_HOME") or os.path.join(os.path.expanduser("~"), ".config")
    return os.path.join(base, "teaserv-admin", "settings.json")


def is_server_dir(path):
    return bool(path) and os.path.isdir(os.path.join(path, "system"))


def guess_server_dir():
    """A folder with a system folder: next to this program, above it, or the current folder."""
    here = os.path.dirname(os.path.abspath(sys.argv[0] if getattr(sys, "frozen", False) else __file__))
    candidates = [os.getcwd(), here]
    path = here
    for _ in range(4):
        path = os.path.dirname(path)
        candidates.append(path)
    for candidate in candidates:
        if is_server_dir(candidate) and (
                os.path.isfile(os.path.join(candidate, EXE_NAME)) or
                os.path.isfile(os.path.join(candidate, "system", "config.json"))):
            return candidate
    return ""


class AppSettings:
    def __init__(self, path=None):
        self.path = path or default_settings_path()
        self.values = dict(DEFAULTS)
        self.load()

    def load(self):
        try:
            with open(self.path, encoding="utf-8") as f:
                loaded = json.load(f)
        except (OSError, ValueError):
            return
        if isinstance(loaded, dict):
            for key, default in DEFAULTS.items():
                if key in loaded and isinstance(loaded[key], type(default)):
                    self.values[key] = loaded[key]

    def save(self):
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        temp = self.path + ".tmp"
        with open(temp, "w", encoding="utf-8") as f:
            json.dump(self.values, f, indent=2)
        os.replace(temp, self.path)

    def __getitem__(self, key):
        return self.values[key]

    def __setitem__(self, key, value):
        self.values[key] = value

    # Derived paths

    @property
    def server_dir(self):
        return self.values["server_dir"]

    @property
    def server_exe(self):
        return self.values["server_exe"] or os.path.join(self.server_dir, EXE_NAME)

    @property
    def system_dir(self):
        return os.path.join(self.server_dir, "system")

    @property
    def config_path(self):
        return os.path.join(self.system_dir, "config.json")

    @property
    def log_dir(self):
        return os.path.join(self.server_dir, "logs")

    @property
    def stats_dir(self):
        return os.path.join(self.system_dir, "stats")

    @property
    def backup_dir(self):
        return os.path.join(self.server_dir, "config-backups")

    def problems(self):
        """Why the server cannot be started with these settings (empty list: fine)."""
        found = []
        if not self.server_dir:
            found.append("The server folder is not set.")
        elif not is_server_dir(self.server_dir):
            found.append("The server folder has no \"system\" folder: %s" % self.server_dir)
        if self.server_dir and not os.path.isfile(self.server_exe):
            found.append("The server program was not found: %s" % self.server_exe)
        if is_server_dir(self.server_dir) and not os.path.isfile(self.config_path):
            found.append("system/config.json does not exist yet. Copy system/config.example.json to "
                         "system/config.json first.")
        return found
