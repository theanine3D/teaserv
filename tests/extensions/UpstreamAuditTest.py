"""Exercise offline upstream inspection in disposable Git repositories only."""

import argparse
import json
import os
from pathlib import Path
import runpy
import subprocess
import tempfile
import unittest


parser = argparse.ArgumentParser()
parser.add_argument("--git", required=True)
parser.add_argument("--build", type=Path, required=True)
args, remaining = parser.parse_known_args()
ROOT = Path(__file__).resolve().parents[2]
api = runpy.run_path(str(ROOT / "tools/extensions/AuditUpstream.py"))


class UpstreamAuditTests(unittest.TestCase):
    def setUp(self):
        build = args.build.resolve()
        self.temporary = tempfile.TemporaryDirectory(prefix="upstream-audit-", dir=build)
        self.root = Path(self.temporary.name).resolve()
        self.assertEqual(self.root.parent, build)
        self.addCleanup(self.temporary.cleanup)
        self.repo = self.root / "repo with spaces"
        self.repo.mkdir()
        self.git("init", "--quiet")
        self.write("src/Client.hh", "baseline\n")
        self.write("src/ReceiveCommands.cc", "login baseline\n")
        self.write("untouched.txt", "original\n")
        self.base = self.commit("baseline")
        self.watch = {"baseline": self.base, "surfaces": {
            "src/Client.hh": "identity and phases", "src/ReceiveCommands.cc": "login and hooks",
            "src/Extensions/": "runtime contracts"}}

    def git(self, *arguments):
        result = subprocess.run(
            [args.git, "-C", str(self.repo), "-c", "user.name=Fixture",
             "-c", "user.email=fixture@example.invalid", "-c", "commit.gpgsign=false",
             "-c", "core.hooksPath=" + str(self.root / "no-hooks"), *arguments],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True, timeout=30)
        return result.stdout.decode("utf-8", "surrogateescape").strip()

    def write(self, path, text):
        target = self.repo / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding="utf-8")

    def commit(self, message):
        self.git("add", "--all")
        self.git("commit", "--quiet", "-m", message)
        return self.git("rev-parse", "HEAD")

    def audit(self, candidate=None):
        return api["audit"](self.repo, args.git, self.watch, candidate or self.base)

    def test_unchanged_is_not_compatibility_claim(self):
        result = self.audit()
        self.assertFalse(result["review_required"])
        self.assertFalse(result["working_tree_dirty"])
        self.assertEqual(result["watched_changes"], [])
        self.assertIn("not a compatibility verdict", api["markdown"](result))

    def test_upstream_watched_unwatched_deleted_and_moved_paths(self):
        self.write("src/Client.hh", "new phase\n")
        (self.repo / "src/ReceiveCommands.cc").rename(self.repo / "src/MovedLogin.cc")
        self.write("another file.txt", "new\n")
        candidate = self.commit("candidate")
        self.git("checkout", "--quiet", "--detach", self.base)
        result = self.audit(candidate)
        self.assertEqual(result["upstream_changed_path_count"], 4)
        self.assertEqual({item["path"]: item["status"] for item in result["watched_changes"]},
                         {"src/Client.hh": "M", "src/ReceiveCommands.cc": "D"})
        self.assertEqual({item["path"] for item in result["other_upstream_changes"]},
                         {"src/MovedLogin.cc", "another file.txt"})
        self.assertEqual(result["same_path_changes"], [])
        self.assertTrue(result["review_required"])

    def test_committed_staged_unstaged_and_untracked_fork_overlap(self):
        self.write("src/Client.hh", "upstream\n")
        self.write("src/Extensions/New.cc", "upstream extension\n")
        candidate = self.commit("upstream candidate")
        self.git("checkout", "--quiet", "--detach", self.base)
        self.write("src/Client.hh", "fork committed\n")
        head = self.commit("fork fixture")
        self.write("src/ReceiveCommands.cc", "staged\n")
        self.git("add", "src/ReceiveCommands.cc")
        self.write("untouched.txt", "unstaged\n")
        self.write("src/Extensions/New.cc", "untracked\n")
        result = self.audit(candidate)
        self.assertEqual(result["fork_head"], head)
        self.assertEqual(result["working_tree_changed_path_count"], 3)
        self.assertEqual(result["local_watched_paths"], ["src/Extensions/New.cc", "src/ReceiveCommands.cc"])
        self.assertEqual(result["same_path_changes"], ["src/Client.hh", "src/Extensions/New.cc"])
        self.assertTrue(all(item["also_changed_in_fork"] for item in result["watched_changes"]))

    def test_read_only_index_refs_files_and_repeated_output(self):
        self.write("src/Client.hh", "staged\n")
        self.git("add", "src/Client.hh")
        self.write("src/Client.hh", "unstaged\n")
        self.write("src/Extensions/Private.cc", "untracked\n")
        # Ignored data and its contents must not enter the report.
        self.write(".git/info/exclude", "private-data/\n")
        self.write("private-data/account.json", "secret contents\n")

        def snapshot():
            return {str(path.relative_to(self.repo)): path.read_bytes()
                    for path in self.repo.rglob("*") if path.is_file()}

        before = snapshot()
        first = self.audit()
        self.assertEqual(first, self.audit())
        self.assertEqual(before, snapshot())
        self.assertNotIn("secret", json.dumps(first))
        self.assertNotIn("private-data", json.dumps(first))

    def test_bad_revisions_and_non_descendant_are_errors(self):
        for revision in ("--help", "", "absent-ref"):
            with self.assertRaises(api["AuditError"]):
                api["resolve"](self.repo, args.git, revision)
        # A later baseline cannot be used to inspect an older candidate.
        self.write("untouched.txt", "later\n")
        self.watch["baseline"] = self.commit("later")
        with self.assertRaisesRegex(api["AuditError"], "not an ancestor"):
            self.audit(self.base)

    def test_explicit_baseline_and_local_rename(self):
        self.write("new file.txt", "new\n")
        candidate = self.commit("later")
        self.git("checkout", "--quiet", "--detach", self.base)
        (self.repo / "src/ReceiveCommands.cc").rename(self.repo / "src/NewLogin.cc")
        self.git("add", "--all")
        result = api["audit"](self.repo, args.git, self.watch, candidate, "HEAD")
        self.assertEqual(result["baseline"], self.base)
        self.assertEqual(result["working_tree_changed_path_count"], 2)
        self.assertEqual(result["local_watched_paths"], ["src/ReceiveCommands.cc"])
        self.watch["baseline"] = candidate
        with self.assertRaisesRegex(api["AuditError"], "fork HEAD"):
            self.audit(candidate)

    def test_cli_json_markdown_and_failure(self):
        watch = self.root / "watch.json"
        watch.write_text(json.dumps(dict(self.watch, schema_version=1)), encoding="utf-8")
        command = [os.sys.executable, str(ROOT / "tools/extensions/AuditUpstream.py"),
                   "--repository", str(self.repo), "--git", args.git, "--watch", str(watch)]
        result = subprocess.run(command + ["--candidate", self.base, "--format", "json"],
                                capture_output=True, check=True, timeout=30)
        self.assertEqual(json.loads(result.stdout)["baseline"], self.base)
        result = subprocess.run(command + ["--candidate", self.base], capture_output=True, check=True, timeout=30)
        self.assertIn(b"# Teaserv upstream review", result.stdout)
        result = subprocess.run(command + ["--candidate", "missing"], capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 2)
        self.assertEqual(result.stdout, b"")
        self.assertIn(b"Upstream audit failed", result.stderr)

    def test_watch_validation_and_shipped_inventory(self):
        shipped = api["load_watch"](ROOT / "docs/EXTENSIONS-UPSTREAM-WATCH.json")
        self.assertIn("src/ReceiveCommands.cc", shipped["surfaces"])
        self.assertIn("src/Channel.cc", shipped["surfaces"])
        watch = self.root / "invalid.json"
        for value in ([], None, {}, {"schema_version": True, "baseline": self.base},
                      {"schema_version": 2, "baseline": self.base},
                      {"schema_version": 1, "baseline": self.base, "surfaces": {}},
                      {"schema_version": 1, "baseline": self.base, "surfaces": {"src/Client.hh": 7}},
                      {"schema_version": 1, "baseline": self.base, "surfaces": {"../outside": "bad"}}):
            watch.write_text(json.dumps(value), encoding="utf-8")
            with self.assertRaises(api["AuditError"]):
                api["load_watch"](watch)


if __name__ == "__main__":
    unittest.main(argv=[os.sys.argv[0], *remaining])
