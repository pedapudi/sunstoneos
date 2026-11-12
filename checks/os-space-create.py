#!/usr/bin/env python3
"""Check the task-creation hook of include/os_space.h: space from the creator's tag or the creation call, a direct creation in the init hook refused."""
import argparse
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True
import harness

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['include/os_space.h', 'checks/os-space-create.c']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    binary = args.output / 'os-space-create'
    subprocess.run([str(harness.CLANG), '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', *harness.SANITIZE,
                    '-I' + str(ROOT / 'include'), str(ROOT / 'checks/os-space-create.c'), '-o', str(binary)],
                   check=True, cwd=ROOT, env=harness.environment())
    result = subprocess.run([str(binary)], capture_output=True, text=True, env=harness.environment())
    print(result.stdout + result.stderr, end='')
    result.check_returncode()


if __name__ == '__main__':
    main()
