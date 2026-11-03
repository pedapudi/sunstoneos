#!/usr/bin/env python3
"""Pool the task-path histograms of mimus soak entries and print the latency table.

  sched-task-report.py ENTRY... [--queue DIR] [--json]

Each ENTRY is a queue entry id (or an entry directory). The soak output lists, per core, the nonzero bins of the
task path's latency (kind 0), jitter (kind 1), handler entry delay (kind 2) and handler-to-task gap (kind 3)
histograms; a bin is four counter ticks (166.67 ns) and the last bin takes the rest. Percentiles are the upper edge of
the bin that holds the rank, as in the soak output. Cores 1, 2 and 3 run the task path at 1, 2 and 4 kHz.
"""
import argparse
import collections
import json
import re
import sys
from pathlib import Path

QUEUE = Path('~/.cache/sunstoneos/board-queue').expanduser()
BIN_US = 4 / 24.0
RATES = {1: 1000, 2: 2000, 3: 4000}
KINDS = ('latency', 'jitter', 'entry', 'gap')


def entry_text(entry, queue):
    path = Path(entry)
    if not path.is_dir():
        path = queue / 'entries' / entry
    return (path / 'run' / 'output.bin').read_bytes().decode('utf-8', 'replace')


def parse(text):
    hist = collections.defaultdict(lambda: collections.Counter())
    maxima = {}
    for line in text.splitlines():
        m = re.match(r'HIST path=1 core=(\d+) kind=(\d) (.*)', line)
        if m:
            core, kind = int(m.group(1)), int(m.group(2))
            for item in m.group(3).split():
                b, n = item.split(':')
                hist[(core, kind)][int(b)] += int(n)
            continue
        m = re.match(r'TASKPATH core=(\d+) rate_hz=(\d+) n=(\d+) .*lat_max_ns=(\d+) lat_p9999_ns=(\d+) jit_max_ns=(\d+)', line)
        if m:
            core = int(m.group(1))
            maxima[(core, 0)] = int(m.group(4)) / 1000.0
            maxima[(core, 1)] = int(m.group(6)) / 1000.0
    return hist, maxima


def quantile(counter, fraction):
    total = sum(counter.values())
    if not total:
        return 0.0
    need = -(-total * fraction // 1)
    need = max(1, int(-(-total * fraction // 1)))
    seen = 0
    for b in sorted(counter):
        seen += counter[b]
        if seen >= need:
            return (b + 1) * BIN_US
    return 0.0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('entries', nargs='+')
    parser.add_argument('--queue', type=Path, default=QUEUE)
    parser.add_argument('--json', action='store_true')
    args = parser.parse_args()
    pooled = collections.defaultdict(collections.Counter)
    maximum = {}
    for entry in args.entries:
        hist, maxima = parse(entry_text(entry, args.queue))
        for key, counter in hist.items():
            pooled[key].update(counter)
        for key, value in maxima.items():
            maximum[key] = max(maximum.get(key, 0.0), value)
    rows = []
    for core in (1, 2, 3):
        for kind in range(4):
            counter = pooled.get((core, kind))
            if not counter:
                continue
            total = sum(counter.values())
            over = sum(n for b, n in counter.items() if b * BIN_US >= 10.0)
            top = max(counter) if counter else 0
            row = {'rate_hz': RATES[core], 'kind': KINDS[kind], 'n': total, 'p50': quantile(counter, 0.5),
                   'p99': quantile(counter, 0.99), 'p999': quantile(counter, 0.999), 'p9999': quantile(counter, 0.9999),
                   'max': maximum.get((core, kind), (top + 1) * BIN_US), 'over10us': over}
            rows.append(row)
    if args.json:
        json.dump(rows, sys.stdout, indent=1)
        return
    print('%-7s %-8s %9s %7s %7s %7s %8s %8s %9s' % ('rate', 'kind', 'n', 'p50', 'p99', 'p99.9', 'p99.99', 'max', '>=10us'))
    for r in rows:
        print('%-7d %-8s %9d %7.2f %7.2f %7.2f %8.2f %8.2f %9d' % (r['rate_hz'], r['kind'], r['n'], r['p50'], r['p99'], r['p999'],
                                                              r['p9999'], r['max'], r['over10us']))


if __name__ == '__main__':
    main()
