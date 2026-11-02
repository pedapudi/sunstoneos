#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check the C result layout, the histogram bins, the decoder and the real main.c on host threads."""
import argparse
import subprocess
from pathlib import Path
import host

p = argparse.ArgumentParser()
p.add_argument('--cc', default='cc')
p.add_argument('--build', type=Path, required=True)
a = p.parse_args()
a.build.mkdir(parents=True, exist_ok=True)
here = Path(__file__).parent
exe = a.build / 'qos-model'
subprocess.run([a.cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', str(here / 'model.c'), '-o', str(exe)], check=True)
payload = subprocess.check_output([str(exe)])
assert len(payload) == host.RESULT_BYTES <= 16384, (len(payload), host.RESULT_BYTES)
d = host.decode(payload)
assert d['settings'][0]['probes'][0]['max_ns'] > 0 and d['settings'][0]['probes'][0]['p9999_ns'] > 0
# Every cycle count lies in the bin that the image assigns it, by the decoder's edges.
bins = a.build / 'qos-bin'
(a.build / 'bin.c').write_text('#include "result.h"\n#include <stdio.h>\nint main(void){for(unsigned long v=0;v<(1ul<<21);v+=(v<512?1:v/64+1))printf("%lu %u\\n",v,dq_bin((uint32_t)v));return 0;}\n')
subprocess.run([a.cc, '-std=c11', '-O1', '-I' + str(here), str(a.build / 'bin.c'), '-o', str(bins)], check=True)
for line in subprocess.check_output([str(bins)], text=True).splitlines():
    v, b = map(int, line.split())
    lo, hi = host.bin_edges(b)
    assert lo <= v < hi or b == host.BINS - 1, (v, b, lo, hi)
for load in (0, 1, 2, 3):
    exe = a.build / f'qos-protocol-{load}'
    subprocess.run([a.cc, '-std=gnu11', '-O1', '-pthread', '-Wall', '-Wextra', '-Werror', f'-DDQ_LOAD={load}', '-I' + str(here / 'shim'),
                    '-I' + str(here), '-I' + str(here.parents[1] / 'include'), str(here / 'main.c'), str(here / 'shim/shim.c'),
                    str(here / 'kernel.c'), '-o', str(exe)], check=True)
    done = subprocess.run([str(exe)], capture_output=True, timeout=120)
    assert done.returncode == 0, (load, done.returncode, done.stderr)
    r = host.decode(done.stdout)
    assert r['settings'][2]['port_words'][0][0] & (1 << 16) and r['settings'][2]['port_words'][0][1] & 0xf == 15, r['settings'][2]['port_words']
    assert r['settings'][0]['port_words'][0][1] == 0
    if load == 2:
        assert r['settings'][0]['npu']['jobs'] > 0
    if load == 3:
        assert r['settings'][0]['dma']['copies'] > 0
    if load == 1:
        assert all(p['stream_gb_per_s'] > 0 for p in r['settings'][0]['probes'])
print('DRAM latency study: layout, bins, decoder and the protocol under the four loads pass; the settings write the CPU ports and restore them')
