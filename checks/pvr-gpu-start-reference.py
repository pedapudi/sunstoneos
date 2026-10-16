#!/usr/bin/env python3
"""Compare native startup accesses with the pinned complete MIPS startup path."""
import argparse
import ctypes as C
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess


def function(source, name, result):
    start = source.index('\n' + name + '(') + 1
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
#include <errno.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#define BIT(n) (1u<<(n))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define PVR_FW_PROCESSOR_TYPE_MIPS 1
#define PVR_FW_PROCESSOR_TYPE_RISCV 2
#define PVR_FEATURE_VALUE(d,f,p) (*(p)=(d)->f,0)
#define PVR_HAS_FEATURE(d,f) ((d)->f)
#define PVR_HAS_QUIRK(d,q) ((q)==63553 || ((q)==71242 && (d)->quirk71242))
static bool warn_on(bool v) { assert(!v); return v; }
#define WARN_ON(v) warn_on(v)
#include "pvr_rogue_cr_defs.h"
#include "pvr_rogue_mips.h"
struct pvr_fw_mips_data { u64 boot_code_dma_addr,boot_data_dma_addr,exception_code_dma_addr; };
struct pvr_device;
struct fw_defs { int (*wrapper_init)(struct pvr_device *); };
struct pvr_device {
    struct { int processor_type; const struct fw_defs *defs;
        struct { struct pvr_fw_mips_data *mips_data; } processor_data; } fw_dev;
    u32 phys_bus_width,slc_cache_line_size_bits;
    bool pbe2_in_xe,sys_bus_secure_reset,xe_tpu2,gpu_multicore_support,quirk71242;
};
struct event { u32 kind,offset,width; u64 value; };
static struct event events[64];
static u32 count;
static void event(u32 kind,u32 offset,u32 width,u64 value) {
    assert(count<64); events[count++]=(struct event){kind,offset,width,value};
}
static void pvr_cr_write32(struct pvr_device *d,u32 off,u32 v) { (void)d; event(1,off,4,v); }
static void pvr_cr_write64(struct pvr_device *d,u32 off,u64 v) { (void)d; event(1,off,8,v); }
static u32 pvr_cr_read32(struct pvr_device *d,u32 off) {
    (void)d; u32 v=off==ROGUE_CR_SLC_CTRL_MISC ? 0xf1234567 : 0; event(2,off,4,v); return v;
}
static u64 pvr_cr_read64(struct pvr_device *d,u32 off) { (void)d; event(2,off,8,0); return 0; }
static void udelay(u32 us) { event(3,0,0,us); }
static void rogue_bif_init(struct pvr_device *d) { (void)d; assert(0); }
'''
ENTRY = r'''
u32 reference(u32 flags,u32 line,u64 code,u64 data,u64 exceptions,struct event *out) {
    struct pvr_fw_mips_data m={code,data,exceptions};
    const struct fw_defs defs={pvr_mips_wrapper_init};
    struct pvr_device d={.fw_dev={PVR_FW_PROCESSOR_TYPE_MIPS,&defs,{&m}},
        .phys_bus_width=36,.slc_cache_line_size_bits=line,
        .pbe2_in_xe=flags&1,.sys_bus_secure_reset=flags&2,.xe_tpu2=flags&4,
        .gpu_multicore_support=flags&8,.quirk71242=flags&16};
    count=0; assert(!pvr_fw_start(&d));
    for(u32 i=0;i<count;++i) out[i]=events[i];
    return count;
}
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('sources', type=Path)
    ap.add_argument('firmware', type=Path)
    ap.add_argument('evidence', type=Path)
    args = ap.parse_args()
    root, evidence = args.sources, args.evidence
    names = ['pvr_fw_startstop.c', 'pvr_fw_mips.c', 'pvr_rogue_cr_defs.h',
             'pvr_rogue_mips.h', 'pvr_rogue_mips_check.h', 'pvr_rogue_defs.h',
             'pvr_rogue_fwif_dev_info.h', 'pvr_device_info.c', 'pvr_fw.c',
             'pvr_rogue_fwif_check.h']
    sources = {name: (root / name).read_text() for name in names}
    hashes = {name: hashlib.sha256((root / name).read_bytes()).hexdigest() for name in names}
    manifest = json.loads((root.parent / 'manifest.json').read_text())
    for name, digest in hashes.items():
        matches = [r for r in manifest if r['path'] == 'imagination/' + name]
        assert len(matches) == 1 and matches[0]['sha256'] == digest
    for name in ['pvr_rogue_cr_defs.h', 'pvr_rogue_mips.h', 'pvr_rogue_mips_check.h']:
        (evidence / name).write_text(re.sub(r'^#include <linux/[^>]+>\n','',sources[name],flags=re.M))
    definitions = sources['pvr_rogue_defs.h']
    masks = definitions[definitions.index('#define ROGUE_CR_SOFT_RESET_DUST_n_CORE_EN'):]
    masks = masks[:masks.index('/* SOFT_RESET steps')]
    code = ('/* SPDX-License-Identifier: MIT\n'
            ' * Copyright (c) 2023 Imagination Technologies Ltd.\n'
            ' * Retained terms: LICENSE.pvr. */\n' + SHIM + masks)
    code += function(sources['pvr_fw_mips.c'],'pvr_mips_wrapper_init','static int')
    for name, result in [('rogue_axi_ace_list_init','static void'),('rogue_slc_init','static int'),
                         ('pvr_fw_start','int')]:
        code += function(sources['pvr_fw_startstop.c'],name,result)
    code += ENTRY
    reference = evidence / 'reference.c'; reference.write_text(code)
    (evidence / 'LICENSE.pvr').write_bytes((Path(__file__).resolve().parents[1] /
                                          'drivers/gpu/LICENSE.pvr').read_bytes())
    command = ['clang-21','-shared','-fPIC','-O2','-Wall','-Wextra','-Werror',
               str(reference),'-o',str(evidence/'reference.so')]
    subprocess.run(command,check=True)
    class Event(C.Structure):
        _fields_ = [('kind',C.c_uint32),('offset',C.c_uint32),('width',C.c_uint32),('value',C.c_uint64)]
    lib = C.CDLL(str(evidence/'reference.so'))
    lib.reference.argtypes = [C.c_uint32,C.c_uint32,C.c_uint64,C.c_uint64,C.c_uint64,C.POINTER(Event)]
    lib.reference.restype = C.c_uint32
    names = re.findall(r'PVR_FW_HAS_FEATURE_([A-Z0-9_]+)', sources['pvr_rogue_fwif_dev_info.h'])[:-1]
    params = set(re.findall(r'FEATURE_MAPPING_VALUE\(([A-Z0-9_]+),',sources['pvr_device_info.c']))
    assert 'OFFSET_CHECK(struct rogue_fwif_sysinit, firmware_started, 352);' in sources['pvr_rogue_fwif_check.h']
    firmware = args.firmware.read_bytes()
    assert hashlib.sha256(firmware).hexdigest() == '1db1c399c17401d1f79d46c880db81c724d748c784d4639433b076aba2f9c0d2'
    results = []
    for variant, raw in enumerate([firmware,(evidence/'variant.fw').read_bytes()]):
        masks = struct.unpack_from('<QQ',raw,122880+48)
        parameters = iter(struct.unpack_from('<29Q',raw,122880+64))
        enabled = {name for bit,name in enumerate(names) if masks[bit//64]>>(bit%64)&1}
        values = {name:next(parameters) for name in names if name in enabled and name in params}
        assert next(parameters,None) is None
        quirks = struct.unpack_from('<Q',raw,122880+32)[0]
        flags = sum(1<<i for i,name in enumerate(['PBE2_IN_XE','SYS_BUS_SECURE_RESET',
            'XE_TPU2','GPU_MULTICORE_SUPPORT']) if name in enabled) | (16 if quirks & (1<<11) else 0)
        native = json.loads((evidence/f'trace-{variant}.json').read_text())
        out = (Event*64)()
        n = lib.reference(flags,values['SLC_CACHE_LINE_SIZE_BITS'],native['code'],native['data'],native['exceptions'],out)
        expected = [{key:getattr(e,key) for key in ['kind','offset','width','value']} for e in out[:n] if e.kind != 3]
        actual = [{key:e[key] for key in ['kind','offset','width','value']} for e in native['events']]
        assert expected == actual, (variant,expected,actual)
        assert [e.value for e in out[:n] if e.kind==3] == [3,3]
        results.append(dict(variant=variant,synthetic=bool(variant),features=flags,
            line_bits=values['SLC_CACHE_LINE_SIZE_BITS'],access_count=len(actual),
            firmware_sha256=hashlib.sha256(raw).hexdigest(),trace_sha256=hashlib.sha256((evidence/f'trace-{variant}.json').read_bytes()).hexdigest()))
    (evidence/'comparison.json').write_text(json.dumps(dict(result='pass',sources=hashes,
        compiler_command=command,variants=results,physical_access=False),indent=2)+'\n')
    print('PASS complete startup register traces match pinned original functions for firmware and synthetic feature variant')


if __name__ == '__main__':
    main()
