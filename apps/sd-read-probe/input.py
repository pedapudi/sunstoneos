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
    fat_at = sector_at + 512 * MAX_CAPTURE
    if len(raw) >= fat_at + 4952:
        f = raw[fat_at:fat_at + 4952]
        names = ('mount_status', 'free_status', 'list_status')
        fat = dict(zip(names, struct.unpack_from('<3i', f, 0)))
        keys = ('sectors_per_cluster', 'reserved_sectors', 'fats', 'fat_sectors', 'root_cluster', 'clusters',
                'volume_id', 'hint_free', 'hint_next', 'counted_free')
        fat.update(zip(keys, struct.unpack_from('<10I', f, 12)))
        fat['data_start'], fat['partition_first'], fat['partition_sectors'] = struct.unpack_from('<3Q', f, 56)
        count, seen = struct.unpack_from('<2I', f, 80)
        fat['entries_seen'] = seen
        fat['entries'] = []
        for i in range(count):
            e = f[88 + 76 * i:88 + 76 * (i + 1)]
            size, first = struct.unpack_from('<2I', e, 64)
            fat['entries'].append({'name': e[:64].split(b'\0')[0].decode('ascii', 'replace'), 'size': size,
                                   'first_cluster': first, 'attributes': e[72], 'long_name': e[73], 'truncated': e[74]})
        out['fat'] = fat
    region_at = fat_at + 4952
    if len(raw) >= region_at + 2424:
        g = raw[region_at:region_at + 2424]
        r = dict(zip(('status', 'ledger_status', 'build_status_owner', 'build_status_other', 'build_status_view',
                      'file_status', 'load_status'), struct.unpack_from('<7i', g, 0)))
        flags, ncases, npasses, pmu = struct.unpack_from('<4I', g, 32)
        names = {1: 'prepared', 2: 'isolation', 4: 'passes', 8: 'file-absent', 16: 'loaded', 32: 'digest-ok', 64: 'upper-mapped'}
        r['flags'] = [n for b, n in names.items() if flags & b]
        r['pmu_ok'] = pmu
        r['tables'] = struct.unpack_from('<3I', g, 48)
        r['asid'] = struct.unpack_from('<3I', g, 60)
        (r['base'], r['bytes'], r['free_before'], r['free_after'], r['file_bytes'], r['load_ticks'],
         r['load_requests']) = struct.unpack_from('<7Q', g, 72)
        r['expected'], r['digest'] = g[128:160].hex(), g[160:192].hex()
        r['el1_xor_strided'], r['el0_xor_owner'] = struct.unpack_from('<Q', g, 208)[0], struct.unpack_from('<Q', g, 200)[0]
        if r['load_ticks']:
            r['load_MBps'] = r['file_bytes'] / (r['load_ticks'] / 24)
            r['load_seconds'] = r['load_ticks'] / 24e6
        r['cases'] = []
        for i in range(ncases):
            space, op, ec, _, addr, esr, far, res = struct.unpack_from('<4I4Q', g, 216 + 48 * i)
            r['cases'].append({'space': ['owner', 'other', 'view'][space], 'op': ['read', 'write', 'control'][op],
                               'address': hex(addr), 'ec': hex(ec), 'esr': hex(esr), 'far': hex(far),
                               'dfsc': hex(esr & 63), 'write_not_read': (esr >> 6) & 1, 'result': hex(res)})
        r['passes'] = []
        for i in range(npasses):
            kind, _, nbytes, ticks, cycles, c0, c1, c2, c3, xv = struct.unpack_from('<2I8Q', g, 984 + 72 * i)
            seconds = ticks / 24e6
            r['passes'].append({'kind': ['control-40MiB', 'gib-blocks', 'mib2-blocks'][kind], 'GBps': nbytes / seconds / 1e9,
                                'cycles_per_byte': cycles / nbytes, 'cpu_MHz': cycles / seconds / 1e6,
                                'tlb_l1_refill': c0, 'tlb_l2_refill': c1, 'dtlb_walk': c2, 'l1d_refill': c3,
                                'xor': hex(xv), 'tlb_l1_per_MiB': c0 / (nbytes / 2**20)})
        out['region'] = r
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
    i.add_argument('--fat-first', type=int, default=0, help='first sector of a FAT32 partition to inspect')
    i.add_argument('--fat-sectors', type=int, default=0, help='sectors of that partition; zero: no inspection')
    i.add_argument('--region-bytes', type=int, default=0, help='take a region of this size from upper memory')
    i.add_argument('--load', help='root-directory path of a file to load into the region')
    i.add_argument('--sha256', help='its SHA-256 (hex)')
    d = sub.add_parser('decode')
    d.add_argument('--report', type=Path, required=True)
    d.add_argument('--blocks', type=int, default=256)
    a = p.parse_args()
    if a.cmd == 'input':
        name = (a.load or '').encode()
        digest = bytes.fromhex(a.sha256) if a.sha256 else bytes(32)
        if len(name) > 47 or len(digest) != 32 or (a.load and not a.sha256):
            raise SystemExit('a file name of at most 47 bytes needs its 32-byte SHA-256')
        raw = struct.pack('<Q4I6I2Q', INPUT_MAGIC, a.reads, a.blocks, a.first, len(a.capture),
                          *(list(a.capture) + [0] * (MAX_CAPTURE - len(a.capture))), a.fat_first, a.fat_sectors)
        raw += struct.pack('<QII48s32s', a.region_bytes, 1 if a.load else 0, 0, name, digest)
        assert len(raw) == 160
        with a.output.open('xb') as f:
            f.write(raw)
        print(a.output)
    else:
        out, _ = decode(a.report.read_bytes(), a.blocks)
        print(json.dumps(out, indent=1))


if __name__ == '__main__':
    main()
