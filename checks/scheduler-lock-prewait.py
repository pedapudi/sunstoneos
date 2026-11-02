#!/usr/bin/env python3
"""Build and run, once, the eight-thread fairness check of the unmasked task-lock wait.

The C check filters host descheduling per wait (see its header comment) and exits nonzero
when a phase is inconclusive, so this wrapper does not retry."""
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
    sys.exit(subprocess.run([str(binary)], env=env, timeout=300).returncode)
