#!/usr/bin/env python3
"""Build and run checks/npu-graph-index.c: the index that admission reads from the application's declared graph region
(apps/os/npu-graph-index.c) refuses every malformed index and reads a valid one."""
import argparse
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ('apps/os/npu-graph-index.c', 'apps/os/npu-graph-index.h', 'checks/npu-graph-index.c')
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--cc', default='/usr/bin/clang-21')
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
exe = a.output / 'npu-graph-index'
subprocess.run([a.cc, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                str(ROOT / 'checks/npu-graph-index.c'), str(ROOT / 'apps/os/npu-graph-index.c'), '-o', str(exe)], check=True)
done = subprocess.run([str(exe)], capture_output=True, text=True, check=True)
print(done.stdout.strip())
