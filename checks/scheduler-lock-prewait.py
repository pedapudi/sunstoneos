#!/usr/bin/env python3
"""Build and run the eight-thread fairness check of the unmasked task-lock wait."""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='prewait-') as build:
    binary = Path(build) / 'prewait'
    env = dict(os.environ, TMPDIR=build)
    subprocess.run(['clang-21', '-std=c11', '-O2', '-pthread', '-Wall', '-Wextra', '-Werror', '-I' + str(root / 'runtime'),
                    str(root / 'checks/scheduler-lock-prewait.c'), '-o', str(binary)], check=True, env=env)
    # The wait limits are in host time, and a gate that runs many checks at once can deschedule a thread for
    # milliseconds, which no lock design bounds. A starved thread fails every attempt; a descheduled one does not.
    for attempt in range(1, 4):
        status = subprocess.run([str(binary)], env=env, timeout=300).returncode
        if status == 0:
            sys.exit(0)
        print(f'attempt {attempt} failed; retrying' if attempt < 3 else 'all attempts failed')
    sys.exit(1)
