#!/usr/bin/env python3
"""Exercise handle publication/retirement and inspect native lookup code."""
import argparse
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def run(command, env):
    result = subprocess.run(command, text=True, capture_output=True, env=env, check=True)
    return result.stdout


def check(build, compiler, objdump):
    build.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, TMPDIR=str(build))
    source = str(ROOT / 'kernel/scheduler/freertos_registry.c')
    common = [compiler, '-std=c11', '-Wall', '-Wextra', '-Werror']
    binary = build / 'registry'
    run([*common, '-O1', '-g', '-pthread', '-fsanitize=address,undefined', source,
         str(ROOT / 'kernel/scheduler/owner.c'),
         str(ROOT / 'checks/scheduler-kernel-registry.c'), '-o', str(binary)], env)
    print(run([str(binary)], env), end='')
    obj = build / 'registry-aarch64.o'
    run([*common, '--target=aarch64-none-elf', '-march=armv8.1-a+lse',
         '-ffreestanding', '-O2', '-c', source, '-o', str(obj)], env)
    disassembly = run([objdump, '-d', str(obj)], env)
    (build / 'registry-aarch64.disassembly').write_text(disassembly)
    assert not re.search(r'\b(?:ldaxr|stlxr|ldxr|stxr)\b', disassembly)
    print('scheduler kernel registry: native AArch64 compile passed; no LL/SC')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', required=True, type=Path)
    parser.add_argument('--cc', default='clang-21')
    parser.add_argument('--objdump', default='llvm-objdump-21')
    args = parser.parse_args()
    check(args.build_dir.resolve(), args.cc, args.objdump)


if __name__ == '__main__':
    main()
