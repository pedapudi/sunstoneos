#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Decode the DRAM scaling result (apps/dram-scaling/result.h) and check its digests.

Every variant records, for each participating core, the chunks it read in a fixed
12 ms window and the ticks from the common gate to its last chunk. The decoder
recomputes the fill digest of each slot and the digest of the chunks each core read in
the last timed window, refuses a result whose digests differ, and converts the counts to
gigabytes per second (10^9 bytes per second): a rate per core, their sum (all cores read
during the whole window apart from at most one 64 KiB step each), and the sum of the
bytes over the slowest core's time, which is the figure of the earlier benchmark.
"""
import json
import re
import statistics
import struct
from pathlib import Path

MAGIC = b'SSDRAMSC'
CORES, SLOT_BYTES, REGION_BYTES, STAGGER_WRAP = 8, 5 << 20, 3 << 20, 1 << 20
WINDOW_TICKS, WARM, REPS = 288000, 1, 3
CLOCK = 256
HEAD = struct.Struct('<8sQQ4I5i5i')
FILL = struct.Struct('<' + 'Q' * CORES)
CONFIG = struct.Struct('<4I' + 'II' * (REPS * CORES) + 'Q' * CORES)
VARIANTS = [(m, 0, 0) for m in (0x01, 0x03, 0x0f, 0x3f, 0x40, 0xc0, 0xcf, 0xff)]
for stagger in (4096, 65536, 262144):
    VARIANTS += [(m, 0, stagger) for m in (0x3f, 0xc0, 0xff)]
for mode in (1, 2, 3):
    VARIANTS += [(m, mode, 0) for m in (0x01, 0x40, 0x3f, 0xc0, 0xff)]
VARIANTS += [(m, 4, 0) for m in (0x01, 0x03, 0x0f, 0x3f, 0x40, 0xc0, 0xcf, 0xff)]
CONFIGS = len(VARIANTS)
RESULT_BYTES = HEAD.size + FILL.size + 2 * CLOCK + CONFIGS * CONFIG.size
MODE_NAMES = ('sequential 64 KiB chunks', 'sequential 4 KiB chunks', 'two streams', 'page strides', 'sequential, software prefetch')


def inputs():
    return []


def chunk_bytes(mode):
    return 65536 if mode in (0, 4) else 4096


def chunk_index(mode, j):
    """The chunk visited at step j (numpy array in, numpy array out); mirrors ds_chunk_index."""
    import numpy as np
    n = REGION_BYTES // chunk_bytes(mode)
    if mode == 2:
        return ((j >> 1) % (n // 2)) + (j & 1) * (n // 2)
    if mode == 3:
        return (j * 7) % n
    return j % n


def buffer_words():
    import numpy as np
    index = np.arange(CORES * SLOT_BYTES // 8, dtype=np.uint64)
    return index * np.uint64(0x9e3779b97f4a7c15) + np.uint64(0x1234567)


def decode(payload):
    import numpy as np
    if len(payload) != RESULT_BYTES:
        raise ValueError(f'DRAM scaling result must contain {RESULT_BYTES} bytes, not {len(payload)}')
    fields = HEAD.unpack_from(payload)
    magic, hz, base, window, warm, reps, readiness = fields[:7]
    before, after = fields[7:12], fields[12:17]
    if (magic, window, warm, reps) != (MAGIC, WINDOW_TICKS, WARM, REPS) or hz != 24000000:
        raise ValueError('DRAM scaling workload or counter configuration differs')
    offset = HEAD.size
    fill = FILL.unpack_from(payload, offset)
    offset += FILL.size
    clocks = []
    for _ in range(2):
        clocks.append(payload[offset:offset + CLOCK].split(b'\0', 1)[0].decode('ascii').strip())
        offset += CLOCK
    words = buffer_words()
    slots = words.reshape(CORES, SLOT_BYTES // 8)
    if list(fill) != [int(np.bitwise_xor.reduce(slots[c])) for c in range(CORES)]:
        raise ValueError('the fill digests differ from the fill pattern')
    configs = []
    for c in range(CONFIGS):
        values = CONFIG.unpack_from(payload, offset + c * CONFIG.size)
        mask, mode, stagger = values[:3]
        if (mask, mode, stagger) != VARIANTS[c]:
            raise ValueError(f'configuration {c} differs from the variant table')
        cores = [k for k in range(CORES) if mask >> k & 1]
        samples = values[4:4 + 2 * REPS * CORES]
        digests = values[4 + 2 * REPS * CORES:]
        if any(digests[k] for k in range(CORES) if k not in cores):
            raise ValueError(f'configuration {c} carries a digest for a core outside its set')
        cb = chunk_bytes(mode)
        n = REGION_BYTES // cb
        reps_out = []
        for rep in range(REPS):
            per_core, ticks_all, bytes_all = {}, [], 0
            for k in range(CORES):
                chunks, ticks = samples[2 * (rep * CORES + k)], samples[2 * (rep * CORES + k) + 1]
                if k not in cores:
                    if chunks or ticks:
                        raise ValueError(f'configuration {c} core {k} reports a read outside its set')
                    continue
                if chunks == 0 or not window <= ticks < 10 * window:
                    raise ValueError(f'configuration {c} rep {rep} core {k} window is out of range')
                per_core[k] = chunks * cb / (ticks / hz) / 1e9
                ticks_all.append(ticks)
                bytes_all += chunks * cb
                if rep == REPS - 1:
                    start = k * (SLOT_BYTES // 8) + ((k * stagger) % STAGGER_WRAP) // 8
                    region = words[start:start + REGION_BYTES // 8].reshape(n, cb // 8)
                    chunk_digest = np.bitwise_xor.reduce(region, axis=1)
                    visits = np.bincount(chunk_index(mode, np.arange(chunks, dtype=np.int64)), minlength=n) & 1
                    want = int(np.bitwise_xor.reduce(chunk_digest[visits == 1], initial=np.uint64(0)))
                    if digests[k] != want:
                        raise ValueError(f'configuration {c} core {k} digest differs from the data written')
            reps_out.append(dict(per_core_gb_per_s=per_core, sum_gb_per_s=sum(per_core.values()),
                                 last_finish_gb_per_s=bytes_all / (max(ticks_all) / hz) / 1e9))
        configs.append(dict(mask=mask, cores=len(cores), mode=mode, mode_name=MODE_NAMES[mode], stagger=stagger,
                            sum_gb_per_s=statistics.median(r['sum_gb_per_s'] for r in reps_out),
                            last_finish_gb_per_s=statistics.median(r['last_finish_gb_per_s'] for r in reps_out),
                            per_core_median_gb_per_s={k: statistics.median(r['per_core_gb_per_s'][k] for r in reps_out)
                                                      for k in cores},
                            reps=reps_out))
    clock_pattern = r'CLOCK cpu_l_mhz=(\d+) cpu_b_mhz=(\d+) cpu_l_mv=(\d+) cpu_b_mv=(\d+) transitions=(\d+) dsu_mhz=(\d+) .*'
    for line in clocks:
        if not re.match(clock_pattern, line):
            raise ValueError('clock record is missing or unknown: ' + line)
    return dict(counter_hz=hz, buffer_base=base, readiness=readiness,
                millicelsius_before=dict(zip(('cpu_b', 'ddr', 'npu', 'cpu_l', 'gpu'), before)),
                millicelsius_after=dict(zip(('cpu_b', 'ddr', 'npu', 'cpu_l', 'gpu'), after)),
                clock_before=clocks[0], clock_after=clocks[1], configs=configs)


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument('result', type=Path)
    args = parser.parse_args()
    print(json.dumps(decode(args.result.read_bytes()), indent=2))
