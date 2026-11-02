#!/usr/bin/env python3
"""Run checks/clock-token-retention.c: one retained clock token leaves the registry and the other tokens working."""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
ap = argparse.ArgumentParser()
ap.add_argument('--evidence', type=Path, required=True)
a = ap.parse_args()
a.evidence.mkdir(parents=True, exist_ok=True)
cc = shutil.which('clang-21') or shutil.which('clang')
binary = a.evidence / 'clock-token-retention'
done = subprocess.run([cc, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-Iinclude',
                       'checks/clock-token-retention.c', 'drivers/clock/a733_clock_leases.c', '-o', str(binary)], cwd=root, capture_output=True, text=True)
if done.returncode:
    sys.exit(done.stderr)
done = subprocess.run([str(binary)], capture_output=True, text=True)
print(done.stdout, end='')
if done.returncode:
    sys.exit(done.stderr)
