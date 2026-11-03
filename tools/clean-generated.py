#!/usr/bin/env python3
"""Delete generated SunstoneOS files that nothing references, within fixed roots.

Deletes only under the build cache, the trash directory and session scratch
directories. Refuses evidence, sources, host toolchains, checkpoints, inputs,
queue databases and tokens, and any build directory that an evidence record or
board-queue entry names, because runners verify those files by hash.

  clean-generated.py trash                     # empty ~/.cache/sunstoneos/trash
  clean-generated.py build NAME [NAME...]      # named build directories
  clean-generated.py stale-builds DAYS         # unreferenced builds untouched for DAYS
  clean-generated.py path PATH [PATH...]       # files or directories under an allowed root
Refuses anything changed within the last 6 hours unless --recent is given, so a
name pattern cannot remove a directory that a running agent is using.
Add --apply to delete; without it the tool only reports what it would delete."""
import argparse
import json
import os
import re
import shutil
import sqlite3
import sys
import tempfile
import time
from pathlib import Path

CACHE = Path.home() / ".cache/sunstoneos"
BUILD, TRASH, EVIDENCE = CACHE / "build", CACHE / "trash", CACHE / "evidence"
# Agent session scratch directories: claude-UID under the system temporary directory.
ALLOWED = [BUILD, TRASH, Path(tempfile.gettempdir()) / ("claude-%d" % os.getuid())]
PROTECTED = [EVIDENCE, CACHE / "sources", CACHE / "host", CACHE / "checkpoints", CACHE / "inputs",
             CACHE / "venv", CACHE / "bin"]
# Build directories that host tools and runners load by default path.
PINNED_BUILDS = {"sunxi-a733", "a733-ddr", "qemu-virt"}
RECENT_HOURS = 6
RECENT_OK = False
REFERENCE = re.compile(r"\.cache/sunstoneos/build/([^/\"'\s]+)")


def referenced_builds():
    names = set(PINNED_BUILDS)
    for path in EVIDENCE.rglob("*.json"):
        try:
            names.update(REFERENCE.findall(path.read_text(errors="ignore")))
        except OSError:
            pass
    for db in CACHE.glob("*.sqlite3"):
        connection = sqlite3.connect(db.as_uri() + "?mode=ro", uri=True)
        try:
            for row in connection.execute("SELECT submission, review FROM tests"):
                names.update(REFERENCE.findall(" ".join(filter(None, row))))
        finally:
            connection.close()
    return names


def check(path, references):
    real = path.resolve()
    if not any(real == root or real.is_relative_to(root) for root in ALLOWED) or real in ALLOWED:
        raise SystemExit(f"Refused: {path} is not inside an allowed root")
    if any(real == root or real.is_relative_to(root) for root in PROTECTED):
        raise SystemExit(f"Refused: {path} is protected")
    if real.suffix in (".sqlite3", ".token"):
        raise SystemExit(f"Refused: {path} is a queue database or token")
    if real.is_relative_to(BUILD):
        name = real.relative_to(BUILD).parts[0]
        if name in references:
            raise SystemExit(f"Refused: build directory {name} is referenced by evidence or a queue entry")
    if not RECENT_OK and real.exists() and time.time() - newest(real) < RECENT_HOURS * 3600:
        raise SystemExit(f"Refused: {path} changed within the last {RECENT_HOURS} hours and may be in use; pass --recent to override")
    return real


def size(path):
    if path.is_file() or path.is_symlink():
        return path.lstat().st_size
    return sum(p.lstat().st_size for p in path.rglob("*") if p.is_file() and not p.is_symlink())


def newest(path):
    stamps = [path.lstat().st_mtime] + [p.lstat().st_mtime for p in path.rglob("*")]
    return max(stamps)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("mode", choices=("trash", "build", "stale-builds", "path"))
    parser.add_argument("items", nargs="*")
    parser.add_argument("--apply", action="store_true")
    parser.add_argument("--recent", action="store_true", help="allow paths changed within the last 6 hours")
    args = parser.parse_args()
    global RECENT_OK
    RECENT_OK = args.recent
    references = referenced_builds()
    if args.mode == "trash":
        targets = list(TRASH.iterdir()) if TRASH.exists() else []
    elif args.mode == "build":
        targets = [BUILD / name for name in args.items]
    elif args.mode == "path":
        targets = [Path(item) for item in args.items]
    else:
        days = float(args.items[0]) if args.items else 7
        cutoff = time.time() - days * 86400
        targets = [p for p in BUILD.iterdir() if p.name not in references and newest(p) < cutoff]
    if args.mode != "stale-builds":
        kept = []
        for target in targets:
            try:
                kept.append(check(target, references))
            except SystemExit as refusal:
                print(refusal)
        targets = kept
    total = 0
    for target in targets:
        if not target.exists() and not target.is_symlink():
            continue
        bytes_ = size(target)
        total += bytes_
        print(f"{'delete' if args.apply else 'would delete'} {target} ({bytes_ / 2**20:.0f} MiB)")
        if args.apply:
            if target.is_dir() and not target.is_symlink():
                shutil.rmtree(target)
            else:
                target.unlink()
    print(f"{'Deleted' if args.apply else 'Would delete'} {len(targets)} items, {total / 2**30:.1f} GiB"
          f"{'' if args.apply else ' (dry run; add --apply)'}; {len(references)} build directories protected")


if __name__ == "__main__":
    main()
