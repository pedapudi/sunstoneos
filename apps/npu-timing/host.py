#!/usr/bin/env python3
"""Decode the result of the NPU job timing application (timing.h, struct npu_timing_result); this module never opens a board."""
import struct

MAGIC = 0x544c534547494d54
BYTES = 27072
PREFIXES = 512
NPU_MHZ = 852
GRAPHS = 16
TICKS_PER_US = 24


def decode(raw):
    if len(raw) != BYTES:
        raise ValueError('The result must hold exactly %d bytes.' % BYTES)
    magic, version, size, count, clock0, clock1, open_ticks, _ = struct.unpack_from('<8Q', raw, 0)
    if (magic, version, size) != (MAGIC, 4, BYTES) or not 0 < count <= GRAPHS:
        raise ValueError('The result header differs (a failure record, or another image).')
    us = lambda t: t / TICKS_PER_US
    graphs = []
    for g in range(count):
        (jobs, lo, p50, p99, hi, total, depth, piped, out_bytes, differ, in_bytes, waits_busy) = struct.unpack_from('<12Q', raw, 64 + 96 * g)
        (wait_ticks,) = struct.unpack_from('<Q', raw, 64 + 96 * GRAPHS + 8 * g)
        # The NPU counters over the warm-up and single jobs: bus reads and writes count units of 8 bytes.
        (t_jobs, read8, write8, cycles, idle) = struct.unpack_from('<5Q', raw, 64 + 104 * GRAPHS + 40 * g)
        per = lambda v: v / t_jobs if t_jobs else None
        graphs.append({'graph': g, 'jobs': jobs, 'min_us': us(lo), 'p50_us': us(p50), 'p99_us': us(p99), 'max_us': us(hi),
                       'mean_us': us(total) / jobs if jobs else None, 'depth': depth,
                       'pipelined_us_per_job': us(piped) / jobs if depth and jobs else None,
                       'input_bytes': in_bytes, 'output_bytes': out_bytes, 'differing_bytes': differ,
                       'idle_waits': waits_busy & 0xffffffff, 'idle_busy_bits': '0x%08x' % (waits_busy >> 32),
                       'idle_wait_max_us': us(wait_ticks & 0xffffffff), 'idle_wait_total_us': us(wait_ticks >> 32),
                       'counted_jobs': t_jobs, 'read_bytes_per_job': per(read8 * 8), 'write_bytes_per_job': per(write8 * 8),
                       'cycles_per_job': per(cycles), 'idle_cycles_per_job': per(idle)})
    # The prefix pass: operation k's time is prefix k's less prefix k - 1's (prefix 0 is no job at all), in NPU cycles when the
    # counters were read and in counter ticks of the medians otherwise; its traffic likewise.
    base = 64 + 144 * GRAPHS
    for g in range(count):
        first, n = struct.unpack_from('<I', raw, base + 4 * g)[0], struct.unpack_from('<I', raw, base + 64 + 4 * g)[0]
        if not n:
            continue
        rows, before = [], None
        for k in range(n):
            p50, lo, jobs, ops, cut, _, cycles, read8, write8 = struct.unpack_from('<6I3Q', raw, base + 128 + 48 * (first + k))
            row = {'operations': k + 1, 'p50_us': us(p50), 'min_us': us(lo), 'jobs': jobs, 'slot_operations': ops,
                   'cut_bytes': cut, 'cycles': cycles / jobs if jobs else None,
                   'read_counts': read8 / jobs if jobs else None, 'write_counts': write8 / jobs if jobs else None}
            prev = before or {'p50_us': 0, 'cycles': 0, 'read_counts': 0, 'write_counts': 0}
            row['op_us'] = row['p50_us'] - prev['p50_us']
            if row['cycles'] is not None and prev['cycles'] is not None:
                row['op_cycles'] = row['cycles'] - prev['cycles']
                row['op_read_counts'] = row['read_counts'] - prev['read_counts']
                row['op_write_counts'] = row['write_counts'] - prev['write_counts']
            rows.append(row); before = row
        graphs[g]['prefixes'] = rows
    return {'count': count, 'open_us': us(open_ticks),
            'clock': {'little_mhz': clock0 & 0xffff, 'big_mhz': clock0 >> 16 & 0xffff, 'core': clock0 >> 32 & 0xff,
                      'transitions': clock0 >> 40 & 0xff, 'little_mv': clock1 & 0xffff, 'big_mv': clock1 >> 16 & 0xffff,
                      'dsu_mhz': clock1 >> 32 & 0xffff},
            'graphs': graphs, 'passed': all(r['differing_bytes'] == 0 and r['output_bytes'] for r in graphs)}


if __name__ == '__main__':
    import json, sys
    print(json.dumps(decode(open(sys.argv[1], 'rb').read()), indent=1))
