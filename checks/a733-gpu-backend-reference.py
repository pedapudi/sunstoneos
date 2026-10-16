#!/usr/bin/env python3
"""Check pinned identity decoding and reuse verified register traces for the native binding."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('source', type=Path)
p.add_argument('start_evidence', type=Path)
p.add_argument('stop_evidence', type=Path)
p.add_argument('output', type=Path)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
manifest = json.loads((a.source.parent / 'manifest.json').read_text())
inputs = {}
for name in ['pvr_device.c', 'pvr_rogue_cr_defs.h', 'pvr_fw_startstop.c', 'pvr_fw_mips.c', 'pvr_fw.c']:
    entry = next(v for v in manifest if v['path'] == 'imagination/' + name)
    assert '/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/' in entry['url']
    assert digest(a.source / name) == entry['sha256']
    inputs[name] = entry
source = (a.source / 'pvr_device.c').read_text()
start = source.index('static void\npvr_load_gpu_id(')
end = source.index('\n}\n', start) + 3
function = source[start:end]
shim = r'''/* SPDX-License-Identifier: MIT
 * Copyright (c) 2023 Imagination Technologies Ltd. */
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include "pvr_rogue_cr_defs.h"
typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
struct pvr_gpu_id { u16 b,v,n,c; };
struct pvr_device { struct pvr_gpu_id gpu_id; u64 raw; u32 rev,id; unsigned wide,narrow; };
#define FIELD_GET(mask,value) (((value)&(mask)) >> __builtin_ctzll(mask))
#define PVR_CR_FIELD_GET(value,field) (((value)&~ROGUE_CR_##field##_CLRMSK) >> ROGUE_CR_##field##_SHIFT)
static u64 pvr_cr_read64(struct pvr_device *d, u32 off)
{ assert(off==0x20); ++d->wide; return d->raw; }
static u32 pvr_cr_read32(struct pvr_device *d, u32 off)
{ assert(off==0x20 || off==0x18); ++d->narrow; return off==0x20 ? d->rev : d->id; }
'''
shim += function + r'''
int main(void) {
 struct pvr_device d={.raw=UINT64_C(0x00240038006800b7)};
 pvr_load_gpu_id(&d);
 assert(d.gpu_id.b==36 && d.gpu_id.v==56 && d.gpu_id.n==104 && d.gpu_id.c==183 && d.wide==1 && !d.narrow);
 d=(struct pvr_device){.raw=0x243800,.rev=0x243800,.id=0x68b7};
 pvr_load_gpu_id(&d);
 assert(d.gpu_id.b==36 && d.gpu_id.v==56 && d.gpu_id.n==104 && d.gpu_id.c==183 && d.wide==1 && d.narrow==2);
 puts("PASS original GPU identity decoding and transaction widths");
}
'''
(a.output / 'identity-reference.c').write_text(shim)
command = ['clang-21', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
           '-I' + str(a.source), str(a.output / 'identity-reference.c'), '-o', str(a.output / 'identity-reference')]
subprocess.run(command, check=True)
subprocess.run([str(a.output / 'identity-reference')], check=True)
header = 'struct trace { unsigned write, offset, width; uint64_t value; };\n'
reused = {}
for kind, directory in [('start', a.start_evidence), ('stop', a.stop_evidence)]:
    comparison = json.loads((directory / 'comparison.json').read_text())
    assert comparison['result'] == 'pass'
    for name in ['pvr_fw_startstop.c', 'pvr_rogue_cr_defs.h']:
        assert comparison['sources'][name] == digest(a.source / name)
    for variant in range(2):
        path = directory / f'trace-{variant}.json'
        reused[str(path)] = digest(path)
        trace = json.loads(path.read_text())['events' if kind == 'start' else 'accesses']
        header += f'static const struct trace {kind}_trace_{variant}[] = {{\n'
        for item in trace:
            write = int(item['kind'] == 1) if kind == 'start' else item['write']
            header += f'    {{{write}, {item["offset"]}, {item["width"]}, UINT64_C({item["value"]})}},\n'
        header += '};\n'
(a.output / 'gpu-backend-traces.h').write_text(header)
(a.output / 'reference-inputs.json').write_text(json.dumps({'sources': inputs, 'reused_traces': reused,
    'identity_command': command, 'trace_header_sha256': digest(a.output / 'gpu-backend-traces.h')}, indent=2) + '\n')
print('Prepared previously verified startup/shutdown traces for native MMIO checks')
