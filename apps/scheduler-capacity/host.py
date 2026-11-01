"""Decode measured per-core work without assigning scheduler capacities."""
import json
import re
import struct
import statistics
from pathlib import Path

MAGIC = 0x5353434150413031
OBS_BYTES = 304
CORE_BYTES = 1048
RESULT_BYTES = 32 + 8 * CORE_BYTES
WORDS, ROUNDS, WARM, REPS = 4096, 64, 3, 7


def inputs():
    return []


def expected_digest():
    # Independent integer reference, with the C kernel's unsigned wraparound.
    h = 0xcbf29ce484222325
    for i in range(WORDS):
        x = (0x12345678 ^ (i * 0x9e3779b9)) & 0xffffffff
        for _ in range(ROUNDS):
            x = ((x ^ (x >> 13)) * 1664525 + 1013904223) & 0xffffffff
        h = ((h ^ x) * 0x100000001b3) & 0xffffffffffffffff
    return h


def decode(payload):
    if len(payload) != RESULT_BYTES:
        raise ValueError(f'capacity result must contain {RESULT_BYTES} bytes')
    magic, hz, words, rounds, warm, reps = struct.unpack_from('<2Q4I', payload)
    if (magic, words, rounds, warm, reps) != (MAGIC, WORDS, ROUNDS, WARM, REPS) or hz != 24000000:
        raise ValueError('capacity workload or architectural counter configuration differs')
    reference = expected_digest()
    cores = []
    calibration_errors = []
    clock_epoch = None
    for core in range(8):
        base = 32 + core * CORE_BYTES
        mpidr, midr, isar0, mmfr2, clidr, slot, flags, atomic, ccidx, count, reserved = struct.unpack_from('<5Q6I', payload, base)
        if slot != core or mpidr & 0xff00ffffff != core << 8 or midr != (0x412fd050 if core < 6 else 0x414fd0b1):
            raise ValueError(f'CPU {core} captured identity differs from A733 topology')
        if flags != 15 or atomic not in (2, 3) or atomic != (isar0 >> 20) & 15 or ccidx not in (0, 1) or ccidx != (mmfr2 >> 20) & 15 or count != 4 or reserved:
            raise ValueError(f'CPU {core} feature observation is incomplete or unknown')
        caches = []
        for index in range(6):
            raw, size, level, kind, line, ways, sets, padding = struct.unpack_from('<2Q6I', payload, base + 64 + index * 40)
            if index >= count:
                if any((raw, size, level, kind, line, ways, sets, padding)):
                    raise ValueError('unused cache slot differs')
                continue
            expected_level, expected_kind = ((1, 2), (1, 1), (2, 4), (3, 4))[index]
            decoded_line = 1 << ((raw & 7) + 4)
            decoded_ways = ((raw >> 3) & (0x1fffff if ccidx else 0x3ff)) + 1
            decoded_sets = ((raw >> (32 if ccidx else 13)) & (0xffffff if ccidx else 0x7fff)) + 1
            if (level, kind, line, ways, sets, size, padding) != (expected_level, expected_kind, decoded_line, decoded_ways, decoded_sets, decoded_line * decoded_ways * decoded_sets, 0) or line != 64:
                raise ValueError(f'CPU {core} cache geometry differs')
            caches.append(dict(level=level, kind=kind, bytes=size, line_bytes=line, ways=ways, sets=sets, ccsidr=raw))
        if clidr & 0x1ff != 3 | 4 << 3 | 4 << 6:
            raise ValueError('private L1/L2 and shared L3 cache types differ')
        actual, readiness = struct.unpack_from('<2I', payload, base + OBS_BYTES)
        if actual != core:
            raise ValueError('worker ran on a different CPU')
        if readiness != 15:
            calibration_errors.append(f'CPU {core} clock readiness or thermal admission changed')
        clocks = []
        clock_lines = []
        for offset in (312, 568):
            raw = payload[base + offset:base + offset + 256]
            line = raw.split(b'\0', 1)[0].decode('ascii')
            clock_lines.append(line)
            clock = re.fullmatch(r'CLOCK cpu_l_mhz=(\d+) cpu_b_mhz=(\d+) cpu_l_mv=(\d+) cpu_b_mv=(\d+) transitions=(\d+) dsu_mhz=(\d+) sid=([0-9a-fA-F]{8}) bin=(\d+) core=(\d+) pattern=(\d+) parked_max_us=(\d+)\n?', line)
            if not clock:
                calibration_errors.append(f'CPU {core} clock observation is missing or unknown')
                clocks.append(None)
                continue
            keys = ('cpu_l_mhz', 'cpu_b_mhz', 'cpu_l_mv', 'cpu_b_mv', 'transitions',
                    'dsu_mhz', 'sid', 'bin', 'observer_core', 'pattern', 'parked_max_us')
            values = [int(value, 16 if key == 'sid' else 10) for key, value in zip(keys, clock.groups())]
            observation = dict(zip(keys, values))
            clocks.append(observation)
            epoch = {key: value for key, value in observation.items() if key != 'observer_core'}
            if any(not epoch[key] for key in ('cpu_l_mhz', 'cpu_b_mhz', 'cpu_l_mv', 'cpu_b_mv', 'dsu_mhz')) or epoch['bin'] == 99:
                calibration_errors.append(f'CPU {core} operating point is unknown')
            if clock_epoch is None:
                clock_epoch = epoch
            elif epoch != clock_epoch:
                calibration_errors.append(f'CPU {core} clock epoch changed during sequential work')
        samples = {}
        for kernel, name in enumerate(('integer', 'neon')):
            values = [struct.unpack_from('<2Q', payload, base + 824 + (kernel * REPS + rep) * 16) for rep in range(REPS)]
            if any(ticks <= 0 or ticks > hz or digest != reference for ticks, digest in values):
                raise ValueError(f'CPU {core} {name} work or duration differs')
            samples[name] = [dict(ticks=ticks, digest=digest) for ticks, digest in values]
        cores.append(dict(core=core, mpidr=mpidr, midr=midr, isar0=isar0, mmfr2=mmfr2,
                          clidr=clidr, flags=flags, atomic=atomic, ccidx=ccidx, caches=caches,
                          clocks=clocks, clock_lines=clock_lines, readiness=readiness, samples=samples))
    # Raw durations remain available; ratios describe only this fixed workload.
    medians = {name: [statistics.median(s['ticks'] for s in c['samples'][name]) for c in cores]
               for name in ('integer', 'neon')}
    ratios = None if calibration_errors else {name: statistics.median(values[:6]) / statistics.median(values[6:]) for name, values in medians.items()}
    return dict(counter_hz=hz, words=words, rounds=rounds, warm=warm, reps=reps,
                digest=reference, cores=cores, calibration_valid=not calibration_errors,
                calibration_errors=calibration_errors, measured_a76_over_a55=ratios,
                qualification='finite diagnostic; sustained, thermal and fault qualification remain open')


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument('result', type=Path)
    args = parser.parse_args()
    print(json.dumps(decode(args.result.read_bytes()), indent=2))
