# Reviewing upstream updates

`tools/extensions/AuditUpstream.py` produces a local, read-only review report before an upstream integration. It compares the recorded upstream baseline with a candidate commit already available in this checkout. It does not fetch, merge, switch branches, modify the index or working files, run tests, or contact GitHub. The repository owner continues to perform commits, pushes, and pull requests manually.

After the owner has made an upstream candidate available locally, run from the Teaserv root:

```powershell
python tools/extensions/AuditUpstream.py --candidate upstream/master
```

Use `--git 'C:\Program Files\Git\cmd\git.exe'` if Git is not on PATH. `--repository` selects another checkout; the default is this script's checkout. `--format json` produces a machine-readable report. Output goes to stdout; redirect it to a new file under ignored `build/` when retaining evidence. The tool does not create an output file itself.

For a local smoke check without an available newer upstream ref, use the recorded baseline as `--candidate`. This should report zero upstream changes while still describing local watched edits. It does not demonstrate that a real upstream update has integrated successfully.

The [watch inventory](EXTENSIONS-UPSTREAM-WATCH.json) stores the shared upstream baseline and review reasons for exact file paths or directory prefixes ending in `/`. It includes extension integration hooks and related framing, client/lobby lifecycle, codecs, and quest services. The [integration map](EXTENSIONS-INTEGRATION.md) explains the invariants to inspect. Changes elsewhere upstream remain in the report because they can affect these services indirectly.

The report includes:

- Resolved baseline, candidate, and fork HEAD commit IDs. Both candidate and fork HEAD must descend from the chosen baseline; missing or unrelated revisions fail clearly.
- Every upstream changed path, separated into watched and other changes, with modification/addition/deletion status. Moves appear as deletion plus addition so an old hook path stays visible.
- Paths changed both upstream and in the fork, using the baseline-to-HEAD difference plus staged, unstaged, and untracked local paths. This is a file-level overlap hint, not a prediction of merge conflicts. Ignored files are excluded.
- Local working-tree change count and watched local paths. Script, account, configuration, and storage contents are never copied into the report.

Exit zero means inspection completed; it does not mean an update is compatible. Any upstream changes set JSON `review_required` to true, including changes outside the watch list. Errors return 2 without a partial report. Git inspection commands have bounded subprocess timeouts and disable optional index locks and filesystem-monitor hooks. Filenames are JSON escaped for report display.

`--baseline <local-ref-or-commit>` overrides the recorded baseline for a particular review. `--watch <json-file>` selects a different inventory. These switches do not update the inventory. Keep the baseline at the last successfully integrated upstream revision, rather than advancing it to a candidate merely because inspection completed. After a validated integration, update the inventory and integration map baseline together and retain the candidate hashes and test evidence with release notes.

Review the entire upstream diff and changed callees, use a separate integration branch, then build and test both `NEWSERV_ENABLE_EXTENSIONS=ON` and `OFF`. Run the affected authenticated-client and media acceptance checks before treating the integration as validated. No path inventory can prove packet compatibility, rendering, event ordering, or safe gameplay mutation.

The automated audit suite uses disposable repositories inside the build directory, with synthetic history confined to those fixtures. It checks changed/deleted/moved files, directory watches, committed and uncommitted overlap, baseline overrides, invalid revisions, ancestry, CLI output/errors, and byte-for-byte preservation of refs, index, and files during inspection. It does not create commits in the Teaserv checkout or exercise a real upstream merge.
