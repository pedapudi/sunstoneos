#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check application topology queries with disabled, incomplete and calibrated data."""
import argparse
from pathlib import Path
import subprocess
from harness import CLANG, SANITIZE, environment

CHECK_INPUTS = ['runtime/cpu_topology_app.c', 'platform/a733/cpu_topology.c',
                'include/sunstone_app.h', 'include/sunstone_cpu_topology.h',
                'cmake/sunstone-os-core.cmake']
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
out = a.output.resolve()
if out == root or root in out.parents:
    p.error('output must be outside tracked source')
out.mkdir(parents=True, exist_ok=True)
env = {**environment(), 'TMPDIR': str(out)}
flags = [str(CLANG), '-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
         *SANITIZE, '-ffunction-sections', '-fdata-sections', '-I' + str(root / 'include')]
subprocess.run([*flags, '-Da733_cpu_topology=a733_cpu_topology_data', '-c',
                str(root / 'platform/a733/cpu_topology.c'), '-o', str(out / 'data.o')], check=True, env=env)
for enabled in (False, True):
    exe = out / ('enabled' if enabled else 'disabled')
    sources = ['checks/scheduler-topology-app.c', 'runtime/cpu_topology_app.c']
    extra = []
    if enabled:
        sources += ['kernel/scheduler/topology.c', 'drivers/board/orangepi_zero3w.c',
                    'drivers/clock/a733_cpu_clock.c']
        extra = ['-DSUNSTONE_A733_CPU_TOPOLOGY=1', str(out / 'data.o')]
    subprocess.run([*flags, *extra, *[str(root / s) for s in sources], '-Wl,--gc-sections',
                    '-o', str(exe)], check=True, env=env)
    for case in ('normal', 'missing', 'affinity', 'midr', 'identity', 'invalid', 'uncalibrated', 'calibrated', 'revision') if enabled else ('disabled',):
        r = subprocess.run([str(exe), case], check=True, env=env, capture_output=True, text=True)
        (out / (exe.name + '-' + case + '.log')).write_text(r.stdout)
        print(exe.name, case, r.stdout.strip())
