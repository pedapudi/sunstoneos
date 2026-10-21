#!/usr/bin/env python3
"""Fetch pinned source archives into the local development cache."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import tarfile
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
manifest = json.loads((ROOT / "sources.lock.json").read_text())
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("names", nargs="+", choices=manifest)
parser.add_argument("--cache", type=Path, default=Path.home() / ".cache/sunstoneos/sources")
parser.add_argument("--verify", type=Path, metavar="DIRECTORY",
                    help="only check the pinned files of one source in an existing directory")
args = parser.parse_args()


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def require_files(name, directory):
    """Refuse an extracted tree whose pinned files are missing or different."""
    changed = [path for path, value in manifest[name].get("files", {}).items()
               if not (directory / path).is_file() or digest(directory / path) != value]
    if changed:
        raise SystemExit(f"{directory} differs from the {name} pin: {', '.join(changed)}")


def extract(source, archive, directory, stamp):
    """Extract into a sibling directory, then move the complete tree into place."""
    staging = Path(tempfile.mkdtemp(prefix=f".{directory.name}.", dir=directory.parent))
    try:
        with tarfile.open(archive) as bundle:
            bundle.extractall(staging, filter="data")
        if [x.name for x in staging.iterdir()] != [directory.name]:
            raise SystemExit(f"{archive} does not hold exactly {directory.name}")
        # Another fetch may have finished meanwhile; a stamp alone is not a tree.
        if (directory.is_dir() and stamp.is_file()
                and stamp.read_text().strip() == source["sha256"]):
            return
        if directory.exists():
            # A tree without a stamp may be partial; keep it for inspection.
            kept = directory.with_name(f"{directory.name}.unverified-{os.getpid()}")
            directory.rename(kept)
            print(f"Moved unverified earlier tree to {kept}", file=sys.stderr)
        (staging / directory.name).rename(directory)
        stamp.write_text(source["sha256"] + "\n")
    finally:
        shutil.rmtree(staging, ignore_errors=True)


if args.verify:
    if len(args.names) != 1 or "files" not in manifest[args.names[0]]:
        parser.error("--verify takes one source name whose lock entry pins files")
    require_files(args.names[0], args.verify)
    # Callers such as CMake re-run the check when one of these files changes.
    for path in manifest[args.names[0]]["files"]:
        print(args.verify / path)
    sys.exit()
args.cache.mkdir(parents=True, exist_ok=True)
for name in args.names:
    source = manifest[name]
    archive = args.cache / source["archive"]
    archive.parent.mkdir(parents=True, exist_ok=True)
    if not archive.exists():
        # Archive names such as x.c and x.h share a stem; keep downloads apart.
        temporary = archive.with_name(f"{archive.name}.{os.getpid()}.download")
        try:
            urllib.request.urlretrieve(source["url"], temporary)
            if digest(temporary) != source["sha256"]:
                raise SystemExit(f"Hash mismatch: {name}")
            temporary.replace(archive)
        finally:
            temporary.unlink(missing_ok=True)
    if digest(archive) != source["sha256"]:
        raise SystemExit(f"Cached archive hash mismatch: {archive}")
    if "directory" in source:
        directory = args.cache / source["directory"]
        stamp = directory.with_name(directory.name + ".sha256")
        if not (directory.is_dir() and stamp.is_file()
                and stamp.read_text().strip() == source["sha256"]):
            extract(source, archive, directory, stamp)
        require_files(name, directory)
        print(directory)
    else:
        print(archive)
