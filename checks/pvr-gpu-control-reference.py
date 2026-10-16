#!/usr/bin/env python3
"""Compare compatibility layout, MIPS IRQ operations, and reused health-command evidence."""
import argparse
import ctypes as C
import hashlib
import json
from pathlib import Path
import re
import subprocess


def function(source, name, result):
    start = re.search(r'\b' + name + r'\(', source).start()
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return result + '\n' + source[start:end] + '\n'


SHIM = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint64_t aligned_u64 __attribute__((aligned(8)));
#define __aligned(n) __attribute__((aligned(n)))
#define OFFSET_CHECK(t,f,n) _Static_assert(offsetof(t,f)==(n), #f)
#define SIZE_CHECK(t,n) _Static_assert(sizeof(t)==(n), #t)
#include "pvr_rogue_cr_defs.h"
struct pvr_device { u32 status,offset,value,write; };
static u32 pvr_cr_read32(struct pvr_device *d,u32 off) {
    d->offset=off; d->value=d->status; d->write=0; return d->status;
}
static void pvr_cr_write32(struct pvr_device *d,u32 off,u32 value) {
    d->offset=off; d->value=value; d->write=1;
}
'''
ENTRY = r'''
void fixture(void *output) {
    struct rogue_fwif_compchecks c={0};
    rogue_fwif_compchecks_bvnc_init(&c.hw_bvnc);
    rogue_fwif_compchecks_bvnc_init(&c.fw_bvnc);
    c.hw_bvnc.bvnc=c.fw_bvnc.bvnc=0x00240038006800b7ull;
    c.fw_processor_version=0x12345678; c.ddk_version=0x01020304;
    c.ddk_build=6976702; c.build_options=0x55667788;
    c.init_options.os_count_support=1; c.updated=true;
    memcpy(output,&c,sizeof(c));
}
void irq(u32 write,u32 status,u32 *output) {
    struct pvr_device d={.status=status};
    if(write) pvr_mips_irq_clear(&d);
    else assert(pvr_mips_irq_pending(&d)==(bool)(status&1));
    output[0]=d.write; output[1]=d.offset; output[2]=d.value;
}
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('sources',type=Path)
    ap.add_argument('queue_evidence',type=Path)
    ap.add_argument('evidence',type=Path)
    args = ap.parse_args()
    root, evidence = args.sources, args.evidence
    names = ['pvr_rogue_fwif.h','pvr_rogue_fwif_check.h','pvr_rogue_cr_defs.h',
             'pvr_fw_mips.c','pvr_device.c','pvr_power.c']
    sources = {name:(root/name).read_text() for name in names}
    hashes = {name:hashlib.sha256((root/name).read_bytes()).hexdigest() for name in names}
    manifest = json.loads((root.parent/'manifest.json').read_text())
    for name,digest in hashes.items():
        rows = [r for r in manifest if r['path']=='imagination/'+name]
        assert len(rows)==1 and rows[0]['sha256']==digest
    fwif = sources['pvr_rogue_fwif.h']
    code = ('/* SPDX-License-Identifier: MIT\n'
            ' * Copyright (c) 2023 Imagination Technologies Ltd.\n'
            ' * Retained terms: LICENSE.pvr. */\n'+SHIM)
    for name in ['rogue_fwif_compchecks_bvnc','rogue_fwif_init_options','rogue_fwif_compchecks']:
        code += re.search(r'struct '+name+r' \{.*?\} __aligned\(8\);',fwif,re.S).group()+'\n'
        code += '\n'.join(line for line in sources['pvr_rogue_fwif_check.h'].splitlines()
            if line.startswith(('OFFSET_CHECK(', 'SIZE_CHECK(')) and 'struct '+name+',' in line)+'\n'
    code += re.search(r'^#define ROGUE_FWIF_COMPCHECKS_LAYOUT_VERSION .*$',fwif,re.M).group()+'\n'
    code += re.search(r'static inline void rogue_fwif_compchecks_bvnc_init\(.*?\n\}',fwif,re.S).group()+'\n'
    code += function(sources['pvr_fw_mips.c'],'pvr_mips_irq_pending','static bool')
    code += function(sources['pvr_fw_mips.c'],'pvr_mips_irq_clear','static void')+ENTRY
    assert 'OFFSET_CHECK(struct rogue_fwif_osinit, rogue_comp_checks, 40);' in sources['pvr_rogue_fwif_check.h']
    assert 'ROGUE_FWIF_KCCB_CMD_HEALTH_CHECK' in sources['pvr_power.c']
    original_handler = function(sources['pvr_device.c'],'pvr_device_irq_thread_handler','int')
    assert original_handler.index('pvr_fw_irq_clear(')<original_handler.index('pvr_fwccb_process(')
    (evidence/'pvr_rogue_cr_defs.h').write_text(sources['pvr_rogue_cr_defs.h'])
    (evidence/'LICENSE.pvr').write_bytes((Path(__file__).resolve().parents[1]/'drivers/gpu/LICENSE.pvr').read_bytes())
    reference=evidence/'reference.c'; reference.write_text(code)
    command=['clang-21','-shared','-fPIC','-O2','-Wall','-Wextra','-Werror',str(reference),'-o',str(evidence/'reference.so')]
    subprocess.run(command,check=True)
    lib=C.CDLL(str(evidence/'reference.so'))
    lib.fixture.argtypes=[C.c_void_p]; lib.fixture.restype=None
    lib.irq.argtypes=[C.c_uint32,C.c_uint32,C.POINTER(C.c_uint32)]; lib.irq.restype=None
    data=(C.c_uint8*64)(); lib.fixture(data)
    assert bytes(data)==(evidence/'compatibility.bin').read_bytes()
    events=json.loads((evidence/'irq.json').read_text())
    for event in events:
        out=(C.c_uint32*3)(); lib.irq(event['write'],event['value'] if not event['write'] else 0,out)
        assert list(out)==[event['write'],event['offset'],event['value']]
    old=json.loads((args.queue_evidence/'comparison.json').read_text())
    old_region=(args.queue_evidence/'region-1.bin').read_bytes()
    # The saved queue comparison already checked original command publication.
    expected_hash=old['outputs']['region-1.bin']
    assert hashlib.sha256(old_region).hexdigest()==expected_hash
    assert old_region[:88]==(evidence/'health-command.bin').read_bytes()
    (evidence/'comparison.json').write_text(json.dumps(dict(result='pass',sources=hashes,
        compiler_command=command,irq_operations=len(events),compatibility_bytes=64,
        compatibility_fixture_is_synthetic=True,reused_queue_evidence=str(args.queue_evidence),
        queue_region_sha256=expected_hash,physical_access=False),indent=2)+'\n')
    print('PASS pinned compatibility layout and IRQ functions; health command matches reused publication evidence')


if __name__=='__main__':
    main()
