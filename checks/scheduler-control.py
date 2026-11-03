#!/usr/bin/env python3
"""Exercise actual control code and reject AArch64 LL/SC publication."""
import argparse
import os
from pathlib import Path
import re
import subprocess
ROOT = Path(__file__).resolve().parents[1]
GATE_RUNS = [['--build-dir', '{output}']]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=Path.home() / '.cache/sunstoneos/build/scheduler-control')
    args = parser.parse_args()
    build = args.build_dir.resolve()
    build.mkdir(parents=True, exist_ok=True)
    temporary = build / 'tmp'
    temporary.mkdir(exist_ok=True)
    env = dict(os.environ, TMPDIR=str(temporary))
    common = ['/usr/bin/clang-21', '-std=c11', '-Wall', '-Wextra', '-Werror']
    sources = [ROOT / 'kernel/scheduler/freertos_control.c', ROOT / 'kernel/scheduler/owner.c', ROOT / 'checks/scheduler-control.c']
    def run(command):
        result = subprocess.run(list(map(str, command)), check=True, text=True, capture_output=True, env=env)
        if result.stderr:
            print(result.stderr, end='')
        return result.stdout
    binary = build / 'scheduler-control'
    run(common + ['-O1', '-g', '-pthread', '-fsanitize=address,undefined', *sources, '-o', binary])
    print(run(['/usr/bin/time', '-f', 'peak RSS %M KiB', binary]), end='')
    obj = build / 'control-aarch64.o'
    run(common + ['--target=aarch64-none-elf', '-march=armv8.1-a+lse', '-ffreestanding', '-O2', '-c', sources[0], '-o', obj])
    dis = run(['llvm-objdump-21', '-d', obj])
    (build / 'control-aarch64.disassembly').write_text(dis)
    assert not re.search(r'\b(?:ldaxr|stlxr|ldxr|stxr)\b', dis)
    print('scheduler control: AArch64 LSE atomics contain no LL/SC')
if __name__ == '__main__':
    main()
