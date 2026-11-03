#!/usr/bin/env python3
"""Build and run the production fixed GPU gateway on two QEMU CPUs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

CHECK_INPUTS = ['runtime/gpu_call.S', 'include/pvr_gpu_call.h',
                'patches/freertos-gpu-call.patch', 'patches/freertos-smp-asm.patch',
                'patches/freertos-smp-c.patch', 'patches/freertos-smp-macro.patch',
                'platform/qemu-smp', 'checks/gpu-call.c', 'checks/gpu-call-abi.S']
GATE_RUNS = [
    ['--freertos', '{cache}/sources/FreeRTOS-Kernel-97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2', '--clang', '/usr/lib/llvm-21/bin/clang', '--linker', '{cache}/host/usr/lib/llvm-21/bin/ld.lld', '--qemu', '{cache}/host/usr/bin/qemu-system-aarch64', '--output', '{output}'],
]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--freertos', type=Path, required=True)
    p.add_argument('--clang', type=Path, required=True)
    p.add_argument('--linker', type=Path, required=True)
    p.add_argument('--qemu', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--case', choices=['normal', 'no-dispatch', 'no-gateway'], action='append')
    a = p.parse_args()
    root = Path(__file__).resolve().parents[1]
    a.output.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ, TMPDIR=str(a.output))
    commands = []

    def run(cmd, *, data=None, timeout=30):
        commands.append([str(x) for x in cmd])
        (a.output / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
        r = subprocess.run([str(x) for x in cmd], input=data, text=True,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           env=env, timeout=timeout)
        with (a.output / 'build.log').open('a') as f:
            f.write(r.stdout)
        if r.returncode:
            raise RuntimeError(f'command failed ({r.returncode}): {cmd}\n{r.stdout}')
        return r.stdout

    port = a.freertos / 'portable/GCC/ARM_CR82'
    for name, patch in [('port.c', 'freertos-smp-c.patch'),
                        ('portASM.S', 'freertos-smp-asm.patch'),
                        ('portmacro.h', 'freertos-smp-macro.patch')]:
        target = a.output / name
        target.write_bytes((port / name).read_bytes())
        run(['patch', '--batch', str(target)], data=(root / 'patches' / patch).read_text())
    run(['patch', '--batch', str(a.output / 'portASM.S')],
        data=(root / 'patches/freertos-gpu-call.patch').read_text())
    # The fixture-only SVC returns the actual exception-origin mode. It never
    # changes the origin mode or elevates the task.
    start = (root / 'platform/qemu-smp/start.S').read_text()
    start = start.replace('    cmp x0, #119\n', '    cmp x0, #122\n    b.eq .Lprobe_mode\n    cmp x0, #119\n')
    start += '''\n.Lprobe_mode:
    mrs x0, spsr_el1
    and x0, x0, #15
    ldr x1, [sp, #8]
    add sp, sp, #16
    eret
'''
    (a.output / 'start.S').write_text(start)
    flags = ['--target=aarch64-none-elf', '-march=armv8-a', '-mstrict-align',
             '-ffreestanding', '-fno-builtin', '-fno-stack-protector',
             '-fno-unwind-tables', '-fno-asynchronous-unwind-tables',
             '-ffunction-sections', '-fdata-sections', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
             '-DINCLUDE_xTaskGetCurrentTaskHandle=1', '-DINCLUDE_xTaskGetSchedulerState=1',
             '-DconfigTASK_NOTIFICATION_ARRAY_ENTRIES=3']
    for directory in [root / 'platform/qemu-smp', root / 'include', root / 'runtime/include',
                      a.freertos / 'include', a.output]:
        flags += ['-I', str(directory)]
    sources = [a.output / 'start.S', root / 'platform/qemu-smp/platform.c',
               root / 'runtime/memory.c', a.freertos / 'tasks.c', a.freertos / 'queue.c',
               a.freertos / 'list.c', a.output / 'port.c', a.output / 'portASM.S']
    objects = []
    for index, source in enumerate(sources):
        obj = a.output / f'common-{index}.o'
        extra = ['-DvApplicationIRQHandler=gpu_call_base_irq'] if source.name == 'platform.c' else []
        run([a.clang, *flags, *extra, '-c', source, '-o', obj])
        objects.append(obj)
    results = []
    for case in a.case or ['normal', 'no-dispatch', 'no-gateway']:
        case_dir = a.output / case
        case_dir.mkdir()
        own = [root / 'checks/gpu-call.c']
        define = [] if case == 'normal' else ['-DGPU_CALL_' + case.upper().replace('-', '_') + '=1']
        if case != 'no-gateway':
            own += [root / 'runtime/gpu_call.S', root / 'checks/gpu-call-abi.S']
        linked = list(objects)
        for index, source in enumerate(own):
            obj = case_dir / f'case-{index}.o'
            run([a.clang, *flags, *define, '-c', source, '-o', obj])
            linked.append(obj)
        elf = case_dir / 'gpu-call.elf'
        run([a.linker, '-T', root / 'platform/qemu-smp/link.ld', '--gc-sections',
             '-Map=' + str(case_dir / 'gpu-call.map'), *linked, '-o', elf])
        command = [a.qemu, '-machine', 'virt,gic-version=3,secure=off,virtualization=off',
                   '-icount', 'shift=0,sleep=off', '-cpu', 'cortex-a55', '-smp', '2', '-m', '128M', '-nographic', '-monitor', 'none',
                   '-semihosting-config', 'enable=on,target=native', '-kernel', elf]
        output = run(command, timeout=15)
        (case_dir / 'run.log').write_text(output)
        passed = 'PASS GPU call:' in output and 'FAIL' not in output
        result = {'case': case, 'passed': passed, 'elf_sha256': hashlib.sha256(elf.read_bytes()).hexdigest()}
        results.append(result)
        (a.output / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
        print(output, end='')
        if not passed:
            raise RuntimeError(f'GPU call case {case} failed')


if __name__ == '__main__':
    main()
