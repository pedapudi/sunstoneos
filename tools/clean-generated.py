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
  clean-generated.py slim-gates                # build products of the passing steps of gate directories (slim_gate)
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
# What a passing step keeps of its evidence directory: its text records (results, logs, input lists with their hashes, reports).
# Everything else there is a product that the step built from the tree it gated: object files, linked executables and images,
# generated sources, firmware and tensor copies, fixture repositories and build trees. Nothing reads those after the step:
# tools/land.py reads summary.txt and gate-record.json, tools/host_space.py reads disk-peak.json, the evidence records cite
# summary.txt and commands.txt, and the board runner copies each image into its own queue entry.
RECORD_SUFFIXES = frozenset({"", ".json", ".jsonl", ".log", ".txt", ".md", ".csv", ".tsv", ".out", ".err", ".diff", ".patch",
                             ".yaml", ".yml", ".toml"})
STEP_LINE = re.compile(r"^(PASS|WARN|FAIL|SKIP) ([A-Za-z0-9_][A-Za-z0-9_.+-]*)", re.M)
RETENTION_FILE = "retention.json"
# A record over this size at the top of a passing step's directory is kept gzip-compressed (the largest are manifests of 1 to 3 MiB).
COMPRESS_BYTES = 1 << 20


def build_tree(directory, names):
    """A directory that a passing step drops as a whole: a CMake build tree, a Git repository (a fixture clone), or a fixture build
    tree or input copy (build, build-*, inputs)."""
    name = directory.name
    return ("CMakeCache.txt" in names or "CMakeFiles" in names or name.endswith(".git") or
            name in ("build", "inputs") or name.startswith("build-"))


def is_record(path):
    """A file that a passing step keeps: a text file of a record suffix, up to 8 MiB, that is no ELF file or archive."""
    if path.suffix.lower() not in RECORD_SUFFIXES:
        return False
    try:
        if path.lstat().st_size > SLIM_FILE_BYTES:
            return False
        with open(path, "rb") as stream:
            head = stream.read(4096)
    except OSError:
        return True                                    # what cannot be read is kept
    return not head.startswith((b"\x7fELF", b"!<arch>")) and b"\0" not in head


def retention_product(path):
    """A file that slim_directory wrote at the top of a step directory: retention.json, a records archive or a compressed record."""
    name = path.name
    return (name == RETENTION_FILE or re.fullmatch(r"records(-\d+)?\.tar\.gz", name) is not None or
            (name.endswith(".gz") and Path(name[:-3]).suffix.lower() in RECORD_SUFFIXES - {""}))


def linked_elf(path):
    """Whether path is a linked ELF executable or shared object (not an object file), by its header."""
    try:
        with open(path, "rb") as stream:
            head = stream.read(18)
    except OSError:
        return False
    if len(head) < 18 or not head.startswith(b"\x7fELF"):
        return False
    return int.from_bytes(head[16:18], "little" if head[5] == 1 else "big") in (2, 3)          # ET_EXEC, ET_DYN


def sha256(path):
    import hashlib
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def slim_directory(directory, apply=True):
    """Reduce a passing step's evidence directory to its records.

    Removes the build products (everything that is not a record, is_record, and the build trees, build_tree), keeps the records at
    the top of the directory as plain files (gzip-compressed in place, NAME.gz, when over COMPRESS_BYTES), and packs the records and
    symbolic links of its subdirectories into one records.tar.gz there. retention.json records what was removed: the count and
    bytes and, for every linked ELF, its size and SHA-256, so that a verdict still names the executables it covered.
    Returns the bytes on disk freed (or that would be)."""
    import gzip
    import tarfile
    doomed_trees, doomed_files, packed, compressed = [], [], [], []
    for root, dirs, names in os.walk(directory):
        here = Path(root)
        if here != directory and build_tree(here, dirs + names):
            doomed_trees.append(here)
            dirs.clear()
            continue
        for name in sorted(names):
            path = here / name
            if here == directory and retention_product(path):
                continue
            if path.is_symlink() or is_record(path):
                if here != directory:
                    packed.append(path)
                elif not path.is_symlink() and path.lstat().st_size > COMPRESS_BYTES:
                    compressed.append(path)
            else:
                doomed_files.append(path)
    tree_files = [Path(root) / name for tree in doomed_trees for root, _, names in os.walk(tree) for name in names]
    linked = {str(path.relative_to(directory)): {"bytes": path.lstat().st_size, "sha256": sha256(path)}
              for path in [*doomed_files, *tree_files] if not path.is_symlink() and linked_elf(path)}
    blocks = lambda paths: sum(p.lstat().st_blocks * 512 for p in paths)
    removed_bytes, removed_files = blocks(doomed_files) + blocks(tree_files), len(doomed_files) + len(tree_files)
    freed = removed_bytes + blocks(packed) + blocks(compressed)
    if not apply or not (doomed_files or doomed_trees or packed or compressed):
        return freed if not apply else 0
    for path in [*doomed_trees, *doomed_files]:
        try:
            remove(path)
        except OSError:
            pass
    archive = None
    if packed:
        number, archive = 1, directory / "records.tar.gz"
        while archive.exists():
            number += 1
            archive = directory / ("records-%d.tar.gz" % number)
        with tarfile.open(archive, "w:gz", compresslevel=6) as stream:
            for path in packed:
                stream.add(path, arcname=str(path.relative_to(directory)), recursive=False)
        freed -= blocks([archive])
        for path in packed:
            try:
                remove(path)
            except OSError:
                pass
    for path in compressed:
        target = path.with_name(path.name + ".gz")
        with open(path, "rb") as source, gzip.open(target, "wb", compresslevel=6) as sink:
            shutil.copyfileobj(source, sink)
        shutil.copystat(path, target)
        freed -= blocks([target])
        remove(path)
    for root, dirs, names in os.walk(directory, topdown=False):
        if Path(root) != directory and not os.listdir(root):
            try:
                os.chmod(Path(root).parent, os.stat(Path(root).parent).st_mode | 0o300)
                os.rmdir(root)
                freed += 4096
            except OSError:
                pass
    previous = {}
    try:
        previous = json.loads((directory / RETENTION_FILE).read_text())
    except (OSError, ValueError):
        pass
    record = {"removed_files": removed_files + previous.get("removed_files", 0),
              "removed_bytes": removed_bytes + previous.get("removed_bytes", 0),
              "linked_elf": {**previous.get("linked_elf", {}), **linked},
              "archives": sorted({*previous.get("archives", []), *([archive.name] if archive else [])}),
              "compressed": sorted({*previous.get("compressed", []), *(p.name for p in compressed)})}
    (directory / RETENTION_FILE).write_text(json.dumps(record, indent=1, sort_keys=True) + "\n")
    return freed


def step_directory(gate, step):
    """The evidence directory that a step of tools/integration-gate.py writes: image-NAME evidence/images/NAME,
    indexed-NAME evidence/indexed/NAME and check-NAME evidence/NAME; None for a step that writes none there (a build, a session)."""
    for prefix, parent in (("image-", "evidence/images"), ("indexed-", "evidence/indexed"), ("check-", "evidence")):
        if step.startswith(prefix) and len(step) > len(prefix):
            return gate / parent / step[len(prefix):]
    return None


def step_verdicts(gate):
    """{step: verdict} from a gate's summary.txt; a step recorded twice keeps its worst verdict."""
    summary = gate / "summary.txt"
    text = summary.read_text(errors="replace") if summary.is_file() else ""
    verdicts = {}
    for verdict, step in STEP_LINE.findall(text):
        if verdicts.get(step) not in ("FAIL", "SKIP"):
            verdicts[step] = verdict
    return verdicts


def slim_gate(gate, references, apply=True, verdicts=None, superseded=False):
    """Slim the evidence directory of every step of a gate that passed (PASS or WARN); a failed or skipped step keeps everything,
    and so does any evidence directory that no step names. A superseded gate (a later gate of its landing label passed) slims
    its whole evidence directory. verdicts defaults to the gate's summary.txt. Each directory obeys the rules of check().
    Returns the bytes freed (or that would be)."""
    if superseded:
        evidence = gate / "evidence"
        tops = [d for d in sorted(evidence.iterdir()) if d.is_dir() and not d.is_symlink()] if evidence.is_dir() else []
        targets = [d for top in tops for d in ([s for s in sorted(top.iterdir()) if s.is_dir() and not s.is_symlink()]
                                              if top.name in ("images", "indexed") else [top])]
    else:
        verdicts = step_verdicts(gate) if verdicts is None else verdicts
        targets = [d for step, verdict in sorted(verdicts.items()) if verdict in ("PASS", "WARN")
                   for d in [step_directory(gate, step)] if d is not None and d.is_dir() and not d.is_symlink()]
    total = 0
    for path in targets:
        try:
            check(path, references)
        except SystemExit:
            continue
        total += slim_directory(path, apply)
    return total


def slim_after_gate(output, label, verdicts, passed):
    """What a gate does at its end: slim its own passing steps and, when the whole gate passed, the earlier gates of the same
    landing label, which may have failed and been kept for diagnosis."""
    global RECENT_OK
    RECENT_OK = True
    references = referenced_builds()
    total = slim_gate(output, references, verdicts=verdicts)
    if passed:
        base = re.sub(r"-\d{4}-\d{6}(-\d+)?$", "", label)
        total += sum(slim_gate(d, references, superseded=True) for d in output.parent.glob("gate-*") if d != output and d.is_dir() and
                     re.fullmatch("gate-" + re.escape(base) + r"-\d{4}-\d{6}(-\d+)?", d.name))
    return total


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
        gates = [g for g in sorted(BUILD.glob("gate-*")) if g.is_dir() and (g / "summary.txt").is_file()]
        total = sum(slim_gate(g, references, args.apply) for g in gates)
        print(f"{'Freed' if args.apply else 'Would free'} {total / 2**30:.1f} GiB in the passing steps of {len(gates)} gate directories"
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
