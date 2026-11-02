#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Validate every split record and decode counter times without pooling cores."""
import json
import re
import statistics
import struct

MAGIC = 0x53534353504c3031
RESULT_BYTES = 4032
UNITS, WARM, REPS, CORES = 256, 1, 3, 8
DIGEST = 0xf0ebbf03e42fda66  # Independent reference in host.py/check.py.
MASK = (1 << 64) - 1


def decode(payload):
    if len(payload) != RESULT_BYTES:
        raise ValueError('split result must contain 4032 bytes')
    magic, hz, units, warm, reps, cores = struct.unpack_from('<2Q4I', payload)
    if (magic, hz, units, warm, reps, cores) != (MAGIC, 24000000, UNITS, WARM, REPS, CORES):
        raise ValueError('split workload or counter differs')
    capacities = struct.unpack_from('<8I', payload, 32)
    if not all(0 < c <= 1024 for c in capacities):
        raise ValueError('split capacity is unknown or outside range')
    clocks = [payload[o:o + 256].split(b'\0', 1)[0].decode('ascii') for o in (64, 320)]
    pattern = r'CLOCK cpu_l_mhz=1794 cpu_b_mhz=2002 cpu_l_mv=1000 cpu_b_mv=1050 transitions=\d+ dsu_mhz=1274 sid=[0-9a-fA-F]{8} bin=\d+ core=6 pattern=0 parked_max_us=\d+\n?'
    if clocks[0] != clocks[1] or not re.fullmatch(pattern, clocks[0]):
        raise ValueError('split top-point epoch changed or is unknown')
    modes = {}
    for mode, name in enumerate(('equal', 'proportional')):
        phases = []
        for rep in range(REPS):
            base = 576 + (mode * REPS + rep) * 576
            launch, ready, *reserved = struct.unpack_from('<8Q', payload, base)
            if not launch or ready != 3 or any(reserved):
                raise ValueError('split readiness or phase padding differs')
            rows = []
            end = 0
            lengths = []
            for core in range(CORES):
                begin, stop, start, finish, digest, actual, r0, r1 = struct.unpack_from('<8Q', payload, base + 64 + core * 64)
                if begin != end or not begin <= stop <= UNITS or actual != core or r0 or r1:
                    raise ValueError('split core, range or padding differs')
                if not launch <= start < finish < launch + hz:
                    raise ValueError('split timing exceeds the one-second job bound')
                expected = sum(DIGEST ^ tile for tile in range(begin, stop)) & MASK
                if digest != expected:
                    raise ValueError('split work digest differs')
                count = stop - begin
                lengths.append(count)
                rows.append(dict(core=core, begin=begin, end=stop, start_ticks=start - launch,
                                 finish_ticks=finish - launch, elapsed_ticks=finish - start,
                                 elapsed_us=(finish - start) * 1e6 / hz,
                                 finish_us=(finish - launch) * 1e6 / hz))
                end = stop
            if end != UNITS:
                raise ValueError('split does not conserve all work')
            if mode == 0 and any(n != UNITS // CORES for n in lengths):
                raise ValueError('equal work ranges differ')
            # Integer largest-remainder allocation must be within one tile of
            # each exact capacity share, independent of tie-breaking details.
            if mode == 1 and any(abs(n - UNITS * c / sum(capacities)) >= 1 for n, c in zip(lengths, capacities)):
                raise ValueError('proportional ranges do not match capacities')
            finished = [r['finish_ticks'] for r in rows]
            for row in rows:
                row['finish_skew_us'] = (row['finish_ticks'] - min(finished)) * 1e6 / hz
            phases.append(dict(rep=rep, completion_us=max(finished) * 1e6 / hz,
                               skew_us=(max(finished) - min(finished)) * 1e6 / hz, cores=rows))
        completion = sorted(p['completion_us'] for p in phases)
        modes[name] = dict(phases=phases, p50_us=statistics.median(completion),
                           p99_us=completion[-1], max_us=completion[-1],
                           median_finish_skew_us=statistics.median(p['skew_us'] for p in phases),
                           per_core_median_finish_us=[statistics.median(p['cores'][c]['finish_us'] for p in phases) for c in range(CORES)],
                           per_core_median_skew_us=[statistics.median(p['cores'][c]['finish_skew_us'] for p in phases) for c in range(CORES)])
    return dict(capacities=capacities, clock=clocks[0], modes=modes,
                speedup=modes['equal']['p50_us'] / modes['proportional']['p50_us'],
                work_updates=UNITS * 4096 * 64,
                scope='Three paired finite trials; elapsed task time includes interruptions; seven-sample capacity calibration is separate.')


if __name__ == '__main__':
    import argparse
    from pathlib import Path
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('result', type=Path)
    print(json.dumps(decode(p.parse_args().result.read_bytes()), indent=2))
