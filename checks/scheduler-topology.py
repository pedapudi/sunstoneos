#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile and execute the portable topology implementation with host sanitizers."""
import argparse
import os
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--cc', default='/usr/bin/clang-21')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
out = args.output.resolve()
if root == out or root in out.parents:
    parser.error('output must be outside tracked source')
out.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, TMPDIR=str(out))
sources = ['checks/scheduler-topology.c', 'kernel/scheduler/topology.c',
           'platform/a733/cpu_topology.c', 'drivers/board/orangepi_zero3w.c',
           'drivers/clock/a733_cpu_clock.c']
command = [args.cc, '-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
           '-ffunction-sections', '-fdata-sections', '-fsanitize=address,undefined',
           '-I' + str(root / 'include'), *[str(root / p) for p in sources],
           '-Wl,--gc-sections', '-o', str(out / 'scheduler-topology')]
subprocess.run(command, check=True, env=env)
result = subprocess.run([str(out / 'scheduler-topology')], check=True,
                        capture_output=True, text=True, env=env)
(out / 'result.txt').write_text(result.stdout)
print(result.stdout, end='')
