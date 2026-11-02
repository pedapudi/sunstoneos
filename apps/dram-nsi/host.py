#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Decode the DRAM interconnect study result (apps/dram-nsi/result.h) and check its digests.

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

MAGIC = b'SSDRAMNS'
CORES, SLOT_BYTES, REGION_BYTES, STAGGER_WRAP = 8, 5 << 20, 3 << 20, 1 << 20
WINDOW_TICKS, WARM, REPS = 288000, 1, 3
SHARED_CHUNKS = 2048
CLOCK = 256
HEAD = struct.Struct('<8sQQ4I5i5i')
FILL = struct.Struct('<' + 'Q' * CORES)
CONFIG = struct.Struct('<4I' + 'II' * (REPS * CORES) + 'I' * (7 * CORES) + 'Q' * CORES)
WORDS, EXTRA, PHASES = 9, 13, 2
PROBE = struct.Struct('<' + 'I' * (2 * WORDS + EXTRA + 3))
WORD_ADDRESSES = (0x0202446c, 0x0202466c, 0x02023a14, 0x02023c14, 0x02023e14, 0x02024014, 0x02024214, 0x0202200c, 0x0202220c)
WORD_VALUES = (0x40a05, 0x40a05, 0x3f, 0x7f, 0xf, 0xf1, 0xf1, 1, 1)
VARIANTS = [(m, 0, 0) for m in (0x01, 0x03, 0x0f, 0x3f, 0x40, 0xc0, 0xcf, 0xff)]
VARIANTS += [(m, 3, 0) for m in (0x01, 0x40, 0xff)] + [(0x3f, 5, 0), (0xff, 5, 0), (0x3f, 6, 0), (0xff, 6, 0)]
CONFIGS = len(VARIANTS)
RESULT_BYTES = HEAD.size + FILL.size + 2 * CLOCK + PROBE.size + PHASES * CONFIGS * CONFIG.size
MODE_NAMES = {0: 'sequential 64 KiB chunks', 3: 'page strides', 5: 'one shared stream, barrier per round',
              6: 'own regions, barrier per round'}


def inputs():
    return []


def chunk_bytes(mode):
    return 65536 if mode == 0 else 16384 if mode >= 5 else 4096


def chunk_index(mode, j):
    """The chunk visited at step j (numpy array in, numpy array out); mirrors ds_chunk_index."""
    import numpy as np
    n = REGION_BYTES // chunk_bytes(mode)
    if mode == 2:
        return ((j >> 1) % (n // 2)) + (j & 1) * (n // 2)
    if mode == 3:
        return (j * 7) % n
    return j % n   # modes 0, 1, 4 and 6; mode 5 is shared and handled by the caller


def buffer_words():
    import numpy as np
    index = np.arange(CORES * SLOT_BYTES // 8, dtype=np.uint64)
    return index * np.uint64(0x9e3779b97f4a7c15) + np.uint64(0x1234567)


def pmu_summary(pmu, cores, last, cb, hz, window):
    """Per core counters of the last window and the ratios used in the study. The L2 refill count times 64
    bytes is compared with the bytes the core consumed in that window."""
    out = {}
    for k in cores:
        cycles, l2r, l3r, bus, busrd, stall, mem = pmu[7 * k:7 * k + 7]
        out[k] = dict(cycles=cycles, l2d_refill=l2r, l3d_refill=l3r, bus_access=bus, bus_access_rd=busrd,
                      stall_backend=stall, mem_access=mem, stall_fraction=stall / cycles if cycles else 0.0)
    return out


def decode_phase(payload, base_offset, words, readiness, window, hz):
    import numpy as np
    if readiness & 16:
        return []
    configs = []
    for c in range(CONFIGS):
        values = CONFIG.unpack_from(payload, base_offset + c * CONFIG.size)
        mask, mode, stagger = values[:3]
        if (mask, mode, stagger) != VARIANTS[c]:
            raise ValueError(f'configuration {c} differs from the variant table')
        cores = [k for k in range(CORES) if mask >> k & 1]
        samples = values[4:4 + 2 * REPS * CORES]
        digests = values[4 + 2 * REPS * CORES + 7 * CORES:]
        pmu = values[4 + 2 * REPS * CORES:4 + 2 * REPS * CORES + 7 * CORES]
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
                if rep == REPS - 1 and mode == 5:
                    rank = cores.index(k)
                    chunk_digest = np.bitwise_xor.reduce(words[:SHARED_CHUNKS * cb // 8].reshape(SHARED_CHUNKS, cb // 8), axis=1)
                    steps = (np.arange(chunks, dtype=np.int64) * len(cores) + rank) % SHARED_CHUNKS
                    visits = np.bincount(steps, minlength=SHARED_CHUNKS) & 1
                    want = int(np.bitwise_xor.reduce(chunk_digest[visits == 1], initial=np.uint64(0)))
                    if digests[k] != want:
                        raise ValueError(f'configuration {c} core {k} digest differs from the data written')
                elif rep == REPS - 1:
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
                            pmu=pmu_summary(pmu, cores, reps_out[-1], cb, hz, window),
                            reps=reps_out))
    return configs


def decode(payload):
    import numpy as np
    if len(payload) != RESULT_BYTES:
        raise ValueError(f'DRAM interconnect study result must contain {RESULT_BYTES} bytes, not {len(payload)}')
    fields = HEAD.unpack_from(payload)
    magic, hz, base, window, warm, reps, readiness = fields[:7]
    before, after = fields[7:12], fields[12:17]
    if (magic, window, warm, reps) != (MAGIC, WINDOW_TICKS, WARM, REPS) or hz != 24000000:
        raise ValueError('DRAM interconnect study workload or counter configuration differs')
    offset = HEAD.size
    fill = FILL.unpack_from(payload, offset)
    offset += FILL.size
    clocks = []
    for _ in range(2):
        clocks.append(payload[offset:offset + CLOCK].split(b'\0', 1)[0].decode('ascii').strip())
        offset += CLOCK
    words = buffer_words()
    slots = words.reshape(CORES, SLOT_BYTES // 8)
    if not readiness & 16 and list(fill) != [int(np.bitwise_xor.reduce(slots[c])) for c in range(CORES)]:
        raise ValueError('the fill digests differ from the fill pattern')
    pv = PROBE.unpack_from(payload, offset)
    offset += PROBE.size
    probe = dict(words_before=dict(zip(map(hex, WORD_ADDRESSES), pv[:WORDS])),
                 words_written=dict(zip(map(hex, WORD_ADDRESSES), pv[WORDS:2 * WORDS])),
                 extra=pv[2 * WORDS:2 * WORDS + EXTRA], write_enabled=pv[2 * WORDS + EXTRA])
    if probe['write_enabled'] and tuple(pv[WORDS:2 * WORDS]) != WORD_VALUES:
        probe['write_mismatch'] = True
    phases = []
    for phase in range(PHASES):
        if phase == 1 and readiness & 32:
            phases.append([])
            continue
        phases.append(decode_phase(payload, offset + phase * CONFIGS * CONFIG.size, words, readiness, window, hz))
    clock_pattern = r'CLOCK cpu_l_mhz=(\d+) cpu_b_mhz=(\d+) cpu_l_mv=(\d+) cpu_b_mv=(\d+) transitions=(\d+) dsu_mhz=(\d+) .*'
    for line in clocks:
        if not re.match(clock_pattern, line):
            raise ValueError('clock record is missing or unknown: ' + line)
    return dict(counter_hz=hz, buffer_base=base, readiness=readiness,
                millicelsius_before=dict(zip(('cpu_b', 'ddr', 'npu', 'cpu_l', 'gpu'), before)),
                millicelsius_after=dict(zip(('cpu_b', 'ddr', 'npu', 'cpu_l', 'gpu'), after)),
                clock_before=clocks[0], clock_after=clocks[1], probe=probe, phases=phases)


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument('result', type=Path)
    args = parser.parse_args()
    print(json.dumps(decode(args.result.read_bytes()), indent=2))
