#!/usr/bin/env python3
"""Summarize wake latencies of latency diagnostic reports, to compare images that differ in SUNSTONE_LATENCY_TLBI.

  tlbi-report.py LABEL=output.bin ...

Reads the binary report (apps/latency/workload.h): per core 64 idle and 128 busy wake samples as (ready, running) counter
pairs, the counter frequency, and, in an image with SUNSTONE_LATENCY_TLBI, the largest ticks the issuing core spent in one
invalidation (the dma_jobs word). Prints, for each report and each metric (idle, busy) and cluster (cores 0 to 5 Cortex-A55,
cores 6 and 7 Cortex-A76), the sample count, median, 99th percentile, 99.9th percentile (the largest sample when fewer than
1000 samples) and maximum, in microseconds.
"""
import struct
import sys

CORES, IDLE, BUSY = 8, 64, 128
WAKES, SAMPLE = 20848, 24
MAGIC = 0x53534c4154454e31


def load(path):
    raw = open(path, 'rb').read()
    assert len(raw) == 63872, '%s: %d bytes, not a latency report' % (path, len(raw))
    magic, frequency = struct.unpack_from('<2Q', raw)
    assert magic == MAGIC, '%s: not a latency report' % path
    issue = struct.unpack_from('<I', raw, 32)[0]
    samples = {(m, g): [] for m in ('idle', 'busy') for g in ('A55', 'A76')}
    for core in range(CORES):
        for index in range(IDLE + BUSY):
            ready, running, metric, flags = struct.unpack_from('<2Q2I', raw, WAKES + (core * (IDLE + BUSY) + index) * SAMPLE)
            assert flags == (1 if index < IDLE else 2), (path, core, index, flags)
            samples['idle' if index < IDLE else 'busy', 'A76' if core >= 6 else 'A55'].append((running - ready) * 1e6 / frequency)
    return frequency, issue, samples


def quantile(values, q):
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(q * len(ordered)))]


def main():
    for argument in sys.argv[1:]:
        label, path = argument.split('=', 1)
        frequency, issue, samples = load(path)
        print('%s: issuing core spent at most %.2f us in one invalidation' % (label, issue * 1e6 / frequency)
              if issue else '%s: no invalidation issued' % label)
        for (metric, cluster), values in sorted(samples.items()):
            print('  %-4s %-3s n=%4d  p50 %6.2f  p99 %6.2f  p99.9 %6.2f  max %6.2f us' % (
                metric, cluster, len(values), quantile(values, .5), quantile(values, .99), quantile(values, .999), max(values)))


if __name__ == '__main__':
    main()
