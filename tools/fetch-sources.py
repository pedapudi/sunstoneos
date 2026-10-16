#!/usr/bin/env python3
"""Fetch pinned source archives into the local development cache."""
import argparse
import hashlib
import json
from pathlib import Path
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
manifest = json.loads((ROOT / "sources.lock.json").read_text())
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("names", nargs="+", choices=manifest)
parser.add_argument("--cache", type=Path, default=Path.home() / ".cache/sunstoneos/sources")
args = parser.parse_args()
args.cache.mkdir(parents=True, exist_ok=True)
for name in args.names:
    source = manifest[name]
    archive = args.cache / source["archive"]
    archive.parent.mkdir(parents=True, exist_ok=True)
    if not archive.exists():
        temporary = archive.with_suffix(".download")
        urllib.request.urlretrieve(source["url"], temporary)
        if hashlib.sha256(temporary.read_bytes()).hexdigest() != source["sha256"]:
            temporary.unlink()
            raise SystemExit(f"Hash mismatch: {name}")
        temporary.replace(archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != source["sha256"]:
        raise SystemExit(f"Cached archive hash mismatch: {archive}")
    if "directory" in source:
        directory = args.cache / source["directory"]
        if not directory.exists():
            with tarfile.open(archive) as bundle:
                bundle.extractall(args.cache, filter="data")
        print(directory)
    else:
        print(archive)
