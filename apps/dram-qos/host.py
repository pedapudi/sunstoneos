#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Decode the CPU memory-latency study result (apps/dram-qos/result.h).

Each probe is a log-linear histogram of cycles per timed event. The decoder converts cycles to nanoseconds with the
core clock of the clock record (CPU-B for the A76 core, CPU-L for the A55 core), reports the median, 99th, 99.99th
percentile (the upper edge of the bin that holds it) and the exact maximum, and the cost to the other masters.
"""
import json
import re
import struct
from pathlib import Path

MAGIC = b'SSDRAMQS'
SETTINGS, PROBES, BINS = 3, 3, 256
HEAD = struct.Struct('<8sQQ4I5i5i256s')
PROBE = struct.Struct('<256I4I2I2Q')
SETTING_A = struct.Struct('<8I2I')
SETTING_B = struct.Struct('<2I4Q2IQ')
SETTING_BYTES = SETTING_A.size + PROBES * PROBE.size + SETTING_B.size
RESULT_BYTES = HEAD.size + SETTINGS * SETTING_BYTES
PROBE_NAMES = ('pointer chase, A76 core 7', 'pointer chase, A55 core 0', 'control loop at 10 kHz, A76 core 7')
SETTING_NAMES = ('as found (all ports equal)', 'CPU ports priority 3', 'CPU ports priority 3 and high-priority reads')
LOADS = ('none', 'streaming reads on the other worker cores', 'NPU job stream', 'DMA0 4 KiB copies')


def inputs():
    return []


def bin_edges(b):
    if b < 64:
        return 4 * b, 4 * b + 4
    msb = 8 + (b - 64) // 16
    sub = (b - 64) % 16
    width = 1 << (msb - 4)
    return (16 + sub) * width, (17 + sub) * width


def percentile(hist, fraction):
    total = sum(hist)
    need = total * fraction
    run = 0
    for b, n in enumerate(hist):
        run += n
        if run >= need and n:
            return bin_edges(b)[1]
    return 0


def decode(payload):
    if len(payload) != RESULT_BYTES:
        raise ValueError(f'DRAM latency result must contain {RESULT_BYTES} bytes, not {len(payload)}')
    f = HEAD.unpack_from(payload)
    magic, hz, base, load, readiness = f[:5]
    before, after = f[7:12], f[12:17]
    clock = f[17].split(b'\0', 1)[0].decode('ascii').strip()
    m = re.match(r'CLOCK cpu_l_mhz=(\d+) cpu_b_mhz=(\d+) .*', clock)
    if magic != MAGIC or hz != 24000000 or not m or load > 3:
        raise ValueError('DRAM latency workload, counter or clock record differs')
    mhz = (int(m.group(2)), int(m.group(1)), int(m.group(2)))     # probe 0 on CPU-B, 1 on CPU-L, 2 on CPU-B
    out = []
    offset = HEAD.size
    for s in range(SETTINGS):
        a = SETTING_A.unpack_from(payload, offset)
        offset += SETTING_A.size
        probes = []
        for p in range(PROBES):
            v = PROBE.unpack_from(payload, offset)
            offset += PROBE.size
            hist, (max_cycles, samples, overhead, _), (misses, late_max), (stream_bytes, ticks) = v[:BINS], v[BINS:BINS + 4], v[BINS + 4:BINS + 6], v[BINS + 6:]
            if sum(hist) != samples or not samples:
                raise ValueError(f'setting {s} probe {p}: histogram and sample count differ')
            if max_cycles < max(bin_edges(b)[0] for b in range(BINS) if hist[b]) and max_cycles < 1:
                raise ValueError('maximum below the histogram')
            ns = 1000.0 / mhz[p]
            probes.append(dict(name=PROBE_NAMES[p], samples=samples, overhead_cycles=overhead,
                               p50_ns=percentile(hist, .5) * ns, p99_ns=percentile(hist, .99) * ns,
                               p9999_ns=percentile(hist, .9999) * ns, max_ns=max_cycles * ns,
                               misses=misses, late_max_us=late_max / 24.0,
                               stream_gb_per_s=stream_bytes / (ticks / hz) / 1e9 if ticks else 0.0))
        b = SETTING_B.unpack_from(payload, offset)
        offset += SETTING_B.size
        npu_jobs, npu_failed, ts, tmax, tmin, window, dma, dma_failed, dma_window = b
        out.append(dict(name=SETTING_NAMES[s], port_words=[a[0:4], a[4:8]], npu_port_words=a[8:10], probes=probes,
                        npu=dict(jobs=npu_jobs, failed=npu_failed, mean_ms=ts / npu_jobs / hz * 1000 if npu_jobs else 0.0,
                                 max_ms=tmax / hz * 1000, min_ms=tmin / hz * 1000, window_s=window / hz,
                                 jobs_per_s=npu_jobs / (window / hz) if window else 0.0),
                        dma=dict(copies=dma, failed=dma_failed, window_s=dma_window / hz,
                                 mb_per_s=dma * 4096 / (dma_window / hz) / 1e6 if dma_window else 0.0)))
    return dict(load=LOADS[load], buffer_base=base, readiness=readiness, clock=clock,
                millicelsius_before=before, millicelsius_after=after, settings=out)


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument('result', type=Path)
    args = parser.parse_args()
    print(json.dumps(decode(args.result.read_bytes()), indent=2))
