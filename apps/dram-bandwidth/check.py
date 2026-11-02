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

# The task protocol: the real main.c and kernel.c run on host threads with a stand-in for the
# FreeRTOS calls. Without early wake-ups and with them, every digest equals the fill pattern.
# The earlier protocol, which treated any notification as completion, fails with early wake-ups.
import re
shim = here / 'shim'
def build(source, name):
    out = a.build / name
    subprocess.run([a.cc, '-std=gnu11', '-O2', '-pthread', '-Wall', '-Wextra', '-Werror', '-I' + str(shim),
                    '-I' + str(here), '-I' + str(here.parents[1] / 'include'), str(source),
                    str(shim / 'shim.c'), str(here / 'kernel.c'), '-o', str(out)], check=True)
    return out
def run(exe, spurious):
    env = {'SHIM_SPURIOUS': '1'} if spurious else {}
    done = subprocess.run([str(exe)], capture_output=True, env=env, timeout=120)
    return done.returncode, done.stdout
current = build(here / 'main.c', 'bandwidth-protocol')
for spurious in (False, True):
    code, out = run(current, spurious)
    assert code == 0, (spurious, code)
    host.decode(out)
earlier = subprocess.check_output(['git', '-C', str(here), 'show', 'd4cd997d:apps/dram-bandwidth/main.c'], text=True)
earlier = earlier.replace('#include "result.h"', '#include "result.h"\n#include "portable.h"', 1)
earlier = re.sub(r'static uint64_t counter\(void\)\n\{.*?\n\}\n', '#define counter bw_counter\n', earlier, count=1, flags=re.S)
earlier = earlier.replace('__asm__ volatile("yield")', 'bw_relax()').replace('__asm__ volatile("dsb ish" ::: "memory");', 'bw_publish();')
earlier = earlier.replace('__asm__ volatile("mrs %0,cntfrq_el0" : "=r"(result.counter_hz));', 'result.counter_hz = bw_counter_hz();')
(a.build / 'main-earlier.c').write_text(earlier)
legacy = build(a.build / 'main-earlier.c', 'bandwidth-protocol-earlier')
code, out = run(legacy, False)
assert code == 0
host.decode(out)
rejected = False
try:
    code, out = run(legacy, True)
    if code == 0:
        host.decode(out)
except ValueError:
    rejected = True
rejected = rejected or code != 0
assert rejected, 'the earlier protocol survived early wake-ups'
print('task protocol passes with and without early wake-ups; the earlier protocol fails with them')

# Admission refuses a manifest whose run deadline and return reserve exceed the supervised
# session lifetime (apps/os/session.c); an image with such a manifest stops at the EL3 monitor.
import json
import re
def lifetime_us(root):
    text = (root / 'include/a733_board_application.h').read_text()
    return int(re.search(r'#define A733_BOARD_SUPERVISED_SECONDS (\d+)u', text).group(1)) * 1000000
def admitted(manifest, root):
    d = json.loads(manifest.read_text())['deadlines']
    return d['return_reserve_us'] < lifetime_us(root) and d.get('run_us', 0) <= lifetime_us(root) - d['return_reserve_us']
root = here.parents[1]
assert admitted(here / 'manifest.json', root)
too_long = a.build / 'manifest-too-long.json'
too_long.write_text(json.dumps({'deadlines': {'run_us': 90000000, 'return_reserve_us': 5000000}}))
assert not admitted(too_long, root)
print('manifest deadlines fit the supervised session lifetime; a 90 s run with a 5 s reserve is refused')
