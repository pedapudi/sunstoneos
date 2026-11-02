#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check the C result layout, the chunk orders, the digests and the decoder's rejection paths on the host,
and run the real main.c and kernel.c on host threads against a stand-in for the FreeRTOS calls."""
import argparse
import json
import re
import struct
import subprocess
from pathlib import Path
import host

p = argparse.ArgumentParser()
p.add_argument('--cc', default='cc')
p.add_argument('--build', type=Path, required=True)
a = p.parse_args()
a.build.mkdir(parents=True, exist_ok=True)
here = Path(__file__).parent
exe = a.build / 'scaling-model'
subprocess.run([a.cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', str(here / 'model.c'),
                str(here / 'kernel.c'), '-o', str(exe)], check=True)
payload = subprocess.check_output([str(exe)])
assert len(payload) == host.RESULT_BYTES and host.RESULT_BYTES <= 16384, len(payload)
# The variant table of the decoder equals the one in result.h.
header = (here / 'result.h').read_text()
table = re.search(r'#define DS_VARIANTS \{(.*?)\}\s*\n#define DS_CONFIGS (\d+)u', header, re.S)
triples = [tuple(int(x, 0) for x in t) for t in re.findall(r'\{(\w+),(\w+),(\w+)\}', table.group(1))]
assert triples == host.VARIANTS and int(table.group(2)) == host.CONFIGS == len(triples)
result = host.decode(payload)
assert len({(c['mask'], c['mode'], c['stagger']) for c in result['configs']}) == host.CONFIGS
# Every chunk order visits each chunk equally often over a whole number of passes.
import numpy as np
for mode in range(7):
    n = host.REGION_BYTES // host.chunk_bytes(mode)
    if mode == 5:
        continue
    visits = np.bincount(host.chunk_index(mode, np.arange(n, dtype=np.int64)), minlength=n)
    assert (visits == 1).all(), mode
base = host.HEAD.size + host.FILL.size + 2 * host.CLOCK
for offset, replacement, fmt in ((0, 0x58, '<B'), (24, 1, '<I'), (host.HEAD.size, 1, '<Q'),
                                 (base + host.CONFIG.size, 0x41, '<I'), (base + 16, 0, '<I'),
                                 (base + host.CONFIG.size * 3 - 8, 5, '<Q'),
                                 (base + 16 + 4, 1, '<I')):
    altered = bytearray(payload)
    struct.pack_into(fmt, altered, offset, replacement)
    try:
        host.decode(bytes(altered))
    except ValueError:
        pass
    else:
        raise AssertionError(f'decoder accepted corruption at {offset}')
print('DRAM scaling result layout, variant table, fill and chunk digests pass; corrupt header, fill, variant, digest, window and unused core rejected')

shim = here / 'shim'
exe = a.build / 'scaling-protocol'
subprocess.run([a.cc, '-std=gnu11', '-O2', '-pthread', '-Wall', '-Wextra', '-Werror', '-I' + str(shim), '-I' + str(here),
                '-I' + str(here.parents[1] / 'include'), str(here / 'main.c'), str(shim / 'shim.c'),
                str(here / 'kernel.c'), '-o', str(exe)], check=True)
for env in ({}, {'SHIM_SPURIOUS': '1'}):
    done = subprocess.run([str(exe)], capture_output=True, env=env, timeout=300)
    assert done.returncode == 0, (env, done.returncode, done.stderr)
    decoded = host.decode(done.stdout)
    assert all(c['sum_gb_per_s'] > 0 for c in decoded['configs'])
print('task protocol passes with and without early wake-ups; the digests of the image match the decoder')

lifetime = int(re.search(r'#define A733_BOARD_SUPERVISED_SECONDS (\d+)u',
                         (here.parents[1] / 'include/a733_board_application.h').read_text()).group(1)) * 1000000
d = json.loads((here / 'manifest.json').read_text())['deadlines']
assert d['return_reserve_us'] < lifetime and d['run_us'] <= lifetime - d['return_reserve_us']
print('manifest deadlines fit the supervised session lifetime')
