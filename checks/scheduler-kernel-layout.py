#!/usr/bin/env python3
"""Check exact fork patch composition and C/C++ static task layout."""
import argparse
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def command(args, env):
    result = subprocess.run([str(a) for a in args], text=True, capture_output=True, env=env)
    if result.returncode:
        raise RuntimeError(f'{args[0]} failed:\n{result.stdout}{result.stderr}')
    return result.stdout


def check(source, build, compiler, sysroot):
    build.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, TMPDIR=str(build))
    include = build / 'include'
    include.mkdir(exist_ok=True)
    for output, original, patch in (
        (build / 'tasks-smp.c', source / 'tasks.c', 'freertos-smp-tasks.patch'),
        (build / 'tasks.c', build / 'tasks-smp.c', 'freertos-partitioned-tasks.patch'),
        (include / 'FreeRTOS.h', source / 'include/FreeRTOS.h', 'freertos-partitioned-layout.patch'),
        (build / 'portmacro.h', source / 'portable/GCC/ARM_CR82/portmacro.h', 'freertos-smp-macro.patch'),
    ):
        command(['patch', '--fuzz=0', '--reject-file=-', '--output', output,
                 original, ROOT / 'patches' / patch], env)
    # Checking the complete tasks translation unit observes the private TCB;
    # FreeRTOS's own creation check is runtime-only and can miss ABI drift.
    translation = build / 'layout.c'
    translation.write_text('#include "tasks.c"\n'
                           '_Static_assert(sizeof(TCB_t) == sizeof(StaticTask_t), "TCB size");\n'
                           '_Static_assert(_Alignof(TCB_t) == _Alignof(StaticTask_t), "TCB alignment");\n')
    cpp = build / 'layout.cc'
    cpp.write_text('#include "FreeRTOS.h"\n#include "task.h"\n'
                   'static_assert(__is_trivially_copyable(StaticTask_t), "C ABI storage");\n'
                   'StaticTask_t task_storage;\n')
    flags = ['--target=aarch64-none-elf', '-march=armv8.1-a+lse', '-ffreestanding',
             '-fsyntax-only', '-Werror', '-I' + str(include), '-I' + str(build),
             '-I' + str(ROOT / 'platform/qemu-smp'), '-I' + str(ROOT / 'include'),
             '-I' + str(source / 'include'), '-isystem', str(sysroot / 'include')]
    for enabled in (0, 1):
        feature = f'-DconfigSS_PARTITIONED_SCHEDULER={enabled}'
        command([compiler, '-std=c11', *flags, feature, translation], env)
        command([compiler, '-std=c++23', *flags, feature, cpp], env)
    # Unset must behave as zero, and invalid selectors must fail at the header.
    command([compiler, '-std=c11', *flags, translation], env)
    invalid = subprocess.run([str(compiler), '-std=c11', *flags,
                              '-DconfigSS_PARTITIONED_SCHEDULER=2', str(translation)],
                             text=True, capture_output=True, env=env)
    assert invalid.returncode and 'must be 0 or 1' in invalid.stderr
    print('scheduler kernel layout: exact SMP/fork composition; matching C TCB size/alignment; C++ ABI; default and selector checks')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--freertos', required=True, type=Path)
    parser.add_argument('--build-dir', required=True, type=Path)
    parser.add_argument('--cc', required=True, type=Path)
    parser.add_argument('--sysroot', required=True, type=Path)
    args = parser.parse_args()
    check(args.freertos.resolve(), args.build_dir.resolve(), args.cc.resolve(), args.sysroot.resolve())


if __name__ == '__main__':
    main()
