#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build word expectations from an explicitly pinned identity configuration.

The baseline must be a previously decoded result from the same target and bin.
Only the duration words vary. Identity and clock values stay exact.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import struct

spec = importlib.util.spec_from_file_location('capacity_host', Path(__file__).with_name('host.py'))
host = importlib.util.module_from_spec(spec)
spec.loader.exec_module(host)


def expectation(baseline):
    decoded = host.decode(baseline)
    if not decoded['calibration_valid']:
        raise ValueError('baseline clock epoch is invalid for calibration')
    words = list(struct.unpack('<' + 'I' * (len(baseline) // 4), baseline))
    for core in range(8):
        for kernel in range(2):
            for rep in range(host.REPS):
                offset = 32 + core * host.CORE_BYTES + 1144 + (kernel * host.REPS + rep) * 16
                words[offset // 4] = {'min': 1, 'max': decoded['counter_hz']}
                words[offset // 4 + 1] = 0
    return {'schema': 'sunstone-board-expect-u32-v1', 'words': words}


def discovery_expectation():
    # Discovery permits unknown register/geometry words, never unknown digests.
    raw = {'min': 0, 'max': 0xffffffff}
    words = [0] * (host.RESULT_BYTES // 4)
    digest = host.expected_digest()
    def u64(offset, value):
        words[offset // 4:offset // 4 + 2] = [value & 0xffffffff, value >> 32]
    u64(0, host.MAGIC)
    u64(8, 24000000)
    words[4:8] = [host.WORDS, host.ROUNDS, host.WARM, host.REPS]
    for core in range(8):
        base = 32 + core * host.CORE_BYTES
        # MPIDR.MT is set on both observed A733 core types. Affinity is in Aff1.
        u64(base, 0x81000000 | core << 8)
        u64(base + 8, 0x412fd050 if core < 6 else 0x414fd0b1)
        words[(base + 16) // 4:(base + 40) // 4] = [raw] * 6
        words[(base + 40) // 4:(base + 64) // 4] = [core, 15, 2, {'min': 0, 'max': 1}, 4, 0]
        for index in range(4):
            offset = base + 64 + index * 40
            words[offset // 4:(offset + 16) // 4] = [raw] * 4
            level, kind = ((1, 2), (1, 1), (2, 4), (3, 4))[index]
            words[(offset + 16) // 4:(offset + 40) // 4] = [level, kind, 64, raw, raw, 0]
        words[(base + 624) // 4:(base + 632) // 4] = [core, 15]
        # Clock text is retained for host decoding, including its variable telemetry.
        words[(base + 632) // 4:(base + 1144) // 4] = [raw] * 128
        for kernel in range(2):
            for rep in range(host.REPS):
                offset = base + 1144 + (kernel * host.REPS + rep) * 16
                words[offset // 4] = {'min': 1, 'max': 24000000}
                u64(offset + 8, digest)
    return {'schema': 'sunstone-board-expect-u32-v1', 'words': words}


if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('baseline', type=Path, nargs='?')
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    a.output.write_text(json.dumps(expectation(a.baseline.read_bytes()) if a.baseline else discovery_expectation(), indent=2) + '\n')
