#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Pin a complete split result, permitting clock telemetry and counter samples."""
import argparse
import importlib.util
import json
from pathlib import Path
import struct

spec = importlib.util.spec_from_file_location('split_host', Path(__file__).with_name('split-host.py'))
host = importlib.util.module_from_spec(spec)
spec.loader.exec_module(host)


def expectation(payload):
    host.decode(payload)
    words = list(struct.unpack('<' + 'I' * (len(payload) // 4), payload))
    raw = {'min': 0, 'max': 0xffffffff}
    words[64 // 4:576 // 4] = [raw] * (512 // 4)
    for phase in range(2 * host.REPS):
        base = 576 + phase * 576
        words[base // 4:base // 4 + 2] = [raw, raw]
        for core in range(host.CORES):
            start = base + 64 + core * 64 + 16
            words[start // 4:start // 4 + 4] = [raw] * 4
    return {'schema': 'sunstone-board-expect-u32-v1', 'words': words}


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('model', type=Path)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    a.output.write_text(json.dumps(expectation(a.model.read_bytes()), indent=2) + '\n')
