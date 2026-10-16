#!/usr/bin/env python3
"""Compare native firmware state with pinned structure layouts and initializers."""
import argparse
import ctypes as C
import hashlib
import json
from pathlib import Path
import re
import subprocess


def function(source, name):
    start = source.index('\n' + name + '(') + 1
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return 'static void\n' + source[start:end] + '\n'


SHIM = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef uint64_t aligned_u64 __attribute__((aligned(8)));
typedef uint64_t dma_addr_t;
#define __aligned(n) __attribute__((aligned(n)))
#define BIT(n) (1u << (n))
#define BIT_ULL(n) (1ull << (n))
#define GENMASK(h,l) ((~0u << (l)) & (~0u >> (31-(h))))
#define ALIGN(x,a) (((x)+(a)-1)&~((a)-1))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
static bool warn_on(bool value) { assert(!value); return value; }
#define WARN_ON(x) warn_on(x)
#define PVR_FEATURE_VALUE(d,f,p) (*(p) = (d)->f, 0)
#define PVR_ROGUE_FAULT_PAGE_SIZE 4096
#define ROGUE_FWIF_SLC_MIN_SIZE_FOR_DM_OVERLAP_KB 4
#include "pvr_rogue_fwif.h"
#include "pvr_rogue_heap_config.h"

struct pvr_fw_object { u32 address; u64 physical; };
struct pvr_fw_mem {
    struct pvr_fw_object *hwrinfobuf_obj, *osdata_obj, *power_sync_obj,
        *fault_page_obj, *runtime_cfg_obj, *sysdata_obj, *gpu_util_fwcb_obj, *core_data_obj;
};
struct pvr_fw_trace_buffer { struct pvr_fw_object *buf_obj; u32 *buf; };
struct pvr_fw_trace {
    struct pvr_fw_trace_buffer buffers[2];
    u32 group_mask;
    struct pvr_fw_object *tracebuf_ctrl_obj;
};
struct pvr_fw_device { struct pvr_fw_mem mem; struct pvr_fw_trace fw_trace; };
struct pvr_device {
    struct pvr_fw_device fw_dev;
    struct { struct { u32 ctrl_fw_addr, ccb_fw_addr; } ccb; struct pvr_fw_object *rtn_obj; } kccb;
    struct { u32 ctrl_fw_addr, ccb_fw_addr; } fwccb;
    u32 *clk, num_clusters, slc_size_in_kilobytes;
};
static u32 clk_get_rate(u32 *clock) { return *clock; }
static void pvr_fw_object_get_fw_addr(struct pvr_fw_object *o, u32 *out) { *out=o->address; }
static int pvr_fw_object_get_dma_addr(struct pvr_fw_object *o, u32 off, u64 *out)
{ assert(off==0); *out=o->physical; return 0; }
'''

ENTRY = r'''
static const u32 object_sizes[19] = {
    sizeof(struct rogue_fwif_ccb_ctl),128*sizeof(struct rogue_fwif_kccb_cmd),128*sizeof(u32),
    sizeof(struct rogue_fwif_ccb_ctl),32*sizeof(struct rogue_fwif_fwccb_cmd),sizeof(u32),
    sizeof(struct rogue_fwif_hwrinfobuf),sizeof(u32),sizeof(struct rogue_fwif_sysdata),4096,
    sizeof(struct rogue_fwif_gpu_util_fwcb),sizeof(struct rogue_fwif_runtime_cfg),
    sizeof(struct rogue_fwif_tracebuf),ROGUE_FW_TRACE_BUF_DEFAULT_SIZE_IN_DWORDS*sizeof(u32),
    ROGUE_FW_TRACE_BUF_DEFAULT_SIZE_IN_DWORDS*sizeof(u32),sizeof(struct rogue_fwif_osdata),
    sizeof(struct rogue_fwif_connection_ctl),sizeof(struct rogue_fwif_osinit),sizeof(struct rogue_fwif_sysinit)
};
u32 object_size(u32 i) { assert(i<19); return object_sizes[i]; }
void reference(void **buffers, const u32 *addresses, const u64 *physical, const u64 *cpu,
               u32 clock, u32 clusters, u32 slc)
{
    struct pvr_fw_object objects[19];
    for(unsigned i=0;i<19;++i) {
        objects[i]=(struct pvr_fw_object){addresses[i],physical[i]};
        memset(buffers[i],0,i<5 ? object_sizes[i] : ALIGN(object_sizes[i],4096));
    }
    struct rogue_fwif_ccb_ctl *host=buffers[0], *firmware=buffers[3];
    host->wrap_mask=127; host->cmd_size=sizeof(struct rogue_fwif_kccb_cmd);
    firmware->wrap_mask=31; firmware->cmd_size=sizeof(struct rogue_fwif_fwccb_cmd);
    struct pvr_device d={.clk=&clock,.num_clusters=clusters,.slc_size_in_kilobytes=slc};
    d.kccb.ccb.ctrl_fw_addr=addresses[0]; d.kccb.ccb.ccb_fw_addr=addresses[1];
    d.kccb.rtn_obj=&objects[2]; d.fwccb.ctrl_fw_addr=addresses[3]; d.fwccb.ccb_fw_addr=addresses[4];
    struct pvr_fw_mem *mem=&d.fw_dev.mem;
    mem->power_sync_obj=&objects[5]; mem->hwrinfobuf_obj=&objects[6];
    mem->sysdata_obj=&objects[8]; mem->fault_page_obj=&objects[9];
    mem->gpu_util_fwcb_obj=&objects[10]; mem->runtime_cfg_obj=&objects[11];
    mem->osdata_obj=&objects[15];
    struct pvr_fw_trace *trace=&d.fw_dev.fw_trace;
    trace->tracebuf_ctrl_obj=&objects[12];
    for(unsigned i=0;i<2;++i) trace->buffers[i]=(struct pvr_fw_trace_buffer){&objects[13+i],(u32 *)(uintptr_t)cpu[13+i]};
    fw_sysdata_init(buffers[8],&d); fw_fault_page_init(buffers[9],NULL);
    fw_gpu_util_fwcb_init(buffers[10],&d); fw_runtime_cfg_init(buffers[11],&d);
    tracebuf_ctrl_init(buffers[12],trace); fw_osdata_init(buffers[15],&d);
    fw_osinit_init(buffers[17],&d); fw_sysinit_init(buffers[18],&d);
}
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('sources', type=Path)
    ap.add_argument('evidence', type=Path)
    args = ap.parse_args()
    root, evidence = args.sources, args.evidence
    headers = ['pvr_rogue_fwif.h', 'pvr_rogue_defs.h', 'pvr_rogue_cr_defs.h',
               'pvr_rogue_fwif_common.h', 'pvr_rogue_fwif_shared.h',
               'pvr_rogue_fwif_shared_check.h', 'pvr_rogue_fwif_check.h',
               'pvr_rogue_heap_config.h']
    names = headers + ['pvr_fw.c', 'pvr_fw_trace.c', 'pvr_fw.h']
    sources = {name: (root / name).read_text() for name in names}
    hashes = {name: hashlib.sha256((root / name).read_bytes()).hexdigest() for name in names}
    manifest = json.loads((root.parent / 'manifest.json').read_text())
    for name, digest in hashes.items():
        records = [r for r in manifest if r['path'] == 'imagination/' + name]
        assert len(records) == 1 and records[0]['sha256'] == digest
    for name in headers:
        (evidence / name).write_text(re.sub(r'^#include <linux/[^>]+>\n', '', sources[name], flags=re.M))
    assert 'KCCB_NUMCMDS_LOG2_DEFAULT 7' in sources['pvr_fw.h']
    assert 'FWCCB_NUMCMDS_LOG2 5' in sources['pvr_fw.h']
    assert '#define PVR_ROGUE_FW_CONFIG_HEAP_GRANULARITY SZ_64K' in sources['pvr_fw.c']
    assert '#define PVR_ROGUE_FW_CONFIG_HEAP_SIZE (3 * PVR_ROGUE_FW_CONFIG_HEAP_GRANULARITY)' in sources['pvr_fw.c']
    original = ''.join(function(sources['pvr_fw.c'], name) for name in [
        'fw_osinit_init', 'fw_osdata_init', 'fw_fault_page_init', 'fw_sysinit_init',
        'fw_sysdata_init', 'fw_runtime_cfg_init', 'fw_gpu_util_fwcb_init'])
    original += function(sources['pvr_fw_trace.c'], 'tracebuf_ctrl_init')
    reference = evidence / 'reference.c'
    reference.write_text('/* Retained reference license: GPL-2.0-only OR MIT.\n'
                         ' * Copyright (c) 2023 Imagination Technologies Ltd. */\n' + SHIM + original + ENTRY)
    command = ['clang-21', '-shared', '-fPIC', '-O2', '-Wall', '-Wextra', '-Werror',
               '-Wno-unused-parameter', '-Wno-sign-compare',
               str(reference), '-o', str(evidence / 'reference.so')]
    subprocess.run(command, check=True)
    fixture = json.loads((evidence / 'fixture.json').read_text())
    resources = fixture['resources']
    assert len(resources) == 19 and fixture['clusters'] == 1 and fixture['slc_kilobytes'] == 16
    library = C.CDLL(str(evidence / 'reference.so'))
    library.object_size.argtypes = [C.c_uint32]; library.object_size.restype = C.c_uint32
    buffers = []
    next_address = 0xc0100000
    for i, resource in enumerate(resources):
        size = library.object_size(i)
        allocation = (size + 4095) & ~4095
        assert resource['bytes'] == size and resource['allocation_bytes'] == allocation
        if i < 16:
            assert resource['address'] == next_address
            next_address += allocation
        else:
            assert resource['address'] == 0xc0fd0000 + (i - 16) * 65536
        buffers.append((C.c_uint8 * allocation)(*([0xa5] * allocation)))
    library.reference.argtypes = [C.POINTER(C.c_void_p), C.POINTER(C.c_uint32),
        C.POINTER(C.c_uint64), C.POINTER(C.c_uint64), C.c_uint32, C.c_uint32, C.c_uint32]
    library.reference.restype = None
    library.reference((C.c_void_p * 19)(*[C.addressof(b) for b in buffers]),
        (C.c_uint32 * 19)(*[r['address'] for r in resources]),
        (C.c_uint64 * 19)(*[r['physical'] for r in resources]),
        (C.c_uint64 * 19)(*[r['cpu'] for r in resources]),
        fixture['clock_hz'], fixture['clusters'], fixture['slc_kilobytes'])
    outputs = {}
    for i, buffer in enumerate(buffers):
        name = f'resource-{i}.bin'
        assert (evidence / name).read_bytes() == bytes(buffer), name
        outputs[name] = hashlib.sha256(bytes(buffer)).hexdigest()
    (evidence / 'comparison.json').write_text(json.dumps(dict(result='pass', sources=hashes,
        compiler_command=command, outputs=outputs, resource_count=19,
        allocation_bytes=sum(len(b) for b in buffers), physical_access=False,
        clock_is_synthetic=True), indent=2) + '\n')
    print('PASS all 19 resource allocations match pinned firmware layouts and initialization functions')


if __name__ == '__main__':
    main()
