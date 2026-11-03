#!/usr/bin/env python3
"""Tabulate the object lock trace of a mimus soak entry: who waits for, and who holds, the kernel object locks.

  sched-lock-trace-report.py ENTRY [--image ELF] [--top N]

The soak output of an image built with SUNSTONE_OBJECT_TRACE and SUNSTONE_MIMUS_OBJECT_TRACE has OBJ lines (per core,
per object and call site: acquisitions, wait and hold of the task-side lock T and the interrupt-side lock I, in counter
ticks of 41.67 ns, and the waits over 5 us) and OBJPAIR lines (a wait over 5 us charged to the acquisition that held
the lock when the waiter arrived). Addresses are resolved against the image's symbols. The report prints the callers
ranked by wait, the objects ranked by wait and hold, and the share of the long waits whose holder worked on another
object than the waiter.
"""
import argparse
import bisect
import collections
import re
import subprocess
import sys
from pathlib import Path

QUEUE = Path('~/.cache/sunstoneos/board-queue').expanduser()
TICK_US = 1 / 24.0


class Symbols:
    def __init__(self, image):
        self.addresses, self.names = [], []
        if image:
            out = subprocess.run(['llvm-nm-21', '-n', '--defined-only', str(image)], capture_output=True, text=True).stdout
            for line in out.splitlines():
                parts = line.split()
                if len(parts) == 3 and parts[1] in 'tTdDbBrR':
                    self.addresses.append(int(parts[0], 16))
                    self.names.append(parts[2])

    def name(self, address):
        if not address:
            return '-'
        i = bisect.bisect_right(self.addresses, address) - 1
        if i < 0 or address - self.addresses[i] > 0x10000:
            return '%#x' % address
        offset = address - self.addresses[i]
        return '%s+%#x' % (self.names[i], offset) if offset else self.names[i]


def entry_dir(entry):
    path = Path(entry)
    return path if path.is_dir() else QUEUE / 'entries' / entry


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('entry')
    parser.add_argument('--image', type=Path)
    parser.add_argument('--top', type=int, default=12)
    args = parser.parse_args()
    directory = entry_dir(args.entry)
    image = args.image or directory / 'image.elf'
    symbols = Symbols(image)
    text = (directory / 'run' / 'output.bin').read_bytes().decode('utf-8', 'replace')
    rows, pairs = [], []
    for line in text.splitlines():
        m = re.match(r'OBJ core=(\d+) object=(\w+) site=(\w+) T:n=(\d+) wsum=(\d+) wmax=(\d+) hsum=(\d+) hmax=(\d+) wlong=(\d+) '
                     r'I:n=(\d+) wsum=(\d+) wmax=(\d+) hsum=(\d+) hmax=(\d+) wlong=(\d+)', line)
        if m:
            v = [int(m.group(1))] + [int(m.group(i), 16) for i in (2, 3)] + [int(m.group(i)) for i in range(4, 16)]
            rows.append(v)
            continue
        m = re.match(r'OBJPAIR core=(\d+) lock=(\d+) object=(\w+) site=(\w+) holder_core=(\d+) holder_object=(\w+) holder_site=(\w+) '
                     r'n=(\d+) wsum=(\d+) wmax=(\d+)', line)
        if m:
            pairs.append((int(m.group(1)), int(m.group(2)), int(m.group(3), 16), int(m.group(4), 16), int(m.group(5)),
                          int(m.group(6), 16), int(m.group(7), 16), int(m.group(8)), int(m.group(9)), int(m.group(10))))
    if not rows and not pairs:
        print('no trace lines in', directory)
        return 1
    # rows: core, object, site, Tn, Tw, Twmax, Th, Thmax, Tlong, In, Iw, Iwmax, Ih, Ihmax, Ilong
    by_site = collections.defaultdict(lambda: [0] * 12)
    by_object = collections.defaultdict(lambda: [0] * 8)
    for r in rows:
        core, obj, site = r[0], r[1], r[2]
        a = by_site[(obj, site)]
        for k, i in enumerate(range(3, 15)):
            a[k] = max(a[k], r[i]) if k in (2, 4, 8, 10) else a[k] + r[i]
        b = by_object[obj]
        b[0] += r[3] + r[9]
        b[1] += r[4] + r[10]
        b[2] += r[6] + r[12]
        b[3] += r[8] + r[14]
        b[4] = max(b[4], r[5], r[11])
        b[5] = max(b[5], r[7], r[13])
        b[6] += 1
    print('callers ranked by total wait (ms) on the task-side (T) and interrupt-side (I) locks; ticks of 41.67 ns -> us')
    print('%-6s %-46s %-24s %9s %9s %8s %8s %8s %8s %8s' % ('rank', 'caller', 'object', 'n', 'wait ms', 'wmax us', 'hold ms', 'hmax us', 'long', ''))
    ranked = sorted(by_site.items(), key=lambda kv: -(kv[1][1] + kv[1][7]))[:args.top]
    for i, ((obj, site), a) in enumerate(ranked):
        n = a[0] + a[6]
        print('%-6d %-46s %-24s %9d %9.2f %8.1f %8.2f %8.1f %8d' % (i + 1, symbols.name(site), symbols.name(obj) if obj else 'public', n,
              (a[1] + a[7]) * TICK_US / 1000, max(a[2], a[8]) * TICK_US, (a[3] + a[9]) * TICK_US / 1000,
              max(a[4], a[10]) * TICK_US, a[5] + a[11]))
    print()
    print('objects ranked by total wait')
    print('%-6s %-30s %10s %9s %9s %8s' % ('rank', 'object', 'acquires', 'wait ms', 'hold ms', 'long'))
    for i, (obj, b) in enumerate(sorted(by_object.items(), key=lambda kv: -kv[1][1])[:args.top]):
        print('%-6d %-30s %10d %9.2f %9.2f %8d' % (i + 1, symbols.name(obj) if obj else 'public critical sections', b[0], b[1] * TICK_US / 1000,
                                                  b[2] * TICK_US / 1000, b[3]))
    total = sum(p[7] for p in pairs)
    same = sum(p[7] for p in pairs if p[2] == p[5])
    public_waiter = sum(p[7] for p in pairs if p[2] == 0)
    public_holder = sum(p[7] for p in pairs if p[5] == 0)
    print()
    print('waits over 5 us charged to the holder at arrival: %d (T %d, I %d)' % (total, sum(p[7] for p in pairs if p[1] == 0),
                                                                              sum(p[7] for p in pairs if p[1] == 1)))
    if total:
        print('  holder worked on the same object as the waiter: %d (%.0f%%)' % (same, 100.0 * same / total))
        print('  holder worked on another object: %d (%.0f%%)' % (total - same, 100.0 * (total - same) / total))
        print('  waiter in a public critical section: %d, holder in one: %d' % (public_waiter, public_holder))
    print()
    print('waiter -> holder pairs ranked by waits over 5 us')
    print('%-5s %-4s %-36s %-36s %7s %9s' % ('lock', 'core', 'waiter caller', 'holder caller', 'n', 'wait ms'))
    for p in sorted(pairs, key=lambda p: -p[8])[:args.top]:
        print('%-5s %-4d %-36s %-36s %7d %9.2f' % ('T' if p[1] == 0 else 'I', p[0], symbols.name(p[3]), symbols.name(p[6]), p[7], p[8] * TICK_US / 1000))


if __name__ == '__main__':
    sys.exit(main())
