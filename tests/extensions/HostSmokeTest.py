"""Exercise real package staging/reload without opening server listeners."""

import argparse
import json
import os
from pathlib import Path
import queue
import re
import shutil
import subprocess
import tempfile
import threading
import time


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True)
    parser.add_argument("--server", required=True)
    parser.add_argument("--jsonformat", required=True)
    parser.add_argument("--build", required=True)
    args = parser.parse_args()
    source = Path(args.source).resolve()
    build = Path(args.build).resolve()
    package_root = (source / "system/extensions").resolve()
    package_dir = Path(tempfile.mkdtemp(prefix="smoke-local-", dir=package_root)).resolve()
    dependent_dir = Path(tempfile.mkdtemp(prefix="smoke-local-", dir=package_root)).resolve()
    temporary = Path(tempfile.mkdtemp(prefix="host-smoke-", dir=build)).resolve()
    # Verify the exact absolute cleanup targets before any recursive removal.
    require(package_dir.parent == package_root and package_dir.name.startswith("smoke-local-"), "unsafe package test directory")
    require(dependent_dir.parent == package_root and dependent_dir.name.startswith("smoke-local-"), "unsafe dependent test directory")
    require(temporary.parent == build and temporary.name.startswith("host-smoke-"), "unsafe config test directory")
    process = None
    pending = ""
    history = ""
    output = queue.Queue()
    banner_files = []
    try:
        normalized = temporary / "base.json"
        subprocess.run([args.jsonformat, str(source / "tests/config.json"), str(normalized)], check=True)
        config = json.loads(normalized.read_text(encoding="utf-8"))
        config.update({
            "LocalAddress": "127.0.0.1", "ExternalAddress": "127.0.0.1",
            "PortConfiguration": {}, "DNSServerPort": 0, "HTTPListen": [],
            "IPStackListen": [], "PPPStackListen": [], "PPPRawListen": [],
            "RunInteractiveShell": True, "User": "", "WorkerThreads": 1,
        })
        config.setdefault("LogLevels", {})["Config"] = "INFO"
        package_id = package_dir.name
        dependent_id = dependent_dir.name
        config["Episode3LobbyBanners"] = []
        (source / "system/ep3/banners").mkdir(parents=True, exist_ok=True)
        fallback_banner = source / "system/ep3/banners" / (package_id + "-fallback.bmp")
        require(not fallback_banner.exists(), "test fallback already exists")
        fallback_banner.write_bytes((package_root / "lobby-decor/assets/checker.bmp").read_bytes())
        banner_files.append(fallback_banner)
        for fixture, location in [("pc-noise", 0x10), ("pc-multi", 0x20)]:
            banner = source / "system/ep3/banners" / (package_id + "-" + fixture + ".gvm.prs")
            require(not banner.exists(), "test banner already exists")
            banner.write_bytes((build / "lobby-media-fixtures" / (fixture + ".gvm.prs")).read_bytes())
            banner_files.append(banner)
            config["Episode3LobbyBanners"].append([1, location, banner.name, fallback_banner.name])
        config["Extensions"] = {
            "Enabled": True, "Scenes": {"Enabled": True, "ExperimentalModels": False},
            "Packages": [{"id": package_id, "grants": ["commands", "commands.read", "scene.media", "lobby.read", "events"],
                          "Settings": {"count": 8, "label": "private-setting-value-not-for-diagnostics"}},
                         {"id": dependent_id, "grants": ["commands"]}],
        }
        manifest = {
            "id": package_id, "version": "0.1.0", "api_version": 1, "entry": "main.lua",
            "permissions": ["commands", "commands.read", "scene.media", "lobby.read", "events"],
            "assets": {"checker": {"texture": "checker.bmp"}},
            "settings": {"count": {"type": "integer", "default": 7, "min": 0, "max": 10},
                         "label": {"type": "string", "required": True, "max_bytes": 64},
                         "enabled": {"type": "boolean", "default": False},
                         "optional": {"type": "string"}},
        }
        manifest_file = package_dir / "extension.json"
        script_file = package_dir / "main.lua"
        image_file = package_dir / "checker.bmp"
        manifest_file.write_text(json.dumps(manifest), encoding="utf-8")
        valid_script = ('isolation_marker = 123\n'
                        'assert(type(extensions.lobby) == "function")\n'
                        'assert(type(extensions.commands) == "function")\n'
                        'assert(type(extensions.scene_asset_support) == "function")\n'
                        'extensions.on("game_opened",function() error("unexpected game opening") end)\n'
                        'extensions.on("game_closed",function() error("unexpected game closure") end)\n'
                        'extensions.on("game_join_ready",function() error("unexpected join readiness") end)\n'
                        'extensions.on("quest_assigned",function() error("unexpected quest assignment") end)\n'
                        'extensions.on("quest_map_loaded",function() error("unexpected quest map") end)\n'
                        'extensions.on("quest_load_acknowledged",function() error("unexpected quest acknowledgement") end)\n'
                        'extensions.on("quest_join_acknowledged",function() error("unexpected quest join acknowledgement") end)\n'
                        'assert(extensions.setting("count") == 8)\n'
                        'assert(extensions.setting("label") == "private-setting-value-not-for-diagnostics")\n'
                        'assert(extensions.setting("enabled") == false and extensions.setting("optional") == nil)\n'
                        f'extensions.command("{package_id}", {{cooldown_ms = extensions.setting("count") * 1000, help = "Smoke command", arguments = {{}}}}, function() end)\n')
        script_file.write_text(valid_script, encoding="utf-8")
        dependent_manifest = {
            "id": dependent_id, "version": "0.1.0", "api_version": 1, "entry": "main.lua",
            "permissions": ["commands"], "dependencies": [package_id],
        }
        (dependent_dir / "extension.json").write_text(json.dumps(dependent_manifest), encoding="utf-8")
        dependent_script = dependent_dir / "main.lua"
        valid_dependent = f'assert(isolation_marker == nil)\nextensions.command("{dependent_id}", {{}}, function() end)\n'
        dependent_script.write_text(valid_dependent, encoding="utf-8")
        valid_image = (package_root / "lobby-decor/assets/checker.bmp").read_bytes()
        image_file.write_bytes(valid_image)
        config_file = temporary / "config.json"
        config_file.write_text(json.dumps(config), encoding="utf-8")
        # Exercise configured fallbacks through real DataIndex loading, including
        # the independent compressed/output budgets and complete-group rule.
        good_banners = config["Episode3LobbyBanners"]
        for label, entries in [
                ("compressed", [[*good_banners[0][:3], good_banners[0][2]]]),
                ("expanded", [[*good_banners[1][:3], good_banners[1][2]]]),
                ("partial-group", [good_banners[0], [2, good_banners[0][1], good_banners[0][2]]]),
                ("duplicate-group", [good_banners[0], good_banners[0]])]:
            bad_config = dict(config, Episode3LobbyBanners=entries)
            bad_file = temporary / (label + ".json")
            bad_file.write_text(json.dumps(bad_config), encoding="utf-8")
            failed = subprocess.run([args.server, "--config=" + str(bad_file)], cwd=source,
                                    input=b"", stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
            require(failed.returncode != 0 and b"newserv> " not in failed.stdout,
                    "invalid configured fallback accepted: " + label + "\n" + failed.stdout.decode(errors="replace")[-2000:])
        process = subprocess.Popen(
            [args.server, "--config=" + str(config_file)], cwd=source,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0)

        def read_output():
            while True:
                data = os.read(process.stdout.fileno(), 4096)
                if not data:
                    output.put(None)
                    return
                output.put(data.decode("utf-8", errors="replace"))

        reader = threading.Thread(target=read_output, daemon=True)
        reader.start()

        def prompt(timeout=150):
            nonlocal pending, history
            deadline = time.monotonic() + timeout
            while "newserv> " not in pending:
                require(time.monotonic() < deadline, "server prompt timeout")
                try:
                    data = output.get(timeout=0.1)
                except queue.Empty:
                    continue
                require(data is not None, "server exited before prompt")
                pending += data
                history = (history + data)[-65536:]
            result, pending = pending.split("newserv> ", 1)
            return result

        def command(text):
            process.stdin.write((text + "\n").encode("utf-8"))
            process.stdin.flush()
            return prompt(30)

        def active():
            response = command("extensions list")
            require(re.search(re.escape(package_id) + r" 0\.1\.0: enabled", response), "active package was lost")
            require(re.search(re.escape(dependent_id) + r" 0\.1\.0: enabled", response), "dependent package was lost")
            require("1 commands" in response, "unexpected command registry after staging")

        prompt()
        active()
        original_info = command("extensions info " + package_id)
        require("Callbacks: 0; failures: 0" in original_info and "peak" in original_info and
                "Callback time (us): last=0; peak=0; total=0; over 10 ms=0" in original_info and
                "pending save: no" in original_info and "Smoke command" in original_info and
                "Usage: " + package_id in original_info, "package health/command information was incomplete")
        require("Dependencies: " + package_id in command("extensions info " + dependent_id), "dependency information was missing")
        require("FAILED:" in command("extensions info missing-package"), "unknown package info succeeded")
        require("FAILED:" in command("extensions info ../invalid"), "invalid package ID accepted")
        original_scenes = command("extensions scenes " + package_id)
        require("Assets: 1; owned objects: 0; event overrides: 0" in original_scenes and
                "Asset checker" in original_scenes and "Original-client fallback: none" in original_scenes and
                "compatible original=yes pc=yes" in original_scenes and "rendering are unconfirmed" in original_scenes,
                "prepared scene diagnostics were incomplete")
        require("Assets: 0; owned objects: 0; event overrides: 0" in command("extensions scenes " + dependent_id),
                "scene inspection leaked another package's catalog")
        require("FAILED:" in command("extensions scenes missing-package") and
                "FAILED:" in command("extensions scenes ../invalid") and
                "FAILED:" in command("extensions scenes"), "invalid scene inspection succeeded")
        require(str(package_dir) not in original_scenes and "checker.bmp" not in original_scenes and
                "private-setting-value-not-for-diagnostics" not in original_scenes,
                "scene inspection exposed paths or settings")
        require(command("extensions scenes " + package_id) == original_scenes and
                command("extensions info " + package_id) == original_info,
                "scene inspection changed active counters or registry")
        require("count: integer (set)" in original_info and "optional: string (unset)" in original_info and
                "cooldown=8000 ms" in original_info and "private-setting-value-not-for-diagnostics" not in original_info,
                "setting metadata leaked values or did not apply overrides")

        def apply_settings(values):
            config["Extensions"]["Packages"][0]["Settings"] = values
            config_file.write_text(json.dumps(config), encoding="utf-8")
            require("FAILED:" not in command("reload config"), "config snapshot reload failed")

        require("commands.read" in original_info, "commands.read grant was not loaded")
        script_file.write_text('extensions.commands()\n', encoding="utf-8")
        require("FAILED:" in command("extensions check"), "initialization queried command catalog")
        require(command("extensions info " + package_id) == original_info,
                "failed catalog initialization changed active state")
        script_file.write_text(valid_script, encoding="utf-8")
        config["Extensions"]["Packages"][0]["grants"].remove("commands.read")
        apply_settings(config["Extensions"]["Packages"][0]["Settings"])
        require("FAILED:" in command("extensions check"), "missing requested commands.read grant was accepted")
        require(command("extensions info " + package_id) == original_info, "catalog grant failure changed active instance")
        config["Extensions"]["Packages"][0]["grants"].append("commands.read")
        apply_settings(config["Extensions"]["Packages"][0]["Settings"])

        require("lobby.read" in original_info, "lobby.read grant was not loaded")
        script_file.write_text('extensions.scene_asset_support("checker")\n', encoding="utf-8")
        require("FAILED:" in command("extensions check"), "initialization queried live asset support")
        require(command("extensions info " + package_id) == original_info,
                "failed asset support initialization changed active state")
        script_file.write_text(valid_script, encoding="utf-8")
        script_file.write_text('extensions.lobby()\n', encoding="utf-8")
        require("FAILED:" in command("extensions check"), "initialization queried live lobby state")
        require(command("extensions info " + package_id) == original_info, "failed query initialization changed active state")
        script_file.write_text(valid_script, encoding="utf-8")
        grants = config["Extensions"]["Packages"][0]["grants"]
        grants.remove("lobby.read")
        apply_settings(config["Extensions"]["Packages"][0]["Settings"])
        require("FAILED:" in command("extensions check"), "missing requested lobby.read grant was accepted")
        require(command("extensions info " + package_id) == original_info, "grant failure changed active instance")
        grants.append("lobby.read")
        apply_settings(config["Extensions"]["Packages"][0]["Settings"])
        good_settings = dict(config["Extensions"]["Packages"][0]["Settings"])
        for bad_settings in [dict(good_settings, count=True), dict(good_settings, count=8.0),
                             dict(good_settings, count="8"), dict(good_settings, count=11),
                             dict(good_settings, count=-1), dict(good_settings, unknown=1),
                             dict(good_settings, label=None), dict(good_settings, label="x" * 65),
                             dict(good_settings, label="zero\0byte"), {"count": 8}, [],
                             dict(good_settings, label={}), dict(good_settings, enabled=1)]:
            apply_settings(bad_settings)
            failure = command("extensions check")
            require("FAILED:" in failure and "private-setting-value-not-for-diagnostics" not in failure,
                    "invalid settings accepted or values exposed in check error")
            require("FAILED:" in command("extensions reload"), "invalid settings activated")
            require(command("extensions info " + package_id) == original_info, "invalid settings damaged active runtime")
        apply_settings(good_settings)
        original_schema = json.loads(json.dumps(manifest["settings"]))
        for name, definition in [
                ("count", {"type": "integer", "default": 11, "max": 10}),
                ("count", {"type": "integer", "min": 10, "max": 0}),
                ("count", {"type": "integer", "min": 0.0}),
                ("enabled", {"type": "boolean", "min": 0}),
                ("enabled", {"type": "boolean", "required": 1}),
                ("label", {"type": "string", "max_bytes": 0}),
                ("label", {"type": "string", "max_bytes": 1025}),
                ("label", {"type": "string", "unknown_option": 1}),
                ("optional", {"type": "array"})]:
            manifest["settings"] = dict(original_schema, **{name: definition})
            manifest_file.write_text(json.dumps(manifest), encoding="utf-8")
            require("FAILED:" in command("extensions check"), "invalid settings schema accepted")
            require(command("extensions info " + package_id) == original_info, "invalid schema changed active runtime")
        manifest["settings"] = original_schema
        manifest_file.write_text(json.dumps(manifest), encoding="utf-8")
        apply_settings(dict(good_settings, count=9))
        script_file.write_text(valid_script.replace('== 8)', '== 9)'), encoding="utf-8")
        require("Validation passed:" in command("extensions check"), "new valid settings failed check")
        require(command("extensions info " + package_id) == original_info, "check applied settings to active instance")
        require("FAILED:" not in command("extensions reload"), "new valid settings failed activation")
        require("cooldown=9000 ms" in command("extensions info " + package_id), "reload did not apply new settings")
        apply_settings(good_settings)
        script_file.write_text(valid_script, encoding="utf-8")
        require("FAILED:" not in command("extensions reload"), "settings could not be restored")
        # A fresh instance has different allocator bookkeeping; compare subsequent checks to this snapshot.
        original_info = command("extensions info " + package_id)
        script_file.write_text(f'extensions.command("{package_id}-candidate", {{}}, function() end)\n', encoding="utf-8")
        checked = command("extensions check")
        require("Validation passed: 2 packages" in checked and "Active state unchanged" in checked, "candidate validation failed")
        require(command("extensions info " + package_id) == original_info, "successful check replaced or mutated the active runtime")
        active()
        script_file.write_text(valid_script, encoding="utf-8")
        dependent_script.write_text(f'extensions.command("{package_id}", {{}}, function() end)\n', encoding="utf-8")
        require("FAILED:" in command("extensions check"), "check accepted a cross-package collision")
        require("Last check failure:" in command("extensions list"), "failed check diagnostic was lost")
        require(command("extensions info " + package_id) == original_info, "failed check changed active health/state")
        require("FAILED:" in command("extensions reload"), "cross-package command collision was accepted")
        require("Last reload failure:" in command("extensions list"), "failed reload diagnostic was lost")
        active()
        dependent_script.write_text(f'extensions.command("{dependent_id}", {{arguments = {{{{name="x", type="unknown"}}}}}}, function() end)\n', encoding="utf-8")
        require("FAILED:" in command("extensions reload"), "invalid schema replacement was accepted")
        active()
        dependent_script.write_text(valid_dependent, encoding="utf-8")
        require("Validation passed:" in command("extensions check"), "valid check did not recover after failure")
        status = command("extensions list")
        require("Last check failure:" not in status and "Last reload failure:" in status,
                "check and reload diagnostics were cleared together")
        # Exercise actual package staging of a valid texture above the GC limit.
        large_image = build / "lobby-media-fixtures/pc-noise.gvm.prs"
        require(large_image.is_file(), "run extension-native-assets before this test")
        (package_dir / "pc-noise.gvm.prs").write_bytes(large_image.read_bytes())
        manifest["assets"]["pc-only"] = {"texture": "pc-noise.gvm.prs", "media_profile": "pc-port",
                                            "fallback": {"texture": "checker.bmp"}}
        manifest_file.write_text(json.dumps(manifest), encoding="utf-8")
        require("FAILED:" not in command("extensions reload"), "PC-profile asset could not activate")
        active()
        pc_scenes = command("extensions scenes " + package_id)
        require("Assets: 2; owned objects: 0; event overrides: 0" in pc_scenes and "Asset pc-only" in pc_scenes and
                "compatible original=no pc=yes" in pc_scenes and "Original-client fallback: compressed bytes" in pc_scenes,
                "PC asset/fallback diagnostics were missing")
        require("Validation passed:" in command("extensions check") and
                command("extensions scenes " + package_id) == pc_scenes, "dry check changed scene diagnostics")
        for invalid_fallback in [{"texture": "pc-noise.gvm.prs"},
                                 {"texture": "pc-noise.gvm.prs", "media_profile": "pc-port"},
                                 {"texture": "../outside.bmp"},
                                 {"texture": "checker.bmp", "animation": "checker.bmp"},
                                 {"texture": "checker.bmp", "fallback": {"texture": "checker.bmp"}}]:
            manifest["assets"]["pc-only"]["fallback"] = invalid_fallback
            manifest_file.write_text(json.dumps(manifest), encoding="utf-8")
            require("FAILED:" in command("extensions reload"), "invalid fallback activated")
            active()
            require(command("extensions scenes " + package_id) == pc_scenes, "rejected fallback changed active diagnostics")
        manifest["assets"]["pc-only"]["fallback"] = {"texture": "checker.bmp"}
        del manifest["assets"]["pc-only"]["media_profile"]
        manifest_file.write_text(json.dumps(manifest), encoding="utf-8")
        require("FAILED:" in command("extensions reload"), "default profile accepted oversized media")
        active()
        manifest["assets"]["pc-only"]["media_profile"] = "unknown"
        manifest_file.write_text(json.dumps(manifest), encoding="utf-8")
        require("FAILED:" in command("extensions reload"), "unknown media profile was accepted")
        active()
        del manifest["assets"]["pc-only"]
        manifest_file.write_text(json.dumps(manifest), encoding="utf-8")
        image_file.write_bytes(b"invalid BMP")
        require("FAILED:" in command("extensions reload"), "invalid texture replacement was accepted")
        active()
        image_file.write_bytes(valid_image)
        script_file.write_text('extensions.command("debug", {}, function() end)\n', encoding="utf-8")
        require("FAILED:" in command("extensions reload"), "built-in command collision was accepted")
        active()
        script_file.write_text(valid_script, encoding="utf-8")
        manifest["assets"]["checker"]["texture"] = "../chat-tools/main.lua"
        manifest_file.write_text(json.dumps(manifest), encoding="utf-8")
        require("FAILED:" in command("extensions reload"), "asset path escaped its package")
        active()
        manifest["assets"]["checker"]["texture"] = "checker.bmp"
        manifest_file.write_text(json.dumps(manifest), encoding="utf-8")
        require("FAILED:" not in command("extensions reload"), "valid replacement could not activate")
        require("Last reload failure:" not in command("extensions list"), "successful reload did not clear its diagnostic")
        active()
        require(command("extensions scenes " + package_id) == original_scenes,
                "replacement scene catalog retained removed assets")
        require("FAILED:" in command("extensions disable " + package_id), "dependency could be unloaded before its dependent")
        active()
        command("extensions disable " + dependent_id)
        response = command("extensions list")
        require(package_id + " 0.1.0: enabled" in response and dependent_id + " 0.1.0" not in response,
                "unloading dependent damaged the prerequisite")
        command("extensions disable " + package_id)
        require("FAILED:" in command("extensions scenes " + package_id), "unloaded package retained scene diagnostics")
        require("No extensions loaded" in command("extensions list"), "disable did not unload package")
        require("FAILED:" not in command("extensions reload"), "disabled package could not reload")
        active()
        # Exercise the real manager grant and safe unavailable-service result.
        # No listeners means Main never creates a native GameServer in this test.
        manifest["permissions"].append("chat.announce")
        manifest_file.write_text(json.dumps(manifest), encoding="utf-8")
        require("FAILED:" in command("extensions check"), "ungranted requested announcement authority accepted")
        config["Extensions"]["Packages"][0]["grants"].append("chat.announce")
        apply_settings(good_settings)
        probe_script = valid_script + (
            'extensions.on("package_load", function()\n'
            '  local ok, why = extensions.announce("Headless announcement probe")\n'
            '  extensions.log("announcement-probe:" .. (ok and "accepted" or why))\n'
            '  local again, reason = extensions.announce("Second probe")\n'
            '  assert(not again and reason == "unavailable")\n'
            'end)\n')
        script_file.write_text(probe_script, encoding="utf-8")
        before_announcements = command("extensions info " + package_id)
        checked = command("extensions check")
        require("Validation passed:" in checked and "announcement-probe:" not in checked and
                command("extensions info " + package_id) == before_announcements,
                "dry check executed an announcement callback or changed active state")
        script_file.write_text("extensions.announce('staging')\n" + probe_script, encoding="utf-8")
        require("FAILED:" in command("extensions reload") and
                command("extensions info " + package_id) == before_announcements,
                "staging announced or changed the active instance")
        script_file.write_text(probe_script, encoding="utf-8")
        response = command("extensions reload")
        require("FAILED:" not in response and "announcement-probe:unavailable" in response,
                "announcement grant or safe unavailable-service result failed")
        require("Callbacks: 1; failures: 0" in command("extensions info " + package_id),
                "announcement service refusal faulted package")
        response = command("extensions reload")
        require("FAILED:" not in response and "announcement-probe:unavailable" in response,
                "announcement service refusal changed across reload")
        require("chat.announce" in command("extensions info " + package_id) and
                "Callbacks: 1; failures: 0" in command("extensions info " + package_id),
                "announcement grant or healthy service refusal missing from diagnostics")
        # Preserve the original startup/restart assertions below; new manager
        # startup occurs before GameServer construction and may be unavailable.
        script_file.write_text(valid_script, encoding="utf-8")
        require("FAILED:" not in command("extensions reload"), "announcement probe cleanup reload failed")
        # EOF uses the shell's normal shutdown path without leaving its input open.
        process.stdin.close()
        require(process.wait(timeout=30) == 0, "server did not shut down cleanly")
        reader.join(timeout=5)
        require(not reader.is_alive(), "server output reader did not finish")
        while not output.empty():
            output.get_nowait()
        pending = ""
        process = subprocess.Popen(
            [args.server, "--config=" + str(config_file)], cwd=source,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0)
        reader = threading.Thread(target=read_output, daemon=True)
        reader.start()
        prompt()
        active()
        process.stdin.close()
        require(process.wait(timeout=30) == 0, "restarted server did not shut down cleanly")
        reader.join(timeout=5)
        require(not reader.is_alive(), "restarted output reader did not finish")
        print("Two-package startup/restart, Lua isolation, assets, schema/collision rollback, dependency unload, reload and clean shutdown passed")
    except Exception as error:
        while not output.empty():
            chunk = output.get_nowait()
            if chunk is not None:
                history += chunk
        raise RuntimeError(f"{error}\nLast server output:\n{history[-12000:]}") from error
    finally:
        if process is not None and process.poll() is None:
            process.kill()
            process.wait(timeout=30)
        require(package_dir.resolve().parent == package_root, "cleanup package path changed")
        require(dependent_dir.resolve().parent == package_root, "cleanup dependent path changed")
        require(temporary.resolve().parent == build, "cleanup config path changed")
        for banner in banner_files:
            require(banner.resolve().parent == (source / "system/ep3/banners").resolve(), "cleanup banner path changed")
            banner.unlink()
        shutil.rmtree(package_dir)
        shutil.rmtree(dependent_dir)
        shutil.rmtree(temporary)


if __name__ == "__main__":
    main()
