#!/usr/bin/env python3
"""Execute the EL1 handoff probe once and collect its result after FEL recovery."""
import argparse
import fcntl
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
CACHE = Path.home() / ".cache/sunstoneos"

def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fel", type=Path, required=True)
    parser.add_argument("--image", type=Path, default=CACHE / "build/a733-el1/probe.bin")
    parser.add_argument("--evidence", type=Path,
                        default=CACHE / "evidence" / time.strftime("el1-%Y%m%d-%H%M%S"))
    args = parser.parse_args(argv)
    sys.path.insert(0, str(ROOT/'tools'))
    import board
    raise SystemExit(board.legacy_run(args, image=args.image))


if __name__ == '__main__':
    main()
