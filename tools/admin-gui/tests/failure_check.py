"""Test: what the program does when the server cannot start or does not stop. No window is opened.

    python failure_check.py --server-dir DIR --server-exe FILE --extra-path FOLDERS [--old-exe FILE]

1. The DLL folder is left out: the start fails and the reason names the missing DLL (only meaningful for a server
   build that needs DLLs from --extra-path).
2. A second server is started in the same folder while the first runs: the start is refused (ports in use), the
   first keeps running and then stops cleanly.
3. --old-exe (a Teaserv build without --force-shell): it starts, the missing shell is reported, a stop request runs
   into the time limit, and only the explicit forced stop ends it.
"""

import argparse
import json
import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from teaserv_admin import app as app_module  # noqa: E402
from teaserv_admin import server  # noqa: E402
from teaserv_admin.appsettings import AppSettings  # noqa: E402

FAILED = []


def check(name, ok, detail=""):
    print("%s  %s  %s" % ("PASS" if ok else "FAIL", name, detail), flush=True)
    if not ok:
        FAILED.append(name)


def make(args, folder, name, **values):
    path = os.path.join(folder, name + ".json")
    data = {"server_dir": os.path.abspath(args.server_dir), "server_exe": args.server_exe,
            "extra_path": args.extra_path, "stats_poll_seconds": 2}
    data.update(values)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f)
    srv = server.ServerProcess(AppSettings(path))
    events = []
    srv.state_listeners.append(lambda state, detail: events.append((state, detail)))
    return srv, events


def wait(servers, condition, seconds=60):
    end = time.time() + seconds
    while time.time() < end:
        for srv in servers:
            srv.poll()
        if condition():
            return True
        time.sleep(0.1)
    return False


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server-dir", required=True)
    parser.add_argument("--server-exe", required=True)
    parser.add_argument("--extra-path", default="")
    parser.add_argument("--old-exe", default="")
    args = parser.parse_args()
    app_module.prepare_process()

    with tempfile.TemporaryDirectory() as folder:
        if args.extra_path:
            srv, events = make(args, folder, "no-dll", extra_path="")
            srv.start()
            wait([srv], lambda: srv.state == server.FAILED, 30)
            detail = events[-1][1] if events else ""
            check("missing DLL: the start fails with a reason", srv.state == server.FAILED and "DLL" in detail,
                  detail)

        first, _ = make(args, folder, "first")
        first.start()
        check("first server starts", wait([first], lambda: first.state == server.RUNNING and first.shell_ready))
        second, events = make(args, folder, "second")
        reason = second.start()
        check("second server on the same ports: the start is refused with a reason",
              "already in use" in reason and second.proc is None, reason)
        check("the first server still runs", first.state == server.RUNNING and first.proc.poll() is None)
        proc = first.proc
        first.stop()
        wait([first], lambda: first.state == server.STOPPED)
        check("the first server stops cleanly", first.state == server.STOPPED and proc.returncode == 0)

        if args.old_exe:
            old, events = make(args, folder, "old", server_exe=args.old_exe, stop_timeout_seconds=5)
            old.start()
            check("old build: starts", wait([old], lambda: old.state == server.RUNNING))
            wait([old], lambda: old.shell_missing_reported, 15)
            check("old build: the missing command shell is reported", old.shell_missing_reported and
                  not old.shell_ready, [line.text for line in old.lines if "does not read commands" in line.text])
            check("old build: commands are refused with a reason", "does not accept commands" in
                  old.send_command("help"))
            proc = old.proc
            old.stop()
            wait([old], lambda: ("stopping", "timeout") in events, 20)
            check("old build: the stop request runs into the time limit, the process is NOT closed",
                  ("stopping", "timeout") in events and proc.poll() is None)
            old.force_stop()
            wait([old], lambda: old.state == server.STOPPED, 20)
            check("old build: the explicit forced stop ends it", old.state == server.STOPPED and
                  proc.poll() is not None)
        for srv in (first, second):
            srv.close()
    print("failures: %d" % len(FAILED))
    return 1 if FAILED else 0


if __name__ == "__main__":
    sys.exit(main())
