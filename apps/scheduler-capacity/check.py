#!/usr/bin/env python3
"""Check C producer layout, independent digests and decoder rejection paths."""
import argparse
from pathlib import Path
import subprocess
import struct
import host
import expect

p = argparse.ArgumentParser()
p.add_argument('--cc', default='cc')
p.add_argument('--build', type=Path, required=True)
a = p.parse_args()
a.build.mkdir(parents=True, exist_ok=True)
root = Path(__file__).resolve().parents[2]
exe = a.build / 'capacity-model'
subprocess.run([a.cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                '-I' + str(root / 'include'), str(Path(__file__).with_name('model.c')),
                str(Path(__file__).with_name('kernel.c')), '-o', str(exe)], check=True)
payload = subprocess.check_output([str(exe)])
result = host.decode(payload)
assert result['digest'] == host.expected_digest()
expected = expect.expectation(payload)['words']
assert sum(isinstance(word, dict) for word in expected) == 8 * 2 * host.REPS
for offset, replacement in ((0, 0), (32 + 40, 7), (32 + 304, 1),
                            (32 + 824, 0), (32 + 832, 0), (24, 2)):
    altered = bytearray(payload)
    struct.pack_into('<I', altered, offset, replacement)
    try:
        host.decode(altered)
    except ValueError:
        pass
    else:
        raise AssertionError(f'decoder accepted corruption at {offset}')
print(f'capacity C layout and exact integer/NEON digest {result["digest"]:016x} pass; corrupt header, identity, CPU, duration, digest and warm count rejected')
