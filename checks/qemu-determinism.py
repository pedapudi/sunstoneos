#!/usr/bin/env python3
"""A QEMU model on one thread with a fixed icount gives the same log whatever else loads the host.

checks/latency-control-integration.py runs its models with -accel tcg,thread=single and -icount shift=0,sleep=off so that guest time,
and with it every counter reading and wake latency in the log, follows the instructions executed. The case: six copies of the idle
latency-control model run at once beside twice as many busy host processes; every copy must produce the same log, byte for byte, and
the log must hold the model's pass line. A model that times out, differs, or runs without the fixed icount fails the case.
Not covered: the eight-core scheduler models of checks/scheduler-control-linked.py, whose logs differ in about one run in twenty under
load (QEMU's round-robin vCPU switch is timed by the host).
Usage: qemu-determinism.py --build DIR --output DIR
"""
import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import threading

GATE_RUNS = [['--build', '{build:latency-control-idle-model}', '--output', '{output}']]
CACHE = Path.home() / '.cache/sunstoneos'
COPIES, BUSY = 6, 12


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--qemu', type=Path, default=CACHE / 'host/usr/bin/qemu-system-aarch64')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    image = args.build / 'latency-control-model.elf'
    command = [str(args.qemu), '-M', 'virt,gic-version=3,secure=off,virtualization=off', '-accel', 'tcg,thread=single',
               '-icount', 'shift=0,sleep=off', '-cpu', 'max', '-smp', '8', '-m', '128M', '-nographic', '-nic', 'none',
               '-monitor', 'none', '-semihosting-config', 'enable=on,target=native', '-kernel', str(image)]
    busy = [subprocess.Popen([sys.executable, '-c', 'while True: pass']) for _ in range(BUSY)]
    logs = {}

    def run(index):
        result = subprocess.run(command, capture_output=True, text=True, timeout=600)
        logs[index] = result.stdout + result.stderr
    try:
        threads = [threading.Thread(target=run, args=(i,)) for i in range(COPIES)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
    finally:
        for process in busy:
            process.kill()
    digests = {hashlib.sha256(log.encode()).hexdigest() for log in logs.values()}
    (args.output / 'qemu.log').write_text(logs[0])
    assert len(logs) == COPIES and all('PASS latency model:' in log for log in logs.values()), 'a copy did not pass'
    assert len(digests) == 1, '%d different logs from %d copies under load' % (len(digests), COPIES)
    print('PASS: %d copies beside %d busy processes gave one log (%s)' % (COPIES, BUSY, next(iter(digests))[:12]))


if __name__ == '__main__':
    main()
