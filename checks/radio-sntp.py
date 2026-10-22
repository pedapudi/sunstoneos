#!/usr/bin/env python3
"""Check SNTP reply validation, RTC conversion and the client sequence on the host."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ['checks/radio-sntp.c', 'drivers/net/sunstone_sntp.c']
INPUTS = SOURCES + ['checks/radio-sntp.py', 'include/sunstone_net.h', 'include/a733_rtc_service.h']


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ['output', 'freertos', 'tcp', 'headers']:
        p.add_argument('--' + name, type=Path, required=True)
    p.add_argument('--cc', default='/usr/bin/clang-21')
    a = p.parse_args()
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    common = ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-DSUNSTONE_AIC8800_RSN=1',
              '-Inetwork/include', '-Iinclude', '-I' + str(a.freertos / 'include'),
              '-I' + str(a.freertos / 'portable/GCC/ARM_AARCH64_SRE'),
              '-I' + str(a.tcp / 'source/include'), '-I' + str(a.tcp / 'source/portable/Compiler/GCC')]
    executable = out / 'radio-sntp'
    commands = [
        [a.cc, *common, '-fsanitize=address,undefined', '-fno-omit-frame-pointer', *SOURCES, '-o', str(executable)],
        # The production unit also compiles freestanding for the board CPU.
        [a.cc, '--target=aarch64-none-elf', '-mcpu=cortex-a55', '-ffreestanding', '-fno-builtin',
         '-mgeneral-regs-only', *common, '-isystem', str(a.headers), '-include', 'stdint.h',
         '-c', 'drivers/net/sunstone_sntp.c', '-o', str(out / 'sunstone_sntp.o')],
        [str(executable)]]
    record = {'inputs': {n: digest(ROOT / n) for n in INPUTS}, 'runs': []}
    passed = True
    for command in commands:
        run = subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                             env={**os.environ, 'ASAN_OPTIONS': 'halt_on_error=1'})
        record['runs'].append({'command': command, 'exit_code': run.returncode, 'stdout': run.stdout, 'stderr': run.stderr})
        if run.returncode:
            passed = False
            break
    record['result'] = 'passed' if passed else 'failed'
    if passed:
        record['artifacts'] = {f.name: digest(f) for f in (executable, out / 'sunstone_sntp.o')}
    (out / 'results.json').write_text(json.dumps(record, indent=2) + '\n')
    last = record['runs'][-1]
    print(json.dumps({'result': record['result'], 'stdout': last['stdout'], 'stderr': last['stderr'][-3000:]}))
    raise SystemExit(0 if passed else 1)


if __name__ == '__main__':
    main()
