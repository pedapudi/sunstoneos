#!/usr/bin/env python3
"""Compile and run the register-selector model without hardware access."""
import argparse
from pathlib import Path
import subprocess
p = argparse.ArgumentParser()
p.add_argument('--cc', default='cc')
p.add_argument('--build', type=Path, required=True)
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
a.build.mkdir(parents=True, exist_ok=True)
exe = a.build / 'scheduler-cpu-observation'
subprocess.run([a.cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                '-I' + str(root / 'include'), str(root / 'checks/scheduler-cpu-observation.c'),
                str(root / 'platform/a733/cpu_observation.c'), '-o', str(exe)], check=True)
subprocess.run([str(exe)], check=True)
