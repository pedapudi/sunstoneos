#!/usr/bin/env python3
"""Decode the result of the NPU job timing application (timing.h, struct npu_timing_result); this module never opens a board."""
import struct

MAGIC = 0x544c534547494d54
BYTES = 1728
GRAPHS = 16
TICKS_PER_US = 24


def decode(raw):
    if len(raw) != BYTES:
        raise ValueError('The result must hold exactly %d bytes.' % BYTES)
    magic, version, size, count, clock0, clock1, open_ticks, _ = struct.unpack_from('<8Q', raw, 0)
    if (magic, version, size) != (MAGIC, 2, BYTES) or not 0 < count <= GRAPHS:
        raise ValueError('The result header differs (a failure record, or another image).')
    us = lambda t: t / TICKS_PER_US
    graphs = []
    for g in range(count):
        (jobs, lo, p50, p99, hi, total, depth, piped, out_bytes, differ, in_bytes, waits_busy) = struct.unpack_from('<12Q', raw, 64 + 96 * g)
        (wait_ticks,) = struct.unpack_from('<Q', raw, 64 + 96 * GRAPHS + 8 * g)
        graphs.append({'graph': g, 'jobs': jobs, 'min_us': us(lo), 'p50_us': us(p50), 'p99_us': us(p99), 'max_us': us(hi),
                       'mean_us': us(total) / jobs if jobs else None, 'depth': depth,
                       'pipelined_us_per_job': us(piped) / jobs if depth and jobs else None,
                       'input_bytes': in_bytes, 'output_bytes': out_bytes, 'differing_bytes': differ,
                       'idle_waits': waits_busy & 0xffffffff, 'idle_busy_bits': '0x%08x' % (waits_busy >> 32),
                       'idle_wait_max_us': us(wait_ticks & 0xffffffff), 'idle_wait_total_us': us(wait_ticks >> 32)})
    return {'count': count, 'open_us': us(open_ticks),
            'clock': {'little_mhz': clock0 & 0xffff, 'big_mhz': clock0 >> 16 & 0xffff, 'core': clock0 >> 32 & 0xff,
                      'transitions': clock0 >> 40 & 0xff, 'little_mv': clock1 & 0xffff, 'big_mv': clock1 >> 16 & 0xffff,
                      'dsu_mhz': clock1 >> 32 & 0xffff},
            'graphs': graphs, 'passed': all(r['differing_bytes'] == 0 and r['output_bytes'] for r in graphs)}


if __name__ == '__main__':
    import json, sys
    print(json.dumps(decode(open(sys.argv[1], 'rb').read()), indent=1))
