#!/usr/bin/env python3
"""Run real multicore FreeRTOS fork images with bounded QEMU execution."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


CHECK_INPUTS = ['checks/scheduler-partitioned-qemu.c', 'checks/scheduler-partitioned-qemu-config/FreeRTOSConfig.h', 'checks/smp-kernel.c', 'checks/smp8-soak.c', 'checks/smp-runtime.cc', 'cmake/scheduler-partitioned.cmake', 'cmake/smp.cmake', 'patches/freertos-partitioned-port-asm.patch', 'kernel/scheduler']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--qemu', required=True, type=Path)
    parser.add_argument('--cpu', default='cortex-a76')
    parser.add_argument('--nm', default='/usr/bin/llvm-nm-21', type=Path)
    parser.add_argument('--case', action='append', choices=['kernel', 'soak', 'concurrent', 'runtime'])
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, TMPDIR=str(args.output.resolve()))
    cases = {
        'kernel': ('smp-kernel-model', 2, 'PASS SMP: 256'),
        'soak': ('smp8-soak-model', 8, 'PASS SMP8 soak: 64-lap'),
        'concurrent': ('scheduler-partitioned-qemu', 8, 'PASS partitioned concurrent:'),
        'runtime': ('smp-runtime-model', 2, 'PASS SMP runtime:'),
    }
    reports = []
    for case in args.case or ['kernel', 'concurrent', 'soak']:
        name, cores, marker = cases[case]
        image = args.build_dir.resolve() / (name + '.elf')
        symbols = subprocess.check_output([str(args.nm), '--defined-only', str(image)], text=True)
        for hook in ('ssKernelOwnerIRQExit', 'ssKernelOwnerYieldTail'):
            assert re.search(r' [Tt] ' + hook + r'$' , symbols, re.M), f'{image}: private fork hook {hook} missing'
        command = [str(args.qemu.resolve()), '-machine',
            'virt,gic-version=3,secure=off,virtualization=off', '-accel', 'tcg,thread=multi',
            '-cpu', args.cpu, '-smp', str(cores), '-m', '128M', '-nographic', '-monitor', 'none',
            '-semihosting-config', 'enable=on,target=native',
            '-device', f'loader,file={image},cpu-num=0']
        try:
            result = subprocess.run(['/usr/bin/time', '-v', *command], capture_output=True,
                                    text=True, timeout=45, env=env)
            log = result.stdout + result.stderr
            passed = result.returncode == 0 and marker in log and 'FAIL' not in log
        except subprocess.TimeoutExpired as error:
            log = (error.stdout or b'').decode() + (error.stderr or b'').decode() + '\nQEMU timeout after 45 seconds\n'
            passed = False
        path = args.output / (args.cpu + '-' + case + '.log')
        path.write_text(log)
        peak = re.search(r'Maximum resident set size \(kbytes\): (\d+)', log)
        reports.append({'case': case, 'cpu': args.cpu, 'cores': cores,
            'image': str(image), 'sha256': hashlib.sha256(image.read_bytes()).hexdigest(),
            'command': command, 'passed': passed, 'peak_rss_kib': int(peak[1]) if peak else None,
            'log': str(path.resolve())})
        (args.output / (args.cpu + '-report.json')).write_text(json.dumps(reports, indent=2) + '\n')
        print(('PASS' if passed else 'FAIL') + f' {case}: {path}', flush=True)
        if not passed:
            raise SystemExit(1)


if __name__ == '__main__':
    main()
