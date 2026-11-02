#!/usr/bin/env python3
"""Host cases for the control interrupt class: GIC priority view rules and the lock-free ring.
Also refuses a literal GIC priority or a kernel API call in the control handler source."""
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
source = (root / 'runtime/control_irq.c').read_text()
handler = source[source.index('static void handle('):source.index('void __wrap_vApplicationIRQHandler')]
banned = re.findall(r'\b(?:x[A-Z]\w+|v[A-Z]\w+|ul[A-Z]\w+|pv[A-Z]\w+|portENTER\w+|portDISABLE\w+)\(', handler)
assert not banned, f'the control handler calls the kernel: {banned}'
assert not re.search(r'0x[0-9a-fA-F]{2}u?\s*\)?\s*;', source.split('static void arm')[1].split('static void handle')[0]), \
    'arm() writes a priority literal; use a733_gic_priority.h'
with tempfile.TemporaryDirectory(prefix='control-class-') as build:
    binary = Path(build) / 'class'
    env = dict(os.environ, TMPDIR=build)
    subprocess.run(['clang-21', '-std=c11', '-O2', '-pthread', '-Wall', '-Wextra', '-Werror', str(root / 'checks/scheduler-control-class.c'),
                    '-o', str(binary)], check=True, env=env)
    sys.exit(subprocess.run([str(binary)], env=env, timeout=300).returncode)
