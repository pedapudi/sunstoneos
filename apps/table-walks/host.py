#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Decode the core-6 forced-walk result and compare matched boot profiles."""
import json
import importlib.util
from pathlib import Path
import re
import statistics
import struct

HEAD = struct.Struct('<8s5Q')
SAMPLE = struct.Struct('<6Q')
SAMPLES, ITERATIONS = 6, 1024
RESULT_BYTES = HEAD.size + 512 + SAMPLES * SAMPLE.size


def profiles():
    """Fixed header values, embedded when freezing a standalone decoder."""
    if '_WALK_PROFILES' in globals():
        return _WALK_PROFILES
    path = Path(__file__).resolve().parents[2] / 'tools/a733_tcr.py'
    spec = importlib.util.spec_from_file_location('table_walk_profiles', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.UNCACHED_WALK, module.CACHED_WALK


def freeze(output):
    source = Path(__file__).read_text()
    first, rest = source.split('\n', 1)
    Path(output).write_text(first + '\n_WALK_PROFILES = ' + repr(profiles()) + '\n' + rest)


def inputs():
    return []


def decode(payload):
    if len(payload) != RESULT_BYTES:
        raise ValueError('table-walk result length differs')
    magic, tcr, midr, core, iterations, hz = HEAD.unpack_from(payload)
    if (magic, core, iterations, hz) != (b'SSTWALK1', 6, ITERATIONS, 24000000) or (midr >> 4) & 0xfff != 0xd0b:
        raise ValueError('table-walk CPU or workload differs')
    uncached, cached = profiles()
    if tcr not in (uncached, cached):
        raise ValueError('table-walk translation-control profile differs')
    clocks = []
    for offset in (HEAD.size, HEAD.size + 256):
        line = payload[offset:offset + 256].split(b'\0', 1)[0].decode('ascii').strip()
        fields = {key: int(value) for key, value in re.findall(r'(\w+)=(\d+)', line)}
        if any(fields.get(key) != value for key, value in (
                ('cpu_l_mhz', 1794), ('cpu_b_mhz', 2002), ('dsu_mhz', 1274), ('core', 6))):
            raise ValueError('table-walk operating points differ: ' + line)
        if fields.get('pattern') != 0:
            raise ValueError('table-walk clock modulation is active')
        clocks.append(line)
    samples = []
    for i in range(SAMPLES):
        values = SAMPLE.unpack_from(payload, HEAD.size + 512 + i * SAMPLE.size)
        cycles, ticks, dtlb, itlb, bus, digest = values
        if not cycles or not 0 < ticks < hz // 100:
            raise ValueError(f'table-walk block {i} exceeds the 10 ms budget')
        # Cortex-A76 TRM C2.1 permits approximate absolute counts because of
        # pipeline effects. Keep the warm-up and measured counts as evidence;
        # the linked instruction check establishes the loop's iteration count.
        if not dtlb or not itlb:
            raise ValueError(f'table-walk block {i} walk counters did not advance')
        if abs(cycles / ticks - 2002 / 24) > (2002 / 24) * 0.02:
            raise ValueError(f'table-walk block {i} cycle/counter calibration differs')
        samples.append(dict(cycles=cycles, ticks=ticks, dtlb_walk=dtlb, itlb_walk=itlb,
                            bus_access=bus, digest=digest, cycles_per_iteration=cycles / ITERATIONS))
    measured = [s['cycles_per_iteration'] for s in samples[1:]]
    median = statistics.median(measured)
    return dict(tcr=hex(tcr), cached=tcr == cached, midr=hex(midr), core=core,
                counter_hz=hz, iterations=iterations, clock_before=clocks[0], clock_after=clocks[1],
                samples=samples, median_cycles_per_iteration=median,
                spread_cycles=max(measured) - min(measured),
                spread_percent=100 * (max(measured) - min(measured)) / median)


def compare(a, b):
    if a['cached'] or not b['cached'] or a['midr'] != b['midr'] or a['iterations'] != b['iterations']:
        raise ValueError('comparison needs uncached A and cached B on the same CPU and workload')
    old, new = a['median_cycles_per_iteration'], b['median_cycles_per_iteration']
    for event in ('dtlb_walk', 'itlb_walk'):
        left = [s[event] for s in a['samples'][1:]]
        right = [s[event] for s in b['samples'][1:]]
        if max(min(left), min(right)) > min(max(left), max(right)):
            raise ValueError(f'comparison {event} count ranges do not overlap')
        if not min(left) <= statistics.median(right) <= max(left):
            raise ValueError(f'comparison {event} median is outside the control range')
    spread = max(a['spread_cycles'], b['spread_cycles'])
    return dict(uncached_cycles=old, cached_cycles=new, saved_cycles=old - new,
                saved_percent=100 * (old - new) / old, spread_cycles=spread,
                gain=old - new > spread)


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('uncached', type=Path, nargs='?')
    parser.add_argument('cached', type=Path, nargs='?')
    parser.add_argument('--freeze', type=Path, help='write a standalone decoder with the shared profile values embedded')
    args = parser.parse_args()
    if args.freeze:
        freeze(args.freeze)
        raise SystemExit(0)
    if not args.uncached:
        parser.error('supply a result or --freeze')
    a = decode(args.uncached.read_bytes())
    print(json.dumps(dict(a=a, b=(b := decode(args.cached.read_bytes())), comparison=compare(a, b))
                     if args.cached else a, indent=2))
