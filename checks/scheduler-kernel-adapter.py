#!/usr/bin/env python3
"""Compile the private API patch and link real FreeRTOS owner transitions."""
import argparse
import os
from pathlib import Path
import subprocess
import importlib.util
ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--freertos', required=True, type=Path)
    parser.add_argument('--build-dir', required=True, type=Path)
    parser.add_argument('--sysroot', required=True, type=Path)
    parser.add_argument('--cc', default='/usr/bin/clang-21')
    args = parser.parse_args()
    build = args.build_dir.resolve()
    build.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, TMPDIR=str(build))
    def run(command):
        result = subprocess.run(list(map(str, command)), text=True, capture_output=True, env=env)
        if result.returncode:
            raise RuntimeError(f'{command[0]} failed:\n{result.stdout}{result.stderr}')
        return result.stdout
    # Keep the admitted layout chain independently buildable in both modes.
    spec = importlib.util.spec_from_file_location('layout', ROOT / 'checks/scheduler-kernel-layout.py')
    layout = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(layout)
    layout.check(args.freertos.resolve(), build, args.cc, args.sysroot.resolve())
    patched = build / 'tasks-api.c'
    run(['patch', '--fuzz=0', '--reject-file=-', '--output', patched,
         build / 'tasks.c', ROOT / 'patches/freertos-partitioned-api.patch'])
    common = [args.cc, '-std=c11', '-O1', '-g', '-ffunction-sections', '-fdata-sections',
              '-DconfigSS_PARTITIONED_SCHEDULER=1', '-DSUNSTONE_SMP_CORES=8',
              '-I' + str(build / 'include'), '-I' + str(build),
              '-I' + str(ROOT / 'platform/qemu-smp'), '-I' + str(ROOT / 'include'),
              '-I' + str(ROOT / 'kernel/scheduler'), '-I' + str(args.freertos / 'include')]
    run([*common, '--target=aarch64-none-elf', '-march=armv8.1-a+lse', '-ffreestanding',
         '-Werror', '-isystem', args.sysroot / 'include', '-c', patched, '-o', build / 'tasks-api.o'])
    translation = build / 'linked.c'
    translation.write_text('#include "' + str(ROOT / 'checks/scheduler-kernel-host-port.h') + '"\n'
                           '#include "tasks-api.c"\n'
                           '#include "' + str(ROOT / 'checks/scheduler-kernel-linked.c') + '"\n')
    binary = build / 'linked'
    sources = [args.freertos / 'list.c', *[ROOT / 'kernel/scheduler' / name for name in
               ('owner.c', 'notification.c', 'deadline.c', 'freertos_registry.c', 'freertos_control.c')]]
    run([*common, '-fsanitize=address,undefined', '-Wl,--gc-sections', translation, *sources, '-o', binary])
    print(run([binary]), end='')
    print('private adapter compiled for AArch64 and linked with actual FreeRTOS tasks/list code; live enablement remains gated')

if __name__ == '__main__':
    main()
