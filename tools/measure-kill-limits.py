#!/usr/bin/env python3
"""Measure the wall-clock time of a check's guest or host-binary runs at the gate's parallelism, and record it as the check's kill bound.

A check's kill bound (checks/harness.py: qemu_kill_seconds, host_kill_seconds) is a hang guard: the run it bounds executes a fixed number of
instructions (QEMU with -icount shift=0, or a host binary with constructed time), so its wall time depends only on the share of the host it
gets. The bound is kill_multiplier times the slowest run measured here, with every command of the check's result file run COPIES at once
beside BUSY busy processes; harness.run_qemu runs a killed run once more and reports a second kill as inconclusive, so the bound need not
cover a stalled host. The measurement (copies, busy processes, the load average reached, the number of runs, the date) is stored beside the
figure in tools/gate-slots.json.

Usage: measure-kill-limits.py --table qemu_checks --name radio-network --results RESULT.json [--results ...]
           [--copies 24] [--busy 36] [--reps 1] [--deterministic] [--update]
The result files are the check's own (the 'command' lists in them are the runs; their images must still exist). --deterministic adds
harness.QEMU_DETERMINISTIC to QEMU commands that lack an -icount option, for result files recorded before the check used it.
"""
import argparse
import datetime
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'checks'))
import harness  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
SLOTS = ROOT / 'tools/gate-slots.json'


def commands_of(path):
    found = []

    def walk(node):
        if isinstance(node, dict):
            command = node.get('command')
            # a negative control that must hang is killed on purpose; it has no run time to measure
            if (isinstance(command, list) and command and all(isinstance(word, str) for word in command)
                    and node.get('expected_failure') != 'timeout'):
                found.append(command)
            for value in node.values():
                walk(value)
        elif isinstance(node, list):
            for value in node:
                walk(value)
    walk(json.loads(Path(path).read_text()))
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--table', choices=('qemu_checks', 'host_checks'), required=True)
    parser.add_argument('--name', required=True)
    parser.add_argument('--results', type=Path, action='append', required=True)
    parser.add_argument('--copies', type=int, default=24)
    parser.add_argument('--busy', type=int, default=36)
    parser.add_argument('--reps', type=int, default=1)
    parser.add_argument('--deterministic', action='store_true')
    parser.add_argument('--update', action='store_true')
    parser.add_argument('--pids', type=Path, help='file that receives the pid of every process this tool starts')
    args = parser.parse_args()

    commands = [command for path in args.results for command in commands_of(path)]
    if args.deterministic:
        commands = [command[:command.index('-cpu')] + harness.QEMU_DETERMINISTIC + command[command.index('-cpu'):]
                    if 'qemu-system' in command[0] and '-icount' not in command else command for command in commands]
    env = dict(os.environ)
    env['LD_LIBRARY_PATH'] = str(harness.CACHE / 'host/usr/lib/x86_64-linux-gnu')
    pids = []
    busy = [subprocess.Popen([sys.executable, '-c', 'while True: pass'], start_new_session=True) for _ in range(args.busy)]
    pids += [process.pid for process in busy]
    seconds, loads, statuses = [], [], {}

    def run(command):
        started = time.monotonic()
        process = subprocess.Popen(command, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        pids.append(process.pid)
        status = process.wait()
        seconds.append(time.monotonic() - started)
        statuses[status] = statuses.get(status, 0) + 1

    try:
        for command in commands:
            for _ in range(args.reps):
                threads = [threading.Thread(target=run, args=(command,)) for _ in range(args.copies)]
                for thread in threads:
                    thread.start()
                loads.append(os.getloadavg()[0])
                for thread in threads:
                    thread.join()
    finally:
        for process in busy:
            process.kill()
        if args.pids:
            args.pids.write_text(''.join('%d\n' % pid for pid in pids))
    slowest = max(seconds)
    print('%d commands, %d runs at %d copies beside %d busy processes: slowest %.1f s, median %.1f s, load average up to %.0f, exit statuses %s'
          % (len(commands), len(seconds), args.copies, args.busy, slowest, sorted(seconds)[len(seconds) // 2], max(loads), statuses))
    if args.update:
        table = json.loads(SLOTS.read_text())
        entry = table[args.table].get(args.name, {'kill_multiplier': 4})
        entry.update({'slowest_loaded_run_seconds': round(slowest, 1),
                      'measured': {'date': datetime.date.today().isoformat(), 'runs': len(seconds), 'copies': args.copies,
                                   'busy_processes': args.busy, 'load_average': round(max(loads)), 'commands': len(commands), 'exit_statuses': {str(k): v for k, v in sorted(statuses.items())}}})
        table[args.table][args.name] = entry
        SLOTS.write_text(json.dumps(table, indent=2) + '\n')


if __name__ == '__main__':
    main()
