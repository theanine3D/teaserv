"""Require identical packet replay results from LF and CRLF inputs."""

import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--source", required=True)
    parser.add_argument("--build", required=True)
    args = parser.parse_args()
    source = Path(args.source).resolve()
    build = Path(args.build).resolve()
    data = (source / "tests/GC-XB-EnemyDamageSyncSwitch.test.txt").read_bytes().replace(b"\r\n", b"\n")
    # Both files exercise command shortcuts as well as expected packet bytes.
    # TemporaryDirectory only removes its own generated files under build/.
    with tempfile.TemporaryDirectory(prefix="replay-endings-", dir=build) as directory:
        root = Path(directory).resolve()
        if root.parent != build:
            raise RuntimeError("unexpected replay test directory")
        paths = [root / "lf.test.txt", root / "crlf.test.txt"]
        paths[0].write_bytes(data)
        paths[1].write_bytes(data.replace(b"\n", b"\r\n"))
        result = subprocess.run(
            [args.server, "--parallel", "--config=" + str(source / "tests/config.json"),
             *["--replay-log=" + str(path) for path in paths]],
            cwd=source, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
        output = result.stdout.decode("utf-8", errors="replace")
        # Parallel replay can discard initialization exceptions; exit zero alone
        # does not prove either case was actually processed.
        complete = "[Replay] All replays complete; exiting" in output
        passed = all(str(path) + " OK" in output for path in paths)
        if result.returncode != 0 or not complete or not passed:
            raise RuntimeError("LF/CRLF replay failed:\n" + output[-16000:])
    print("LF and CRLF replay inputs produced the expected packets")


if __name__ == "__main__":
    main()
