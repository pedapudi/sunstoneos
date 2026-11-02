#!/usr/bin/env python3
"""Check native and QEMU private task storage with the real SMP newlib TLS ABI."""
import argparse
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


CHECK_INPUTS = ['cmake/scheduler-partitioned.cmake', 'cmake/a733-smp-cached.cmake', 'cmake/a733-board.cmake', 'cmake/a733-storage-maintenance.cmake', 'include/FreeRTOSConfig.h', 'platform/qemu-smp/freertos_tasks_c_additions.h', 'platform/a733-smp-cached/freertos_tasks_c_additions.h', 'patches/freertos-partitioned-layout.patch', 'patches/freertos-partitioned-tasks.patch', 'patches/freertos-partitioned-api.patch', 'kernel/scheduler']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--freertos', required=True, type=Path)
    parser.add_argument('--sysroot', required=True, type=Path)
    parser.add_argument('--cc', required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, TMPDIR=str(args.output.resolve()))
    for name, platform in [('freertos-smp', 'qemu-smp'),
                           ('freertos-a733-smp-cached', 'a733-smp-cached')]:
        private = args.build_dir.resolve() / name
        source = args.output / (name + '.c')
        source.write_text('#include "' + str(private / 'tasks.c') + '"\n'
            '_Static_assert(sizeof(TCB_t) == sizeof(StaticTask_t), "task storage size");\n'
            '_Static_assert(_Alignof(TCB_t) == _Alignof(StaticTask_t), "task storage alignment");\n'
            '_Static_assert(offsetof(TCB_t, uxOwnerSlot) == offsetof(StaticTask_t, uxDummyOwnerSlot), "owner slot offset");\n'
            '_Static_assert(offsetof(TCB_t, xTLSBlock) == offsetof(StaticTask_t, xDummy17), "newlib TLS offset");\n'
            '_Static_assert(sizeof(((StaticTask_t *)0)->xDummy17) == sizeof(struct _reent), "real newlib reent");\n')
        flags = ['--target=aarch64-none-elf', '-march=armv8.1-a+lse',
                 '-ffreestanding', '-mgeneral-regs-only', '-fsyntax-only',
                 '-DSUNSTONE_NEWLIB=1', '-DSUNSTONE_SMP_NEWLIB=1',
                 '-DconfigSS_PARTITIONED_SCHEDULER=1', '-DSUNSTONE_SMP_CORES=8',
                 '-I' + str(private / 'include'), '-I' + str(ROOT / 'platform' / platform),
                 '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'kernel/scheduler'),
                 '-I' + str(args.freertos.resolve() / 'include'), '-I' + str(private),
                 '-isystem', str(args.sysroot.resolve() / 'include')]
        subprocess.run([str(args.cc), '-std=c11', *flags, str(source)], check=True, env=env)
        application = args.output / (name + '.cc')
        application.write_text('#include "FreeRTOS.h"\n#include "task.h"\n#include <reent.h>\n'
            'static_assert(sizeof(((StaticTask_t *)0)->xDummy17) == sizeof(struct _reent), "application newlib ABI");\n'
            'static_assert(__is_trivially_copyable(StaticTask_t), "application task storage ABI");\n'
            'StaticTask_t external_task_storage;\n')
        subprocess.run([str(args.cc), '-std=c++23', *flags, str(application)], check=True, env=env)
    private = args.build_dir.resolve() / 'freertos-a733-smp-cached'
    native = args.output / 'native-network'
    native.mkdir(exist_ok=True)
    (native / 'FreeRTOSConfig.h').write_text('#include "' + str(ROOT / 'apps/board/FreeRTOSConfig.h') + '"\n')
    text = (private / 'tasks.c').read_text()
    prefix = text.split('#define SS_FREERTOS_TASKS_TRANSLATION 1', 1)[0]
    # The adapter insertion follows an opening selector conditional. Complete
    # that conditional after retaining the exact upstream private TCB layout.
    prefix += '#endif\n'
    translation = native / 'layout.c'
    translation.write_text(prefix + '\n#include <stddef.h>\n'
        '_Static_assert(configSUPPORT_DYNAMIC_ALLOCATION == 1, "native network selector");\n'
        '_Static_assert(sizeof(TCB_t) == sizeof(StaticTask_t), "native network storage size");\n'
        '_Static_assert(_Alignof(TCB_t) == _Alignof(StaticTask_t), "native network storage alignment");\n'
        '_Static_assert(offsetof(TCB_t, xTLSBlock) == offsetof(StaticTask_t, xDummy17), "native network TLS offset");\n')
    subprocess.run([str(args.cc), '-std=c11', '-I' + str(native), *flags,
                    str(translation)], check=True, env=env)
    subprocess.run([str(args.cc), '-std=c++23', '-I' + str(native), *flags,
                    str(application)], check=True, env=env)
    print('PASS native board/network task layout with dynamic selector; dynamic task execution remains gated')
    print('PASS partitioned ABI: native and QEMU TCB size/alignment, owner slot, real newlib TLS and external C++ storage')


if __name__ == '__main__':
    main()
