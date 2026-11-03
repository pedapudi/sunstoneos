#!/usr/bin/env python3
"""Arm-to-return time of the returning OS runs in the board queue's evidence, per application.

Each run's return report (return-report.bin, word 4) holds the diagnostic deadline in counter ticks, which is the
arming time plus the image's lifetime (the watchdog marker in image.elf). The breadcrumbs (breadcrumbs.json) give the
time of the RETURN_ROM stage on the same counter. Their difference is the time from the launcher arming the board to the
ROM return: EL3 startup, OS setup, the application's run and the return. That total is dominated by the application's
own work (a 512 MiB file write, a 1 GiB heap fill, a recognition), so it cannot bound the setup. Where an application's
result carries its own elapsed time from inside main (storage-write: total_ticks, taken after the mount), the difference is the
overhead outside the application: startup, setup and return. tools/session_lifetime.py takes its allowance from that
difference. Prints the totals per application, the pooled maximum, 99th percentile and count, and the overhead.
Usage: session_setup_time.py [--entries DIR] [--since TIMESTAMP_NS_PREFIX]
"""
import argparse
import collections
import importlib.util
import json
import os
from pathlib import Path
import statistics
import struct

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def measure(entries, since=''):
    host = load('setup_time_host', ROOT / 'apps/os/host.py')
    table = collections.defaultdict(list)
    for name in sorted(os.listdir(entries)):
        if name < since:
            continue
        run = Path(entries) / name / 'run'
        try:
            crumbs = json.loads((run / 'breadcrumbs.json').read_text())
            result = json.loads((run / 'result.json').read_text())
            report = (run / 'return-report.bin').read_bytes()
            lifetime = host.watchdog_marker(run / 'image.elf')['session_seconds']
            manifest = host.image_manifest(run / 'image.elf')['manifest']
        except (OSError, ValueError, KeyError, struct.error):
            continue
        if result.get('outcome') not in ('returned', 'failure-return') or crumbs['last']['name'] != 'RETURN_ROM':
            continue
        deadline_s = struct.unpack_from('<Q', report, 32)[0] / 24e6
        table[manifest['name']].append(crumbs['last']['seconds_since_power_on'] - (deadline_s - lifetime))
    return table


def percentile(values, fraction):
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(fraction * len(ordered)))]


def overhead(entries, since=''):
    """(arm-to-return minus the application's own elapsed time) for storage-write runs that report it."""
    host = load('setup_time_host', ROOT / 'apps/os/host.py')
    decoder = load('setup_time_storage_write', ROOT / 'apps/storage-write-test/host.py')
    found = []
    for name in sorted(os.listdir(entries)):
        if name < since:
            continue
        run = Path(entries) / name / 'run'
        try:
            crumbs = json.loads((run / 'breadcrumbs.json').read_text())
            report = (run / 'return-report.bin').read_bytes()
            lifetime = host.watchdog_marker(run / 'image.elf')['session_seconds']
            if host.image_manifest(run / 'image.elf')['manifest']['name'] != 'storage-write' or crumbs['last']['name'] != 'RETURN_ROM':
                continue
            result = decoder.decode((run / 'output.bin').read_bytes())
        except (OSError, ValueError, KeyError, struct.error):
            continue
        # A run that failed before the application's timer started (status 1: the card's bounds) spent that time in
        # application work before main's first measurement, so it does not measure the overhead.
        if not result['total_ticks'] or result['status'] == 1:
            continue
        total = crumbs['last']['seconds_since_power_on'] - (struct.unpack_from('<Q', report, 32)[0] / 24e6 - lifetime)
        found.append(total - result['total_ticks'] / 24e6)
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--entries', type=Path, default=Path.home() / '.cache/sunstoneos/board-queue/entries')
    parser.add_argument('--since', default='')
    args = parser.parse_args()
    table = measure(args.entries, args.since)
    for name, values in sorted(table.items()):
        print('%-16s runs %3d  min %6.2f  median %6.2f  max %6.2f s' % (name, len(values), min(values), statistics.median(values), max(values)))
    pooled = [v for values in table.values() for v in values]
    print('all applications: n %d  max %.2f  p99 %.2f s (application work included)' % (len(pooled), max(pooled), percentile(pooled, .99)))
    outside = overhead(args.entries, args.since)
    if outside:
        print('overhead outside the application (storage-write, own timer subtracted): n %d  min %.2f  max %.2f s' % (len(outside), min(outside), max(outside)))


if __name__ == '__main__':
    main()
