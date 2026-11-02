#!/usr/bin/env python3
"""Bound the work of every interrupt-masked span of the owner scheduler on the host.

QEMU cannot attribute time to one core, and the cache-line transfers that
dominate the A733 spans do not exist there. The two models (cmake/latency.cmake,
checks/latency-ops.h) run the CPU latency workload on eight cores under
the owner scheduler and report each masked span as the number of scheduler
atomic operations its core executed: stores and read-modify-writes in one image,
loads in the other. A span that waited for another core is not counted. The
check prints the largest span at every site and fails when a span exceeds its
bound: a path that scans every task slot, or touches many lines, shows here
deterministically."""
import argparse
import bisect
import json
from pathlib import Path
import re
import subprocess

from harness import CACHE, environment

WRITES, LOADS = 24, 100


def symbols(image, nm):
    rows = []
    for line in subprocess.check_output([nm, '-n', str(image)], text=True).splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in 'tT':
            rows.append((int(parts[0], 16), parts[2]))
    rows.sort()
    keys = [a for a, _ in rows]

    def name(address):
        i = bisect.bisect_right(keys, address) - 1
        return '%s+%#x' % (rows[i][1], address - rows[i][0])
    return name


def spans(image, qemu, nm):
    run = subprocess.run([str(qemu), '-M', 'virt,gic-version=3,virtualization=off', '-cpu', 'cortex-a76',
                          '-smp', '8', '-m', '2G', '-nographic', '-monitor', 'none',
                          '-semihosting-config', 'enable=on,target=native', '-accel', 'tcg,thread=single',
                          '-icount', 'shift=0,sleep=off', '-kernel', str(image)],
                         text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=environment(), timeout=120)
    assert run.returncode == 0 and 'PASS latency model' in run.stdout, run.stdout[-1500:]
    name = symbols(image, nm)
    core, best = None, {}
    for line in run.stdout.splitlines():
        m = re.match(r'core=(0x\w+) sweeps', line)
        if m:
            core = int(m[1], 16)
        m = re.match(r'site_phase=(0x\w+) kind=(0x\w+) slot=\w+ ticks=(0x\w+) start=\w+ enter_site=(0x\w+) exit_site=(0x\w+)', line)
        if m and int(m[2], 16) == 0:
            ticks = int(m[3], 16) - 1  # the recorder adds one tick to every span
            key = (name(int(m[4], 16)), name(int(m[5], 16)))
            if ticks > best.get(key, (-1, 0))[0]:
                best[key] = (ticks, core)
    assert best, 'no masked span was recorded'
    return run.stdout, best


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--qemu', type=Path, default=CACHE / 'host/usr/bin/qemu-system-aarch64')
    parser.add_argument('--nm', type=Path, default=Path('/usr/lib/llvm-21/bin/llvm-nm'))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    report = {}
    for kind, target, bound in (('writes', 'latency-partitioned-writes-model', WRITES),
                                ('loads', 'latency-partitioned-loads-model', LOADS)):
        image = args.build_dir / (target + '.elf')
        log, best = spans(image, args.qemu, args.nm)
        (args.output / (kind + '.log')).write_text(log)
        rows = sorted(((v[0], v[1], k) for k, v in best.items()), reverse=True)
        report[kind] = [dict(ops=o, core=c, enter=k[0], exit=k[1]) for o, c, k in rows]
        for ops, core, (enter, leave) in rows[:6]:
            print('%-6s %4d  core %d  %s -> %s' % (kind, ops, core, enter, leave))
        assert rows[0][0] <= bound, \
            'a masked span executes %d atomic %s, above %d: %s' % (rows[0][0], kind, bound, rows[0][2])
    # The board image's PMU capture: the model must run it at EL0 and EL1 without a fault and count.
    image = args.build_dir / 'latency-partitioned-pmu-model.elf'
    log = subprocess.run([str(args.qemu), '-M', 'virt,gic-version=3,virtualization=off', '-cpu', 'cortex-a76',
                          '-smp', '8', '-m', '2G', '-nographic', '-monitor', 'none',
                          '-semihosting-config', 'enable=on,target=native', '-accel', 'tcg,thread=single',
                          '-icount', 'shift=0,sleep=off', '-kernel', str(image)],
                         text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=environment(), timeout=120)
    assert log.returncode == 0 and 'PASS latency model' in log.stdout, log.stdout[-1500:]
    counted = [int(m[1], 16) for m in re.finditer(r'site_pmu instructions=(0x\w+)', log.stdout)]
    assert any(counted), 'the PMU capture counted no instructions'
    (args.output / 'pmu.log').write_text(log.stdout)
    (args.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS owner scheduler masked spans: at most %d atomic writes and %d atomic loads per span' % (WRITES, LOADS))


if __name__ == '__main__':
    main()
