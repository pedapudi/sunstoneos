#!/usr/bin/env python3
"""Pack the input of the read-only microSD probe, or decode its report; no device access."""
import argparse
import json
from pathlib import Path
import struct
import sys

INPUT_MAGIC = 0x3144524450445353
REPORT_MAGIC = 0x3154505250445353
MAX_READS, MAX_CAPTURE, TIMED = 256, 6, 32
REPORT = struct.Struct('<2Q6Q2Q4Q4Q3Q2Q2Q16Q16Q3Q')
HEAD = struct.calcsize('<2Q6Q2Q4Q4Q3Q2Q2Q16Q16Q3Q')


def decode(raw, plan_blocks):
    if len(raw) < HEAD:
        raise SystemExit('report too short')
    fields = REPORT.unpack_from(raw)
    names = ('magic', 'version', 'stage', 'reason', 'done', 'planned', 'identified_us', 'elapsed_us',
             'window_ticks', 'bytes')
    out = dict(zip(names, fields[:10]))
    if out['magic'] != REPORT_MAGIC:
        raise SystemExit('not a read-only SD probe report')
    cid, csd = fields[10:14], fields[14:18]
    out.update(cid=''.join(f'{w:08x}' for w in cid), csd=''.join(f'{w:08x}' for w in csd),
               ocr=fields[18], rca=fields[19], sectors=fields[20], scr=[fields[21], fields[22]],
               mode=fields[23], card_hz=fields[24],
               baseline_changed=[i for i in range(16) if fields[25 + i] != fields[41 + i]],
               clock_off=fields[57], pmic_parked=fields[58], captured=fields[59])
    base = REPORT.size
    elapsed = struct.unpack_from(f'<{MAX_READS}I', raw, base)
    crc = struct.unpack_from(f'<{MAX_READS}I', raw, base + 4 * MAX_READS)
    timing_at = base + 8 * MAX_READS
    timing = [struct.unpack_from('<6I', raw, timing_at + 24 * i) for i in range(TIMED)]
    sector_at = timing_at + 24 * TIMED
    sectors = [raw[sector_at + 512 * i: sector_at + 512 * (i + 1)] for i in range(MAX_CAPTURE)]
    done = out['done']
    out['elapsed_us'] = out['elapsed_us']
    out['reads'] = [{'elapsed_us': elapsed[i], 'crc32': crc[i]} for i in range(done)]
    if done:
        total_us = out['window_ticks'] / 24
        out['sustained_MBps'] = out['bytes'] / total_us if total_us else None
        out['per_read_MBps'] = sorted(plan_blocks * 512 / elapsed[i] for i in range(done) if elapsed[i])
    out['timing'] = timing[:min(done, TIMED)]
    out['sectors'] = [s.hex() for s in sectors[:out['captured']]]
    return out, sectors[:out['captured']]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest='cmd', required=True)
    i = sub.add_parser('input')
    i.add_argument('--output', type=Path, required=True)
    i.add_argument('--reads', type=int, default=160)
    i.add_argument('--blocks', type=int, default=256)
    i.add_argument('--first', type=int, default=0)
    i.add_argument('--capture', type=int, nargs='*', default=[])
    d = sub.add_parser('decode')
    d.add_argument('--report', type=Path, required=True)
    d.add_argument('--blocks', type=int, default=256)
    a = p.parse_args()
    if a.cmd == 'input':
        raw = struct.pack('<Q4I6I', INPUT_MAGIC, a.reads, a.blocks, a.first, len(a.capture),
                          *(list(a.capture) + [0] * (MAX_CAPTURE - len(a.capture))))
        assert len(raw) == 48
        with a.output.open('xb') as f:
            f.write(raw)
        print(a.output)
    else:
        out, _ = decode(a.report.read_bytes(), a.blocks)
        print(json.dumps(out, indent=1))


if __name__ == '__main__':
    main()
