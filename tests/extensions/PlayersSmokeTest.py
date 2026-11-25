"""Exercise the real host: wall-clock schedules, per-player data and boards on
disk, the Meseta refusal for unknown players, restart recovery and invalid
settings. No game client is involved; live delivery to a client is a separate
acceptance check."""

import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time

from StorageSmokeTest import Server, require

KEY = "p" + "0123456789abcdef01234567"

SCRIPT = '''
extensions.on("package_load", function(info)
  local n = extensions.board_add("wins", "%KEY%", 1, "Tester")
  extensions.player_set("%KEY%", "loads", tostring(n))
  local ok, status = extensions.meseta_grant("%KEY%", 500, "smoke-" .. n, "smoke test")
  extensions.storage_set("grant", tostring(ok) .. " " .. status)
  local t = extensions.now()
  extensions.storage_set("clock", t.year .. " " .. t.month .. " " .. t.day .. " " .. t.weekday .. " " .. t.utc)
end)
extensions.schedule("every-minute", {every_minutes = 1}, function(info)
  local count = tonumber(extensions.storage_get("ticks") or "0") + 1
  extensions.storage_set("ticks", tostring(count))
  extensions.log("schedule fired " .. info.name .. " " .. count)
end)
'''.replace("%KEY%", KEY)


def main():
    parser = argparse.ArgumentParser()
    for field in ["source", "server", "jsonformat", "build"]:
        parser.add_argument("--" + field, required=True)
    args = parser.parse_args()
    source = Path(args.source).resolve()
    build = Path(args.build).resolve()
    package_root = (source / "system/extensions").resolve()
    state_root = source / "system/extensions-state"
    state_root.mkdir(parents=True, exist_ok=True)
    state_root = state_root.resolve()
    require(state_root.is_relative_to(source), "test state must remain inside the data fixture")
    host_file = state_root / "host.players.json"
    host_backup = host_file.read_bytes() if host_file.exists() else None
    folder = None
    temporary = None
    server = None
    files = []
    try:
        folder = Path(tempfile.mkdtemp(prefix="players-", dir=package_root)).resolve()
        require(folder.parent == package_root, "unsafe package test path")
        name = folder.name
        files = [state_root / (name + ".json"), state_root / (name + ".players.json")]
        grants = ["events", "schedule", "storage", "players.store", "boards", "economy.meseta"]
        (folder / "extension.json").write_text(json.dumps({
            "id": name, "version": "0.1.0", "api_version": 1, "entry": "main.lua",
            "permissions": grants}), encoding="utf-8")
        (folder / "main.lua").write_text(SCRIPT, encoding="utf-8")
        temporary = Path(tempfile.mkdtemp(prefix="players-smoke-", dir=build)).resolve()
        normalized = temporary / "base.json"
        subprocess.run([args.jsonformat, str(source / "tests/config.json"), str(normalized)], check=True)
        config = json.loads(normalized.read_text(encoding="utf-8"))
        config.update({
            "LocalAddress": "127.0.0.1", "ExternalAddress": "127.0.0.1",
            "PortConfiguration": {}, "DNSServerPort": 0, "HTTPListen": [],
            "IPStackListen": [], "PPPStackListen": [], "PPPRawListen": [],
            "RunInteractiveShell": True, "User": "", "WorkerThreads": 1,
            "Episode3LobbyBanners": [],
            "Extensions": {"Enabled": True, "TimeZone": "UTC",
                           "Economy": {"MaxGrant": 1000, "DailyLimitPerPackage": 5000, "BalanceCap": 99999},
                           "Packages": [{"id": name, "grants": grants}]},
        })
        config_file = temporary / "config.json"
        config_file.write_text(json.dumps(config), encoding="utf-8")

        server = Server(args.server, config_file, source)
        server.prompt()
        listing = server.command("extensions list")
        require(name + " 0.1.0: enabled" in listing, "package not enabled: " + listing)
        require("Schedules: 1 registered; clock: UTC" in listing, "schedule or clock line missing: " + listing)
        require("limits: 1000 per grant, 5000 per package per day, balance cap 99999" in listing,
                "economy limits not read: " + listing)
        require("0 known Episode 3 accounts, 0 queued Meseta grants" in listing, "unexpected player registry: " + listing)

        # The schedule fires on a wall-clock minute boundary.
        deadline = time.monotonic() + 130
        fired = False
        while time.monotonic() < deadline and not fired:
            time.sleep(5)
            # The schedule adds the third storage key ("ticks"); values are never shown.
            fired = "(3 keys)" in server.command("extensions list")
        require(fired, "the every-minute schedule did not fire within two minutes")

        require("FAILED:" not in server.command("extensions reload"), "reload failed")
        storage = json.loads(files[0].read_text(encoding="utf-8"))["values"]
        require(storage["grant"] == "false not_ep3", "grant to an unknown player was not refused: " + str(storage))
        require(int(storage["ticks"]) >= 1, "schedule storage write lost")
        year, month, day, weekday, utc = (int(x) for x in storage["clock"].split())
        now = time.gmtime()
        require(abs(utc - time.time()) < 600 and year == now.tm_year and month == now.tm_mon and
                1 <= weekday <= 7, "clock fields wrong: " + storage["clock"])
        players = json.loads(files[1].read_text(encoding="utf-8"))
        # Reload flushes the old instance's data before the new instance's load handler runs,
        # so the file holds the first load only.
        require(players["schema_version"] == 1 and players["players"][KEY]["values"]["loads"] == "1",
                "player store not persisted: " + str(players))
        entries = players["boards"]["wins"]["entries"]
        require(len(entries) == 1 and entries[0]["player_key"] == KEY and entries[0]["score"] == 1 and
                entries[0]["name"] == "Tester", "board not persisted: " + str(players))
        require(players["players"][KEY]["grants"] == [], "a refused grant consumed its grant ID")
        host = json.loads(host_file.read_text(encoding="utf-8"))
        require(re.fullmatch("[0-9a-f]{64}", host["secret"]) and host["players"] == {} and host["pending"] == {},
                "host player file wrong: " + str({k: v for k, v in host.items() if k != "secret"}))
        secret = host["secret"]

        # Invalid settings reject the reload and keep the active set.
        config["Extensions"]["TimeZone"] = "Mars"
        config_file.write_text(json.dumps(config), encoding="utf-8")
        server.command("reload config")
        require("FAILED:" in server.command("extensions reload"), "invalid TimeZone accepted")
        require(name + " 0.1.0: enabled" in server.command("extensions list"), "failed reload dropped the package")
        config["Extensions"]["TimeZone"] = "local"
        config["Extensions"]["Economy"]["MaxGrant"] = 0
        config_file.write_text(json.dumps(config), encoding="utf-8")
        server.command("reload config")
        require("FAILED:" in server.command("extensions reload"), "invalid economy limit accepted")
        config["Extensions"]["Economy"]["MaxGrant"] = 1000
        config_file.write_text(json.dumps(config), encoding="utf-8")
        server.command("reload config")
        require("FAILED:" not in server.command("extensions reload"), "repaired settings rejected")
        require("clock: server local time" in server.command("extensions list"), "time zone change not applied")
        before = json.loads(files[1].read_text(encoding="utf-8"))["boards"]["wins"]["entries"][0]["score"]
        require(before == 2, "second load not flushed by the next reload")
        server.close()

        # Restart: data and the key secret survive.
        server = Server(args.server, config_file, source)
        server.prompt()
        require("FAILED:" not in server.command("extensions reload"), "reload after restart failed")
        players = json.loads(files[1].read_text(encoding="utf-8"))
        # At least the flushed value plus this run's startup load.
        require(players["boards"]["wins"]["entries"][0]["score"] > before, "board did not survive restart: " + str(players))
        require(json.loads(host_file.read_text(encoding="utf-8"))["secret"] == secret, "player key secret changed on restart")
        server.close()

        # A corrupt player file is reported and never reset.
        files[1].write_bytes(b'{"schema_version":1,"players":')
        server = Server(args.server, config_file, source)
        server.prompt()
        server.command("extensions list")
        require(files[1].read_bytes() == b'{"schema_version":1,"players":', "corrupt player data was silently reset")
        server.close()
        print("Schedules, clock, player store, boards, Meseta refusal, settings validation, restart and corrupt-file handling passed")
    except Exception as error:
        raise RuntimeError(f"{error}\nLast server output:\n{server.history if server else ''}") from error
    finally:
        if server is not None:
            server.kill()
        for file in files:
            for path in [file, Path(str(file) + ".tmp")]:
                require(path.resolve().parent == state_root, "unsafe storage cleanup")
                if path.exists():
                    path.unlink()
        if host_backup is None:
            if host_file.exists():
                host_file.unlink()
        else:
            host_file.write_bytes(host_backup)
        if folder is not None:
            require(folder.resolve().parent == package_root and folder.name.startswith("players-"), "unsafe package cleanup")
            shutil.rmtree(folder)
        if temporary is not None:
            require(temporary.resolve().parent == build and temporary.name.startswith("players-smoke-"), "unsafe config cleanup")
            shutil.rmtree(temporary)


if __name__ == "__main__":
    main()
