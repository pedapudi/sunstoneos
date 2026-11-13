#!/usr/bin/env python3
"""Compile and exercise the scheduler's handle registry without board access.

The registry's descriptor table is its own open-addressed handle table. The
cases cover home slots, collision chains across tombstones, recycled task
addresses, late references on closed slots, stale keys and a full table.
"""
import argparse
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
GATE_RUNS = [['--build-dir', '{output}']]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path,
                        default=Path.home() / '.cache/sunstoneos/build/scheduler-registry')
    parser.add_argument('--cc', default='clang-21')
    args = parser.parse_args()
    build = args.build_dir.resolve()
    build.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, TMPDIR=str(build))
    binary = build / 'scheduler-registry'
    subprocess.run([args.cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                    '-fsanitize=address,undefined', str(ROOT / 'kernel/scheduler/owner.c'),
                    str(ROOT / 'kernel/scheduler/freertos_registry.c'),
                    str(ROOT / 'checks/scheduler-registry.c'), '-o', str(binary)], check=True, env=env)
    print(subprocess.run([str(binary)], check=True, text=True, capture_output=True, env=env).stdout, end='')


if __name__ == '__main__':
    main()
