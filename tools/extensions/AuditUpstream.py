"""Read-only, offline review aid for a prospective Teaserv upstream update."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys


DEFAULT_WATCH = Path(__file__).resolve().parents[2] / "docs/EXTENSIONS-UPSTREAM-WATCH.json"


class AuditError(ValueError):
    pass


def git(repository, executable, *arguments, allowed=(0,)):
    environment = dict(os.environ, GIT_OPTIONAL_LOCKS="0", GIT_TERMINAL_PROMPT="0")
    result = subprocess.run(
        [executable, "--no-optional-locks", "-c", "core.fsmonitor=false",
         "-C", str(repository), *arguments],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=environment, timeout=30)
    if result.returncode not in allowed:
        raise AuditError("Git inspection failed: " + result.stderr.decode("utf-8", "replace").strip())
    return result


def resolve(repository, executable, revision):
    if not revision or revision.startswith("-"):
        raise AuditError("revision must be a nonempty local commit or ref, not an option")
    return git(repository, executable, "rev-parse", "--verify", "--end-of-options",
               revision + "^{commit}").stdout.decode("ascii").strip()


def changes(repository, executable, before, after):
    # No rename heuristic: a moved hook must flag both its old and new paths.
    parts = git(repository, executable, "diff", "--no-ext-diff", "--no-textconv",
                "--no-renames", "--name-status", "-z", before, after, "--").stdout.split(b"\0")
    return [{"status": parts[index].decode("ascii"),
             "path": parts[index + 1].decode("utf-8", "surrogateescape")}
            for index in range(0, len(parts) - 1, 2)]


def load_watch(path):
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    if (not isinstance(data, dict) or type(data.get("schema_version")) is not int
            or data["schema_version"] != 1 or not isinstance(data.get("baseline"), str)
            or not data["baseline"]):
        raise AuditError("invalid watch schema or baseline")
    entries = data.get("surfaces")
    if not isinstance(entries, dict) or not entries:
        raise AuditError("watch surfaces must be a nonempty path-to-reason object")
    for name, reason in entries.items():
        if (not name or name.startswith(("/", "-")) or "\\" in name or ":" in name
                or any(part in (".", "..", "") for part in name.rstrip("/").split("/"))
                or not isinstance(reason, str) or not reason.strip()):
            raise AuditError("invalid watch path or reason")
    return data


def audit(repository, executable, watch, candidate, baseline=None):
    root = Path(git(repository, executable, "rev-parse", "--show-toplevel").stdout.decode().strip()).resolve()
    base = resolve(root, executable, baseline or watch["baseline"])
    target = resolve(root, executable, candidate)
    head = resolve(root, executable, "HEAD")
    ancestry = git(root, executable, "merge-base", "--is-ancestor", base, target, allowed=(0, 1))
    if ancestry.returncode:
        raise AuditError("baseline is not an ancestor of the candidate; select the actual upstream baseline")
    if git(root, executable, "merge-base", "--is-ancestor", base, head, allowed=(0, 1)).returncode:
        raise AuditError("baseline is not an ancestor of fork HEAD; select the actual shared upstream baseline")
    upstream = changes(root, executable, base, target)
    fork_paths = {item["path"] for item in changes(root, executable, base, head)}
    status = git(root, executable, "-c", "status.renames=false", "status", "--porcelain=v1",
                 "-z", "--untracked-files=all").stdout.split(b"\0")
    local_paths = {item[3:].decode("utf-8", "surrogateescape") for item in status if item}
    fork_paths.update(local_paths)

    def reasons(path):
        return [reason for surface, reason in watch["surfaces"].items()
                if path == surface or (surface.endswith("/") and path.startswith(surface))]

    reviewed = []
    other = []
    for item in upstream:
        matching = reasons(item["path"])
        entry = dict(item, also_changed_in_fork=item["path"] in fork_paths)
        (reviewed if matching else other).append(dict(entry, review=matching))
    return {
        "schema_version": 1,
        "baseline": base, "candidate": target, "fork_head": head,
        "working_tree_dirty": bool(local_paths),
        "working_tree_changed_path_count": len(local_paths),
        "local_watched_paths": sorted(path for path in local_paths if reasons(path)),
        "upstream_changed_path_count": len(upstream),
        "review_required": bool(upstream),
        "watched_changes": reviewed, "other_upstream_changes": other,
        "same_path_changes": sorted(item["path"] for item in upstream if item["path"] in fork_paths),
        "limitations": [
            "Path overlap is a review hint, not a textual merge-conflict prediction.",
            "Unwatched files and changed callees can affect behavior; review the complete upstream diff.",
            "This report does not merge, build, run tests, prove compatibility, or close live-client gates.",
            "Only locally available Git objects are inspected; no fetch or other network operation occurs.",
        ],
    }


def markdown(report):
    lines = ["# Teaserv upstream review", "", "Baseline: `" + report["baseline"] + "`",
             "Candidate: `" + report["candidate"] + "`", "Fork HEAD: `" + report["fork_head"] + "`", "",
             "Working tree changed paths: " + str(report["working_tree_changed_path_count"]),
             "Upstream changed paths: " + str(report["upstream_changed_path_count"]), "",
             "## Watched integration surfaces", ""]
    # JSON quoting escapes control characters and keeps unusual filenames on one line.
    quote = lambda value: json.dumps(value, ensure_ascii=True)
    for item in report["watched_changes"]:
        overlap = " (also changed in fork)" if item["also_changed_in_fork"] else ""
        lines.append("- " + item["status"] + " " + quote(item["path"]) + overlap + ": " + "; ".join(item["review"]))
    if not report["watched_changes"]:
        lines.append("No watched paths changed. This is not a compatibility verdict.")
    lines.extend(["", "## Other upstream changes", ""])
    lines.extend("- " + item["status"] + " " + quote(item["path"])
                 + (" (also changed in fork)" if item["also_changed_in_fork"] else "")
                 for item in report["other_upstream_changes"])
    if not report["other_upstream_changes"]:
        lines.append("None.")
    lines.extend(["", "## Local uncommitted watched paths", ""])
    lines.extend("- " + quote(path) for path in report["local_watched_paths"])
    if not report["local_watched_paths"]:
        lines.append("None.")
    lines.extend(["", "## Required follow-up", "",
                  "Review the full diff and integration map; merge on a separate integration branch,",
                  "then build/test extensions ON and OFF and run affected real-client acceptance checks.", ""])
    lines.extend("- " + item for item in report["limitations"])
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", type=Path, default=DEFAULT_WATCH.parents[1])
    parser.add_argument("--git", default="git")
    parser.add_argument("--watch", type=Path, default=DEFAULT_WATCH)
    parser.add_argument("--baseline", help="override recorded upstream baseline with a local revision")
    parser.add_argument("--candidate", required=True, help="already available upstream commit or ref")
    parser.add_argument("--format", choices=("markdown", "json"), default="markdown")
    args = parser.parse_args()
    try:
        report = audit(args.repository, args.git, load_watch(args.watch), args.candidate, args.baseline)
        print(json.dumps(report, indent=2, ensure_ascii=True) if args.format == "json" else markdown(report), end="\n")
    except (AuditError, OSError, ValueError, subprocess.TimeoutExpired) as error:
        print("Upstream audit failed: " + str(error), file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
