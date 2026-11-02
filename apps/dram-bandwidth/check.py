#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check the C result layout, the read kernel's digests and the decoder's rejection paths on the host."""
import argparse
from pathlib import Path
import struct
import subprocess
import host

p = argparse.ArgumentParser()
p.add_argument('--cc', default='cc')
p.add_argument('--build', type=Path, required=True)
a = p.parse_args()
a.build.mkdir(parents=True, exist_ok=True)
here = Path(__file__).parent
exe = a.build / 'bandwidth-model'
subprocess.run([a.cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', str(here / 'model.c'),
                str(here / 'kernel.c'), '-o', str(exe)], check=True)
payload = subprocess.check_output([str(exe)])
assert len(payload) == host.RESULT_BYTES == 1376
result = host.decode(payload)
assert [c['cores'] for c in result['configs']] == [1, 1, 2, 6, 8]
config = host.HEAD.size + 8 * 8 + 512
for offset, replacement in ((0, 0x58), (16, 7), (host.HEAD.size, 1), (config + host.CONFIG.size, 0x41),
                            (config + 24, 5), (config + 16, 0)):
    altered = bytearray(payload)
    struct.pack_into('<B' if offset < 8 else '<I', altered, offset, replacement)
    try:
        host.decode(bytes(altered))
    except ValueError:
        pass
    else:
        raise AssertionError(f'decoder accepted corruption at {offset}')
print('DRAM bandwidth result layout, fill and pass digests pass; corrupt header, fill digest, core set, pass digest and duration rejected')
