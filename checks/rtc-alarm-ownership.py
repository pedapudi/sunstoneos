#!/usr/bin/env python3
"""Check RTC alarm ownership, callback failures and preserved calendar behavior."""
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
    commands = []
    logs = []

    def run(command):
        command = [str(x) for x in command]
        commands.append(command)
        result = subprocess.run(command, cwd=ROOT, env=harness.environment(),
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        logs.append(result.stdout)
        (output / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
        (output / 'check.log').write_text(''.join(logs))
        result.check_returncode()

    common = [harness.CLANG, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
              '-Iinclude']
    for name, sources in (
        ('ownership', ['checks/rtc-alarm-ownership.c', 'drivers/rtc/a733_rtc.c']),
        ('alarm', ['checks/a733-rtc.c', 'drivers/rtc/a733_rtc.c', 'drivers/rtc/a733_rtc_boot.c']),
        ('calendar', ['checks/a733-rtc-calendar.c', 'drivers/rtc/a733_rtc.c',
                      'drivers/rtc/a733_rtc_initialize.c']),
    ):
        run([*common, *harness.SANITIZE, '-MMD', '-MF', output / (name + '.d'),
             *sources, '-o', output / name])
        run([output / name])
    run([*common, '--target=aarch64-none-elf', '-ffreestanding', '-fno-builtin',
         '-MMD', '-MF', output / 'native.d', '-c', 'drivers/rtc/a733_rtc.c',
         '-o', output / 'a733_rtc.o'])

    # Each compilation has several sources; record their headers explicitly
    # as well as every translation unit, the runner and the shared policy.
    inputs = ['checks/rtc-alarm-ownership.py', 'checks/rtc-alarm-ownership.c',
              'checks/a733-rtc.c', 'checks/a733-rtc-calendar.c', 'checks/harness.py',
              'drivers/rtc/a733_rtc.c', 'drivers/rtc/a733_rtc_boot.c',
              'drivers/rtc/a733_rtc_initialize.c', 'include/a733_rtc.h',
              'include/a733_rtc_boot.h', 'include/a733_rtc_initialize.h', 'include/a733_range.h']
    hashes = {}
    for name in inputs:
        destination = output / 'inputs' / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / name, destination)
        hashes[name] = hashlib.sha256(destination.read_bytes()).hexdigest()
    (output / 'result.json').write_text(json.dumps({
        'passed': True, 'physical_access': False, 'inputs': hashes,
        'compiler': {'path': str(harness.CLANG),
                     'sha256': hashlib.sha256(harness.CLANG.read_bytes()).hexdigest()},
        'scope': 'Actual RTC driver with modeled callbacks; no physical alarm or GIC delivery.',
    }, indent=2) + '\n')
    print(''.join(logs), end='')


if __name__ == '__main__':
    main()
