#!/usr/bin/env python3
"""Compare native MMU cache commands with the pinned driver's complete function."""
import argparse
import ctypes as C
import hashlib
import json
from pathlib import Path
import re
import subprocess

SHIM = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef uint32_t u32;
#define __aligned(n) __attribute__((aligned(n)))
#define EIO 5
#define HZ 100
#define ROGUE_FWIF_KCCB_CMD_MMUCACHE (102u | 0x2abc0000u)
struct pvr_device {
    struct { bool booted; struct { u32 *mmucache_sync_obj; } mem; } fw_dev;
    u32 mmu_flush_cache_flags;
};
static struct pvr_device *from_pvr_device(struct pvr_device *d) { return d; }
static int drm_dev_enter(struct pvr_device *d, int *idx) { (void)d; *idx=1; return 1; }
static void drm_dev_exit(int idx) { assert(idx==1); }
static u32 atomic_xchg(u32 *p,u32 v) { u32 old=*p; *p=v; return old; }
static void pvr_fw_object_get_fw_addr(u32 *object,u32 *out) { *out=*object; }
static int pvr_power_reset(struct pvr_device *d,bool hard) { (void)d;(void)hard;assert(0);return -1; }
static void pvr_device_lost(struct pvr_device *d) { (void)d;assert(0); }
'''
CAPTURE = r'''
struct rogue_fwif_kccb_cmd {
    u32 cmd_type,kccb_flags;
    union { struct rogue_fwif_mmucachedata mmu_cache_data; unsigned char padding[80]; } cmd_data;
} __aligned(8);
_Static_assert(sizeof(struct rogue_fwif_kccb_cmd)==88,"command bytes");
_Static_assert(offsetof(struct rogue_fwif_kccb_cmd,cmd_data)==8,"payload offset");
_Static_assert(sizeof(struct rogue_fwif_mmucachedata)==12,"MMU payload bytes");
static struct rogue_fwif_kccb_cmd captured;
static unsigned sends,waits;
static int pvr_kccb_send_cmd(struct pvr_device *d,struct rogue_fwif_kccb_cmd *cmd,u32 *slot) {
    (void)d; assert(!sends++); captured=*cmd; *slot=17; return 0;
}
static int pvr_kccb_wait_for_completion(struct pvr_device *d,u32 slot,u32 timeout,u32 *result) {
    (void)d; assert(slot==17 && timeout==HZ && !result); ++waits; return 0;
}
'''
ENTRY = r'''
void reference(unsigned level,void *out) {
    static const u32 flags[]={PVR_MMU_SYNC_LEVEL_0_FLAGS,PVR_MMU_SYNC_LEVEL_1_FLAGS,PVR_MMU_SYNC_LEVEL_2_FLAGS};
    assert(level<3); u32 address=0xc0109000;
    struct pvr_device d={{true,{&address}},flags[level]}; sends=waits=0;
    assert(!pvr_mmu_flush_exec(&d,true) && sends==1 && waits==1 && !d.mmu_flush_cache_flags);
    memcpy(out,&captured,88);
}
'''


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('sources',type=Path)
    parser.add_argument('evidence',type=Path)
    args=parser.parse_args()
    names=['pvr_mmu.c','pvr_rogue_fwif.h','pvr_rogue_fwif_check.h']
    sources={name:(args.sources/name).read_text() for name in names}
    hashes={name:hashlib.sha256((args.sources/name).read_bytes()).hexdigest() for name in names}
    manifest=json.loads((args.sources.parent/'manifest.json').read_text())
    for name,digest in hashes.items():
        rows=[r for r in manifest if r['path']=='imagination/'+name]
        assert len(rows)==1 and rows[0]['sha256']==digest
    h=sources['pvr_rogue_fwif.h']; mmu=sources['pvr_mmu.c']
    assert 'ROGUE_FWIF_KCCB_CMD_MMUCACHE = 102U' in h
    assert 'OFFSET_CHECK(struct rogue_fwif_kccb_cmd, cmd_data, 8);' in sources['pvr_rogue_fwif_check.h']
    assert 'SIZE_CHECK(struct rogue_fwif_kccb_cmd, 88);' in sources['pvr_rogue_fwif_check.h']
    macros=h[h.index('#define ROGUE_FWIF_MMUCACHEDATA_FLAGS_PT'):h.index('struct rogue_fwif_mmucachedata')]
    payload=re.search(r'struct rogue_fwif_mmucachedata \{.*?\n\};',h,re.S).group()
    levels=mmu[mmu.index('#define PVR_MMU_SYNC_LEVEL_0_FLAGS'):]
    levels=levels[:levels.index('/**')]
    start=mmu.index('int pvr_mmu_flush_exec('); brace=mmu.index('{',start); depth,end=1,brace+1
    while depth:
        depth+=(mmu[end]=='{')-(mmu[end]=='}'); end+=1
    code=('/* SPDX-License-Identifier: MIT\n * Copyright (c) 2023 Imagination Technologies Ltd.\n'
          ' * Retained terms: LICENSE.pvr. */\n'+SHIM+macros+payload+'\n'+levels+CAPTURE+mmu[start:end]+ENTRY)
    source=args.evidence/'reference.c'; source.write_text(code)
    (args.evidence/'LICENSE.pvr').write_bytes((Path(__file__).resolve().parents[1]/'drivers/gpu/LICENSE.pvr').read_bytes())
    command=['clang-21','-shared','-fPIC','-O2','-Wall','-Wextra','-Werror',str(source),'-o',str(args.evidence/'reference.so')]
    subprocess.run(command,check=True)
    lib=C.CDLL(str(args.evidence/'reference.so')); lib.reference.argtypes=[C.c_uint,C.c_void_p]; lib.reference.restype=None
    native=(args.evidence/'commands.bin').read_bytes(); assert len(native)==3*88
    for level in range(3):
        output=(C.c_ubyte*88)(); lib.reference(level,output)
        assert bytes(output)==native[level*88:(level+1)*88]
    (args.evidence/'reference-result.json').write_text(json.dumps(dict(result='pass',sources=hashes,
        compiler_command=command,compared='Complete commands for all three translation levels; one send and command-return wait.',
        limit='Format/reference protocol agreement only; firmware execution and physical invalidation unqualified.'),indent=2)+'\n')
    print('PASS complete MMU cache commands match pinned driver for leaf, directory, and catalogue changes')


if __name__=='__main__':
    main()
