#!/usr/bin/env python3
"""Run real multicore FreeRTOS fork images with bounded QEMU execution."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import time


CHECK_INPUTS = ['checks/scheduler-partitioned-qemu.c', 'checks/scheduler-partitioned-qemu-config/FreeRTOSConfig.h', 'checks/smp-kernel.c', 'checks/smp8-soak.c', 'checks/smp-runtime.cc', 'cmake/scheduler-partitioned.cmake', 'cmake/smp.cmake', 'patches/freertos-partitioned-port-asm.patch', 'kernel/scheduler']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--qemu', required=True, type=Path)
    parser.add_argument('--cpu', default='cortex-a76')
    parser.add_argument('--nm', default='/usr/bin/llvm-nm-21', type=Path)
    parser.add_argument('--case', action='append', choices=['kernel', 'soak', 'concurrent', 'runtime', 'heap'])
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, TMPDIR=str(args.output.resolve()))
    cases = {
        'kernel': ('smp-kernel-model', 2, 'PASS SMP: 256'),
        'soak': ('smp8-soak-model', 8, 'PASS SMP8 soak: 64-lap'),
        'concurrent': ('scheduler-partitioned-qemu', 8, 'PASS partitioned concurrent:'),
        'runtime': ('smp-runtime-model', 2, 'PASS SMP runtime:'),
        'heap': ('scheduler-partitioned-heap-qemu', 2, 'PASS partitioned heap lifecycle:'),
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
        started = time.monotonic()
        process = subprocess.Popen(['/usr/bin/time', '-v', *command], stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, start_new_session=True, env=env)
        try:
            stdout, stderr = process.communicate(timeout=45)
            log = stdout + stderr
            passed = process.returncode == 0 and marker in log and 'FAIL' not in log
        except subprocess.TimeoutExpired:
            # The timer has a child QEMU process. Stop this run's whole group
            # so a timed-out emulator cannot remain active after the check.
            os.killpg(process.pid, signal.SIGKILL)
            stdout, stderr = process.communicate()
            log = stdout + stderr + '\nQEMU timeout after 45 seconds\n'
            passed = False
        elapsed = time.monotonic() - started
        counters = {}
        if case == 'concurrent':
            owners = []
            for row in re.finditer(r'owner cpu=(0x[0-9a-f]+) switches=(0x[0-9a-f]+) idle-selections=(0x[0-9a-f]+) busy-progress=(0x[0-9a-f]+) switch-path-global-locks=(0x[0-9a-f]+) admin-acquisitions=(0x[0-9a-f]+) switch-admin-acquisitions=(0x[0-9a-f]+)', log):
                owners.append(dict(zip(('cpu', 'switches', 'idle_selections', 'busy_progress', 'switch_path_global_locks', 'admin_acquisitions', 'switch_admin_acquisitions'),
                    (int(value, 16) for value in row.groups()))))
            local = re.search(r'local-wake attempts=(0x[0-9a-f]+) completions=(0x[0-9a-f]+) ticks=(0x[0-9a-f]+) tick-hz=(0x[0-9a-f]+) busy-home-progress=(0x[0-9a-f]+) idle-peer-samples=(0x[0-9a-f]+) all-global-locks=(0x[0-9a-f]+)', log)
            withheld = re.search(r'withheld-ready opportunities=(0x[0-9a-f]+) stranded-observations=(0x[0-9a-f]+) home-completions=(0x[0-9a-f]+) elapsed-ticks=(0x[0-9a-f]+) home-mask=(0x[0-9a-f]+) blocker-priority=(0x[0-9a-f]+) receiver-priority=(0x[0-9a-f]+)', log)
            if withheld:
                counters['withheld_ready'] = dict(zip(('opportunities', 'stranded_observations', 'home_completions', 'elapsed_ticks', 'allowed_mask', 'blocker_priority', 'receiver_priority'),
                    (int(value, 16) for value in withheld.groups())))
            counters['owners'] = owners
            if local:
                counters['local'] = dict(zip(('attempts', 'completions', 'ticks', 'tick_hz', 'busy_home_progress', 'idle_peer_samples', 'all_global_locks'),
                    (int(value, 16) for value in local.groups())))
                measured = counters['local']
                counters['local']['completions_per_second'] = (
                    measured['completions'] * measured['tick_hz'] / measured['ticks']
                    if measured['ticks'] else None)
            passed = passed and len(owners) == 8 and local is not None and withheld is not None
            passed = passed and all(owner['switch_path_global_locks'] == 0 for owner in owners)
            if local:
                passed = passed and measured['all_global_locks'] == 0 and measured['ticks'] > 0
        path = args.output / (args.cpu + '-' + case + '.log')
        path.write_text(log)
        peak = re.search(r'Maximum resident set size \(kbytes\): (\d+)', log)
        reports.append({'case': case, 'cpu': args.cpu, 'cores': cores,
            'image': str(image), 'sha256': hashlib.sha256(image.read_bytes()).hexdigest(),
            'command': command, 'passed': passed, 'elapsed_seconds': elapsed, 'counters': counters, 'peak_rss_kib': int(peak[1]) if peak else None,
            'log': str(path.resolve())})
        (args.output / (args.cpu + '-report.json')).write_text(json.dumps(reports, indent=2) + '\n')
        print(('PASS' if passed else 'FAIL') + f' {case}: {path}', flush=True)
        if not passed:
            raise SystemExit(1)


if __name__ == '__main__':
    main()
