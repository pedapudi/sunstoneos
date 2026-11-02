#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Decode the DRAM bandwidth result (apps/dram-bandwidth/result.h) and check its digests.

Each pass reads from DRAM at least 5 MiB per core. The decoder recomputes the
slice digests and the per-pass digests from the fill pattern, so a pass whose
data differs from what the fill wrote is refused, and converts the tick
counts to gigabytes per second (1 GB = 10^9 bytes).
"""
import json
import re
import statistics
import struct
from pathlib import Path

MAGIC = b'SSDRAMBW'
CORES, SLICES, WARM, REPS, CONFIGS = 8, 8, 2, 8, 5
SLICE_BYTES = 5 << 20
MASKS = (0x01, 0x40, 0xc0, 0x3f, 0xff)
CLOCK = 256
HEAD = struct.Struct('<8sQ4I2I5i5i')
CONFIG = struct.Struct('<4I' + 'QQ' * REPS)
RESULT_BYTES = HEAD.size + 8 * SLICES + 2 * CLOCK + CONFIGS * CONFIG.size
NAMES = ('one A55 core', 'one A76 core', 'two A76 cores', 'six A55 cores', 'all eight cores')


def inputs():
    return []


def word(i):
    return (i * 0x9e3779b97f4a7c15 + 0x1234567) & 0xffffffffffffffff


def slice_digest(slice_index):
    import numpy as np
    base = slice_index * (SLICE_BYTES // 8)
    index = np.arange(base, base + SLICE_BYTES // 8, dtype=np.uint64)
    words = index * np.uint64(0x9e3779b97f4a7c15) + np.uint64(0x1234567)
    return int(np.bitwise_xor.reduce(words))


def decode(payload):
    if len(payload) != RESULT_BYTES:
        raise ValueError(f'DRAM bandwidth result must contain {RESULT_BYTES} bytes, not {len(payload)}')
    fields = HEAD.unpack_from(payload)
    magic, hz, slice_bytes, slices, warm, reps, readiness, _reserved = fields[:8]
    before, after = fields[8:13], fields[13:18]
    if (magic, slice_bytes, slices, warm, reps) != (MAGIC, SLICE_BYTES, SLICES, WARM, REPS) or hz != 24000000:
        raise ValueError('DRAM bandwidth workload or counter configuration differs')
    offset = HEAD.size
    fill = struct.unpack_from('<' + 'Q' * SLICES, payload, offset)
    offset += 8 * SLICES
    clocks = []
    for _ in range(2):
        clocks.append(payload[offset:offset + CLOCK].split(b'\0', 1)[0].decode('ascii').strip())
        offset += CLOCK
    expected = [slice_digest(s) for s in range(SLICES)]
    if list(fill) != expected:
        raise ValueError('the fill digests differ from the fill pattern')
    configs = []
    for c in range(CONFIGS):
        values = CONFIG.unpack_from(payload, offset + c * CONFIG.size)
        mask, cores = values[0], values[1]
        if mask != MASKS[c] or cores != bin(mask).count('1'):
            raise ValueError(f'configuration {c} core set differs')
        rates = []
        for rep in range(REPS):
            ticks, digest = values[4 + 2 * rep], values[5 + 2 * rep]
            pass_index = WARM + rep + 1
            want = 0
            for rank in range(cores):
                want ^= expected[(rank + pass_index) % SLICES]
            if digest != want:
                raise ValueError(f'configuration {c} pass {rep} digest differs from the data written')
            if not 0 < ticks < hz:
                raise ValueError(f'configuration {c} pass {rep} duration is out of range')
            rates.append(cores * SLICE_BYTES / (ticks / hz) / 1e9)
        configs.append(dict(name=NAMES[c], mask=mask, cores=cores, gb_per_s=rates,
                            median_gb_per_s=statistics.median(rates), best_gb_per_s=max(rates),
                            worst_gb_per_s=min(rates),
                            per_core_median_gb_per_s=statistics.median(rates) / cores))
    clock_pattern = r'CLOCK cpu_l_mhz=(\d+) cpu_b_mhz=(\d+) cpu_l_mv=(\d+) cpu_b_mv=(\d+) transitions=(\d+) dsu_mhz=(\d+) .*'
    for line in clocks:
        if not re.match(clock_pattern, line):
            raise ValueError('clock record is missing or unknown: ' + line)
    return dict(counter_hz=hz, slice_bytes=slice_bytes, readiness=readiness,
                millicelsius_before=dict(zip(('cpu_b', 'ddr', 'npu', 'cpu_l', 'gpu'), before)),
                millicelsius_after=dict(zip(('cpu_b', 'ddr', 'npu', 'cpu_l', 'gpu'), after)),
                clock_before=clocks[0], clock_after=clocks[1], configs=configs)


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument('result', type=Path)
    args = parser.parse_args()
    print(json.dumps(decode(args.result.read_bytes()), indent=2))
