"""Exercise actual migration staging, disk persistence, and restart recovery."""

import argparse
import json
import os
from pathlib import Path
import queue
import shutil
import subprocess
import tempfile
import threading
import time


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


class Server:
    def __init__(self, executable, config, source):
        self.output = queue.Queue()
        self.pending = ""
        self.history = ""
        self.process = subprocess.Popen(
            [executable, "--config=" + str(config)], cwd=source,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0)
        self.reader = threading.Thread(target=self.read_output, daemon=True)
        self.reader.start()

    def read_output(self):
        while True:
            data = os.read(self.process.stdout.fileno(), 4096)
            self.output.put(data.decode("utf-8", errors="replace") if data else None)
            if not data:
                return

    def prompt(self, timeout=150):
        deadline = time.monotonic() + timeout
        while "newserv> " not in self.pending:
            require(time.monotonic() < deadline, "server prompt timeout")
            try:
                data = self.output.get(timeout=0.1)
            except queue.Empty:
                continue
            require(data is not None, "server exited before prompt")
            self.pending += data
            self.history = (self.history + data)[-20000:]
        result, self.pending = self.pending.split("newserv> ", 1)
        return result

    def command(self, command):
        self.process.stdin.write((command + "\n").encode())
        self.process.stdin.flush()
        return self.prompt(30)

    def close(self):
        self.process.stdin.close()
        require(self.process.wait(timeout=30) == 0, "server shutdown failed")
        self.reader.join(timeout=5)
        require(not self.reader.is_alive(), "output reader did not exit")

    def kill(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait(timeout=30)
        self.reader.join(timeout=5)


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
    packages = []
    state_files = []
    temporary = None
    blocked = None
    server = None
    try:
        # Sorting makes the deliberately failing writer precede the healthy one.
        for prefix in ["storage-a-", "storage-z-"]:
            folder = Path(tempfile.mkdtemp(prefix=prefix, dir=package_root)).resolve()
            require(folder.parent == package_root and folder.name.startswith(prefix), "unsafe package test path")
            packages.append(folder)
            state = state_root / (folder.name + ".json")
            require(not state.exists() and not state.with_suffix(".json.tmp").exists(), "test state already exists")
            state_files.append(state)
        temporary = Path(tempfile.mkdtemp(prefix="storage-smoke-", dir=build)).resolve()
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
                {"id": folder.name, "grants": ["commands", "storage"]} for folder in packages]},
        })
        config_file = temporary / "config.json"
        config_file.write_text(json.dumps(config), encoding="utf-8")

        def stage(version, old_version):
            for folder in packages:
                manifest = {"id": folder.name, "version": "0.1.0", "api_version": 1,
                            "entry": "main.lua", "permissions": ["commands", "storage"],
                            "storage_version": version}
                (folder / "extension.json").write_text(json.dumps(manifest), encoding="utf-8")
                (folder / "main.lua").write_text(f'''
extensions.migrate(function(old, target)
  assert(old == {old_version} and target == {version})
  local owner = extensions.storage_get('owner')
  assert(owner == nil or owner == '{folder.name}')
  local count = tonumber(extensions.storage_get('count') or '100')
  extensions.storage_set('count', tostring(count + 1))
  extensions.storage_set('owner', '{folder.name}')
  extensions.storage_set('private', 'private-storage-value-not-for-diagnostics')
end)
extensions.command('{folder.name}', {{help='Storage smoke command'}}, function() end)
''', encoding="utf-8")

        def active(version):
            response = server.command("extensions list")
            for folder in packages:
                rows = [line for line in response.splitlines() if folder.name + " 0.1.0:" in line]
                require(len(rows) == 1 and "enabled" in rows[0] and f"storage v{version} " in rows[0],
                        "active package/storage version mismatch: " + response)

        def disk(version, counts):
            for folder, file, count in zip(packages, state_files, counts):
                data = json.loads(file.read_text(encoding="utf-8"))
                require(data == {"schema_version": 1, "data_version": version,
                                 "values": {"count": str(count), "owner": folder.name,
                                            "private": "private-storage-value-not-for-diagnostics"}},
                        "wrong persisted namespace/version/values: " + str(data))

        # Old envelopes without data_version are application version zero.
        state_files[0].write_text(json.dumps({"schema_version": 1, "values": {"count": "41"}}), encoding="utf-8")
        stage(1, 0)
        server = Server(args.server, config_file, source)
        server.prompt()
        active(1)
        require("FAILED:" not in server.command("extensions reload"), "initial migration could not flush/reload")
        disk(1, [42, 101])
        saved = [file.read_bytes() for file in state_files]

        stage(2, 1)
        original_info = [server.command("extensions info " + folder.name) for folder in packages]
        require(all("private-storage-value-not-for-diagnostics" not in info for info in original_info), "diagnostics exposed storage values")
        report = server.command("extensions check")
        require("Validation passed: 2 packages" in report and "storage v2" in report, "check did not stage migrations")
        active(1)
        require([file.read_bytes() for file in state_files] == saved, "successful check persisted candidate migrations")
        require([server.command("extensions info " + folder.name) for folder in packages] == original_info,
                "check mutated active version/health/storage metadata")
        valid_second = (packages[1] / "main.lua").read_text(encoding="utf-8")
        (packages[1] / "main.lua").write_text("error('later package rejects the candidate set')", encoding="utf-8")
        require("FAILED:" in server.command("extensions check"), "check accepted a late candidate failure")
        active(1)
        require([file.read_bytes() for file in state_files] == saved, "failed check persisted candidate migrations")
        require("FAILED:" in server.command("extensions reload"), "late migration staging failure was accepted")
        active(1)
        require([file.read_bytes() for file in state_files] == saved, "rejected staged migrations changed disk state")
        (packages[1] / "main.lua").write_text(valid_second, encoding="utf-8")
        require("FAILED:" not in server.command("extensions reload"), "valid migration failed")
        active(2)
        require("FAILED:" not in server.command("extensions reload"), "migrated data failed to flush")
        disk(2, [43, 102])
        stage(1, 0)
        require("FAILED:" in server.command("extensions reload"), "storage downgrade was accepted")
        active(2)
        disk(2, [43, 102])

        stage(3, 2)
        blocked = state_root / (packages[0].name + ".json.tmp")
        require(not blocked.exists(), "unexpected storage temporary path")
        blocked.mkdir()
        require("FAILED:" not in server.command("extensions reload"), "third-version staging failed")
        active(3)
        before_check = state_files[0].read_bytes()
        require("Validation passed:" in server.command("extensions check"), "check attempted to flush blocked active storage")
        require(state_files[0].read_bytes() == before_check, "check changed blocked active storage")
        require("pending save: yes" in server.command("extensions info " + packages[0].name), "check cleared pending dirty state")
        require("FAILED:" in server.command("extensions reload"), "failed disk replacement was ignored")
        active(3)
        require(json.loads(state_files[0].read_text())["data_version"] == 2, "failed writer replaced valid data")
        require(json.loads(state_files[1].read_text())["data_version"] == 3, "failed writer prevented another package saving")
        require(blocked.resolve().parent == state_root, "unsafe temporary blocker path")
        blocked.rmdir()
        blocked = None
        require("FAILED:" not in server.command("extensions reload"), "dirty storage could not retry after disk repair")
        disk(3, [44, 103])
        server.close()
        server = Server(args.server, config_file, source)
        server.prompt()
        active(3)
        require("FAILED:" not in server.command("extensions reload"), "restart re-ran a completed migration")
        disk(3, [44, 103])
        server.close()

        # A malformed file prevents activation without resetting persisted data.
        good = state_files[0].read_bytes()
        state_files[0].write_bytes(b'{"schema_version":1,"values":')
        server = Server(args.server, config_file, source)
        server.prompt()
        require("No extensions loaded" in server.command("extensions list"), "corrupt storage activated packages")
        require(state_files[0].read_bytes() == b'{"schema_version":1,"values":', "corrupt storage was silently reset")
        for invalid in [
            {"schema_version": 2, "data_version": 3, "values": {}},
            {"schema_version": 1, "data_version": -1, "values": {}},
            {"schema_version": 1, "data_version": 2147483648, "values": {}},
            {"schema_version": 1, "data_version": 4, "values": {}},
            {"schema_version": 1, "data_version": 3, "values": {"count": 123}},
            {"schema_version": 1, "data_version": 3, "values": {"": "invalid key"}},
        ]:
            raw = json.dumps(invalid).encode()
            state_files[0].write_bytes(raw)
            require("FAILED:" in server.command("extensions reload"), "invalid persisted version/values were accepted")
            require(state_files[0].read_bytes() == raw, "invalid persisted state was overwritten")
            require("No extensions loaded" in server.command("extensions list"), "invalid storage partially activated packages")
        state_files[0].write_bytes(good)
        require("FAILED:" not in server.command("extensions reload"), "repaired storage could not load")
        active(3)
        disk(3, [44, 103])
        server.close()
        print("Storage migration, namespace isolation, staged rollback, downgrade rejection, write failure/retry, restart and corrupt-file recovery passed")
    except Exception as error:
        raise RuntimeError(f"{error}\nLast server output:\n{server.history if server else ''}") from error
    finally:
        if server is not None:
            server.kill()
        if blocked is not None:
            require(blocked.resolve().parent == state_root, "unsafe blocker cleanup")
            blocked.rmdir()
        for file in state_files:
            for path in [file, file.with_suffix(".json.tmp")]:
                require(path.resolve().parent == state_root, "unsafe storage cleanup")
                if path.exists():
                    path.unlink()
        for folder in packages:
            require(folder.resolve().parent == package_root and folder.name.startswith("storage-"), "unsafe package cleanup")
            shutil.rmtree(folder)
        if temporary is not None:
            require(temporary.resolve().parent == build and temporary.name.startswith("storage-smoke-"), "unsafe config cleanup")
            shutil.rmtree(temporary)


if __name__ == "__main__":
    main()
