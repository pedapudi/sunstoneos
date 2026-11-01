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
assert result['calibration_valid'] and result['measured_a76_over_a55']
# A down/up excursion restores the frequency prefix but advances its epoch.
for field, old, new, offsets in (
        ('recovered frequency transition', b'transitions=3', b'transitions=5', (568,)),
        ('DSU changed within job', b'dsu_mhz=1274', b'dsu_mhz=1014', (568,)),
        ('DSU changed between jobs', b'dsu_mhz=1274', b'dsu_mhz=1014', (312, 568)),
        ('SID changed', b'sid=12340400', b'sid=12340401', (568,)),
        ('unknown bin', b'bin=0', b'bin=99', (568,)),
        ('park maximum changed', b'parked_max_us=40', b'parked_max_us=50', (568,)),
        ('missing epoch', b'transitions=3 ', b'', (568,))):
    altered = bytearray(payload)
    for offset in offsets:
        start = 32 + 3 * host.CORE_BYTES + offset
        line = payload[start:start + 256].split(b'\0', 1)[0]
        changed = line.replace(old, new)
        assert changed != line
        altered[start:start + 256] = changed.ljust(256, b'\0')
    invalid = host.decode(altered)
    assert not invalid['calibration_valid'] and invalid['measured_a76_over_a55'] is None, field
    assert invalid['cores'][3]['samples'] == result['cores'][3]['samples'], field
    assert invalid['cores'][3]['clock_lines'] != result['cores'][3]['clock_lines'], field
altered = bytearray(payload)
struct.pack_into('<I', altered, 32 + 308, 7)
invalid = host.decode(altered)
assert not invalid['calibration_valid'] and invalid['measured_a76_over_a55'] is None
assert invalid['cores'][0]['readiness'] == 7
print('clock epoch excursions, DSU/SID/bin/park changes, missing epoch and thermal readiness invalidate calibration while preserving observations and samples')
