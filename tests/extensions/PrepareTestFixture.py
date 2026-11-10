"""Copy tracked test data and materialize Git symlinks without changing a checkout."""

import argparse
from pathlib import Path
import shutil
import subprocess


def prepare(source, destination, git):
    source = source.absolute().resolve()
    destination = destination.absolute().resolve()
    build = (source / "build").resolve()
    if destination == build or not destination.is_relative_to(build):
        raise RuntimeError("fixture destination must be a new directory inside source/build/")
    if destination.exists():
        raise RuntimeError("fixture destination already exists; choose a new directory")
    listing = subprocess.check_output(
        [git, "ls-files", "--stage", "-z", "--", "system", "tests"], cwd=source)
    files = {}
    for entry in listing.split(b"\0"):
        if not entry:
            continue
        metadata, relative = entry.decode("utf-8").split("\t", 1)
        mode, _, stage = metadata.split()
        if stage != "0" or mode not in {"100644", "100755", "120000"}:
            raise RuntimeError("unsupported or unresolved Git entry: " + relative)
        files[relative] = mode

    def resolve(relative, seen):
        path = source / relative
        if relative in seen:
            raise RuntimeError("cyclic fixture symlink: " + relative)
        if files.get(relative) == "120000":
            target = path.readlink() if path.is_symlink() else Path(path.read_text(encoding="utf-8"))
            resolved = (path.parent / target).resolve()
            if not resolved.is_relative_to(source):
                raise RuntimeError("fixture symlink leaves checkout: " + relative)
            return resolve(resolved.relative_to(source).as_posix(), seen | {relative})
        resolved = path.resolve()
        if not resolved.is_relative_to(source) or not resolved.is_file():
            raise RuntimeError("missing or external fixture source: " + relative)
        return resolved

    # Validate every input before creating the output. Only tracked data is
    # copied, keeping ignored player/account/configuration files out of fixtures.
    resolved = {name: resolve(name, set()) for name in files}
    destination.mkdir(parents=True)
    for name, path in resolved.items():
        output = destination / name
        output.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, output)
    # Samples can be uncommitted during development. The smoke test uses this
    # original checker image; package scripts under test always come from source.
    name = "system/extensions/lobby-decor/assets/checker.bmp"
    image = source / name
    if image.is_file():
        if not image.resolve().is_relative_to(source):
            raise RuntimeError("example image leaves checkout")
        output = destination / name
        output.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(image, output)
    print(f"Prepared {len(files)} tracked files, including {sum(m == '120000' for m in files.values())} materialized links: {destination}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", default=".")
    parser.add_argument("--destination", required=True)
    parser.add_argument("--git", default="git")
    args = parser.parse_args()
    prepare(Path(args.source), Path(args.destination), args.git)


if __name__ == "__main__":
    main()
