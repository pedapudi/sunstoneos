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
  clean-generated.py slim-gates                # bulky outputs of passing gate directories (slim_gate)
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


DECLARED = (re.compile(r"""(?<![\w.-])build/([A-Za-z0-9][\w.-]*)"""), re.compile(r"""['"]build['"]\s*/\s*['"]([A-Za-z0-9][\w.-]*)['"]"""))


def declared_inputs():
    """Build directories that checks and tools name as inputs or defaults, as `.../build/NAME` or `'build' / 'NAME'` in their sources.

    Derived from the repository this tool belongs to, so a new default input is protected without a list to keep."""
    root = Path(__file__).resolve().parents[1]
    names = set()
    for path in [*(root / "checks").glob("*.py"), *(root / "tools").glob("*.py"), *(root / "tools").glob("*.json")]:
        text = path.read_text(errors="ignore")
        for pattern in DECLARED:
            names.update(pattern.findall(text))
    return names


def referenced_builds():
    names = set(PINNED_BUILDS) | declared_inputs()
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


def remove(path):
    """Delete a file, link or directory tree. A read-only directory (a fixture copied with mode r-x), the one holding the path
    included, gets owner write permission first, so that its contents can go."""
    os.chmod(path.parent, os.stat(path.parent).st_mode | 0o300)      # removing an entry needs write on its directory
    if path.is_dir() and not path.is_symlink():
        for root, dirs, _ in os.walk(path):
            for name in [root, *[os.path.join(root, d) for d in dirs]]:
                if not os.path.islink(name):
                    os.chmod(name, os.stat(name).st_mode | 0o700)
        shutil.rmtree(path)
    else:
        path.unlink()


def size(path):
    if path.is_file() or path.is_symlink():
        return path.lstat().st_size
    return sum(p.lstat().st_size for p in path.rglob("*") if p.is_file() and not p.is_symlink())


def newest(path):
    stamps = [path.lstat().st_mtime] + [p.lstat().st_mtime for p in path.rglob("*")]
    return max(stamps)


SLIM_FILE_BYTES = 8 << 20


def slim_candidates(gate):
    """A gate's bulky outputs: fixture build trees and input copies, and files over 8 MiB, under its evidence directory."""
    found = []
    for root, dirs, files in os.walk(gate / "evidence"):
        for name in list(dirs):
            if name == "build" or name.startswith("build-") or name == "inputs":
                found.append(Path(root) / name)
                dirs.remove(name)
        found += [Path(root) / f for f in files if (Path(root) / f).lstat().st_size > SLIM_FILE_BYTES]
    return found


def gate_passed(gate):
    summary = gate / "summary.txt"
    lines = summary.read_text(errors="replace").splitlines() if summary.is_file() else []
    return any("PASS selected" in line for line in lines) and not any(re.search(r"\bFAIL\b", line) for line in lines)


def slim_gate(gate, references, apply=True):
    """Delete a gate directory's bulky outputs by the rules of check(); the summary, timing, commands, gate-record.json, disk-peak.json,
    step logs and every other file stay. Returns the bytes freed (or that would be)."""
    total = 0
    for path in slim_candidates(gate):
        try:
            check(path, references)
        except SystemExit:
            continue
        total += size(path)
        if apply:
            try:
                remove(path)
            except OSError:
                pass
    return total


def slim_after_pass(output, label):
    """What a passing gate does at its end: slim its own directory and the earlier gates of the same landing label, which
    may have failed and been kept for diagnosis."""
    global RECENT_OK
    RECENT_OK = True
    references = referenced_builds()
    base = re.sub(r"-\d{4}-\d{6}(-\d+)?$", "", label)
    older = [d for d in output.parent.glob("gate-*") if d != output and d.is_dir() and
             re.fullmatch("gate-" + re.escape(base) + r"-\d{4}-\d{6}(-\d+)?", d.name)]
    return sum(slim_gate(d, references) for d in [output, *older])


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("mode", choices=("trash", "build", "stale-builds", "path", "slim-gates"))
    parser.add_argument("items", nargs="*")
    parser.add_argument("--apply", action="store_true")
    parser.add_argument("--recent", action="store_true", help="allow paths changed within the last 6 hours")
    args = parser.parse_args()
    global RECENT_OK
    RECENT_OK = args.recent
    references = referenced_builds()
    if args.mode == "slim-gates":
        gates = [g for g in sorted(BUILD.glob("gate-*")) if g.is_dir() and gate_passed(g)]
        total = sum(slim_gate(g, references, args.apply) for g in gates)
        print(f"{'Freed' if args.apply else 'Would free'} {total / 2**30:.1f} GiB in {len(gates)} passing gate directories"
              f"{'' if args.apply else ' (dry run; add --apply)'}")
        return
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
            try:
                remove(target)
            except OSError as error:
                print(f"failed to delete {target}: {error}; carrying on")
    print(f"{'Deleted' if args.apply else 'Would delete'} {len(targets)} items, {total / 2**30:.1f} GiB"
          f"{'' if args.apply else ' (dry run; add --apply)'}; {len(references)} build directories protected")


if __name__ == "__main__":
    main()
