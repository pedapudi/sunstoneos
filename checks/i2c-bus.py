#!/usr/bin/env python3
"""Check the interrupt-driven header I2C engine against a modeled controller and bus."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True
import harness

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    logs = []

    def run(command):
        command = [str(x) for x in command]
        result = subprocess.run(command, cwd=ROOT, env=harness.environment(),
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        logs.append(result.stdout)
        (output / 'check.log').write_text(''.join(logs))
        result.check_returncode()

    common = [harness.CLANG, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Iinclude']
    sources = ['checks/a733-i2c-bus.c', 'drivers/i2c/a733_i2c_bus.c']
    run([*common, *harness.SANITIZE, '-MMD', '-MF', output / 'bus.d', *sources, '-o', output / 'bus'])
    run([output / 'bus'])
    run([*common, '--target=aarch64-none-elf', '-ffreestanding', '-fno-builtin', '-c',
         'drivers/i2c/a733_i2c_bus.c', '-o', output / 'a733_i2c_bus.o'])
    inputs = ['checks/i2c-bus.py', 'checks/a733-i2c-bus.c', 'checks/harness.py',
              'drivers/i2c/a733_i2c_bus.c', 'include/a733_i2c_bus.h']
    hashes = {}
    for name in inputs:
        destination = output / 'inputs' / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / name, destination)
        hashes[name] = hashlib.sha256(destination.read_bytes()).hexdigest()
    (output / 'result.json').write_text(json.dumps({
        'passed': True, 'physical_access': False, 'inputs': hashes,
        'scope': 'Actual engine with a modeled controller (manual 18.11.6, 18.11.9); no physical bus.',
    }, indent=2) + '\n')
    print(''.join(logs), end='')


if __name__ == '__main__':
    main()
