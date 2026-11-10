"""Exercise committed package lifecycle ordering and persistence in real servers."""

import argparse
import json
from pathlib import Path
import queue
import re
import shutil
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
from StorageSmokeTest import Server, require


def events(output):
    return re.findall(r"LIFECYCLE ([a-z0-9_-]+) (load|unload) (startup|reload|disable|shutdown) ([0-9]+)", output)


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
    require(state_root.is_relative_to(source), "test state must remain inside fixture")
    folders = []
    state_files = []
    temporary = None
    server = None
    try:
        # Dependency order deliberately opposes map/lexicographic order.
        for prefix in ["lifecycle-z-", "lifecycle-a-"]:
            folder = Path(tempfile.mkdtemp(prefix=prefix, dir=package_root)).resolve()
            require(folder.parent == package_root and folder.name.startswith(prefix), "unsafe package test path")
            folders.append(folder)
            state = state_root / (folder.name + ".json")
            require(not state.exists() and not state.with_suffix(".json.tmp").exists(), "test state already exists")
            state_files.append(state)
        base, dependent = [folder.name for folder in folders]
        temporary = Path(tempfile.mkdtemp(prefix="package-lifecycle-", dir=build)).resolve()
        require(temporary.parent == build, "unsafe configuration path")
        normalized = temporary / "base.json"
        subprocess.run([args.jsonformat, str(source / "tests/config.json"), str(normalized)], check=True)
        config = json.loads(normalized.read_text(encoding="utf-8"))
        config.update({
            "LocalAddress": "127.0.0.1", "ExternalAddress": "127.0.0.1",
            "PortConfiguration": {}, "DNSServerPort": 0, "HTTPListen": [],
            "IPStackListen": [], "PPPStackListen": [], "PPPRawListen": [],
            "RunInteractiveShell": True, "User": "", "WorkerThreads": 1,
            "Episode3LobbyBanners": [],
            "Extensions": {"Enabled": True, "Packages": [
                {"id": folder.name, "grants": ["events", "storage"]} for folder in folders]},
        })
        config.setdefault("LogLevels", {})["Config"] = "INFO"
        config_file = temporary / "config.json"
        config_file.write_text(json.dumps(config), encoding="utf-8")

        def stage(version="0.1.0", load_error=False, unload_write=False):
            for index, folder in enumerate(folders):
                manifest = {"id": folder.name, "version": version, "api_version": 1,
                            "entry": "main.lua", "permissions": ["events", "storage"],
                            "dependencies": [base] if index else []}
                (folder / "extension.json").write_text(json.dumps(manifest), encoding="utf-8")
                source_text = '''
extensions.on("package_load", function(info)
  local count = tonumber(extensions.storage_get("loads") or "0") + 1
  extensions.storage_set("loads", tostring(count))
  extensions.log("LIFECYCLE " .. info.id .. " load " .. info.reason .. " " .. count)
  LOAD_ERROR
end)
extensions.on("package_unload", function(info)
  extensions.log("LIFECYCLE " .. info.id .. " unload " .. info.reason .. " " .. extensions.storage_get("loads"))
  UNLOAD_WRITE
end)
'''
                source_text = source_text.replace("LOAD_ERROR", "error('injected load failure')" if index and load_error else "")
                source_text = source_text.replace("UNLOAD_WRITE", "extensions.storage_set('loads', '999')" if index and unload_write else "")
                (folder / "main.lua").write_text(source_text, encoding="utf-8")

        def expected_load(reason, counts):
            return [(base, "load", reason, str(counts[0])), (dependent, "load", reason, str(counts[1]))]

        def expected_unload(reason, counts):
            return [(dependent, "unload", reason, str(counts[1])), (base, "unload", reason, str(counts[0]))]

        def disk(counts):
            for path, count in zip(state_files, counts):
                require(json.loads(path.read_text(encoding="utf-8"))["values"] == {"loads": str(count)},
                        "persisted lifecycle count changed unexpectedly")

        def close_output():
            server.close()
            result = server.pending
            while True:
                try:
                    chunk = server.output.get_nowait()
                except queue.Empty:
                    break
                if chunk is not None:
                    result += chunk
            return result

        stage()
        server = Server(args.server, config_file, source)
        require(events(server.prompt()) == expected_load("startup", [1, 1]), "startup did not follow dependency order")
        for folder in folders:
            require("Callbacks: 1; failures: 0" in server.command("extensions info " + folder.name), "startup load was not recorded")
        stage("0.2.0")
        require(events(server.command("extensions check")) == [], "check emitted candidate load or unload")
        require(base + " 0.1.0" in server.command("extensions list"), "check activated replacement")
        (folders[1] / "main.lua").write_text("error('injected staging failure')", encoding="utf-8")
        for operation in ["extensions check", "extensions reload"]:
            output = server.command(operation)
            require("FAILED:" in output and not events(output), "failed staging emitted lifecycle events")
        for folder in folders:
            require("Callbacks: 1; failures: 0" in server.command("extensions info " + folder.name), "failed staging changed active lifecycle state")
        disk([1, 1])  # Reload flushed the previously active dirty stores.
        stage("0.2.0")
        output = server.command("extensions reload")
        require("FAILED:" not in output and events(output) == expected_unload("reload", [1, 1]) + expected_load("reload", [2, 2]),
                "reload lifecycle order or copied storage was wrong")
        output = server.command("extensions disable " + base)
        require("FAILED:" in output and not events(output), "dependency refusal emitted unload")
        output = server.command("extensions disable " + dependent)
        require(events(output) == [(dependent, "unload", "disable", "2")], "dependent disable emitted wrong unload")
        output = server.command("extensions disable " + base)
        require(events(output) == [(base, "unload", "disable", "2")], "base disable emitted wrong unload")
        require("No extensions loaded" in server.command("extensions list"), "disabled package remained active")
        disk([2, 2])
        output = server.command("extensions reload")
        require(events(output) == expected_load("reload", [3, 3]), "re-enabling disabled packages lost state")

        # Removing a configured package still retires the prior runtime.
        all_entries = config["Extensions"]["Packages"]
        config["Extensions"]["Packages"] = all_entries[:1]
        config_file.write_text(json.dumps(config), encoding="utf-8")
        require(not events(server.command("reload config")), "config snapshot reload emitted package callbacks")
        output = server.command("extensions reload")
        require(events(output) == expected_unload("reload", [3, 3]) + [(base, "load", "reload", "4")], "removed package was not unloaded")

        config["Extensions"]["Packages"] = all_entries
        config_file.write_text(json.dumps(config), encoding="utf-8")
        server.command("reload config")
        stage("0.2.0", load_error=True, unload_write=True)
        output = server.command("extensions reload")
        require("FAILED:" not in output and events(output) == [(base, "unload", "reload", "4")] + expected_load("reload", [5, 4]),
                "post-activation script failure rolled back or aborted activation")
        require("Callbacks: 1; failures: 1" in server.command("extensions info " + dependent), "load failure was not isolated in diagnostics")
        require("Callbacks: 1; failures: 0" in server.command("extensions info " + base), "load failure damaged another package")
        output = server.command("extensions check")
        require("Validation passed:" in output and not events(output), "check executed failing load callback")
        output = server.command("extensions disable " + dependent)
        require("FAILED:" not in output and "read-only" in output and events(output) == [(dependent, "unload", "disable", "4")],
                "unload error prevented disable or escaped storage restriction")
        disk([5, 4])
        require(events(close_output()) == [(base, "unload", "shutdown", "5")], "shutdown missed or duplicated remaining unload")
        disk([5, 4])

        stage("0.2.0")
        server = Server(args.server, config_file, source)
        require(events(server.prompt()) == expected_load("startup", [6, 5]), "restart lost activation counts")
        config["Extensions"]["Enabled"] = False
        config_file.write_text(json.dumps(config), encoding="utf-8")
        server.command("reload config")
        require(not events(server.command("extensions check")), "disabled-config check emitted notifications")
        require(events(server.command("extensions reload")) == expected_unload("reload", [6, 5]), "configuration disable did not retire old packages")
        require("No extensions loaded" in server.command("extensions list"), "configuration disable left active packages")
        disk([6, 5])
        config["Extensions"]["Enabled"] = True
        config_file.write_text(json.dumps(config), encoding="utf-8")
        server.command("reload config")
        require(events(server.command("extensions reload")) == expected_load("reload", [7, 6]), "configuration re-enable lost state")
        require(events(close_output()) == expected_unload("shutdown", [7, 6]), "shutdown did not reverse dependency order")
        disk([7, 6])
        print("Committed load/unload order, failed staging, removal/disable, scoped faults, persistence and shutdown passed")
    except Exception as error:
        detail = server.history if server is not None else ""
        raise RuntimeError(f"{error}\nLast server output:\n{detail[-16000:]}") from error
    finally:
        if server is not None:
            server.kill()
        for path in state_files:
            require(path.resolve().parent == state_root, "unsafe state cleanup path")
            path.unlink(missing_ok=True)
            path.with_suffix(".json.tmp").unlink(missing_ok=True)
        for folder in folders:
            require(folder.resolve().parent == package_root, "unsafe package cleanup path")
            shutil.rmtree(folder)
        if temporary is not None:
            require(temporary.resolve().parent == build, "unsafe configuration cleanup path")
            shutil.rmtree(temporary)


if __name__ == "__main__":
    main()
