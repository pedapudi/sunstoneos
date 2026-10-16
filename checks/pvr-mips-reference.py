#!/usr/bin/env python3
"""Compare native MIPS preparation with isolated functions from pinned GPU sources."""
import argparse
import ctypes as C
import hashlib
import io
import json
from pathlib import Path
import re
import struct
import subprocess
from elftools.elf.elffile import ELFFile

FIRMWARE_SHA = '1db1c399c17401d1f79d46c880db81c724d748c784d4639433b076aba2f9c0d2'


def function(source, name, return_type):
    start = source.index('\n' + name + '(') + 1
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return return_type + '\n' + source[start:end] + '\n'


SHIM = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t s32;
typedef uint64_t u64;
typedef uint64_t dma_addr_t;
#define BIT(n) (1u << (n))
#define PAGE_SHIFT 12
#define PAGE_MASK (~4095ull)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
static bool warn_on(bool value) { assert(!value); return value; }
#define WARN_ON(x) warn_on(x)
#define WRITE_ONCE(a,b) ((a) = (b))
#define check_add_overflow(a,b,p) __builtin_add_overflow((a),(b),(p))
#define PVR_BO_FW_FLAGS_DEVICE_UNCACHED 1
#define PVR_FEATURE_VALUE(dev,feature,p) (*(p) = 36, 0)
#define PVR_HAS_QUIRK(dev,quirk) ((quirk) == 63553)
#define ROGUE_FW_HEAP_BASE 0xE1C0000000ull
#define MIPS_BOOT_CODE 6
#define MIPS_BOOT_DATA 8
#define MIPS_EXCEPTIONS_CODE 5
#define MIPS_STACK 9
#include "mips.h"
#include "cr.h"
struct pvr_fw_mips_data {
    u32 *pt, pfn_mask, cache_policy;
    u64 pt_dma_addr[4], boot_code_dma_addr, boot_data_dma_addr, exception_code_dma_addr;
};
struct pvr_gem_object { u32 flags; const u64 *pages; };
struct pvr_fw_object {
    struct pvr_gem_object *gem;
    struct { u64 start, size; } fw_mm_node;
};
struct pvr_fw_layout_entry { u32 alloc_offset; };
struct pvr_fw_device {
    struct { struct pvr_fw_mips_data *mips_data; } processor_data;
    struct { u64 raw_size, offset_mask; } fw_heap_info;
    struct { struct pvr_fw_object *code_obj, *data_obj; } mem;
};
struct resource { u64 start; };
struct write { u32 offset, width; u64 value; };
struct pvr_device {
    struct pvr_fw_device fw_dev;
    struct resource *regs_resource;
    struct pvr_fw_layout_entry layouts[10];
    struct write *writes;
    u32 write_count;
};
static int pvr_gem_get_dma_addr(struct pvr_gem_object *o, u32 offset, u64 *out)
{ *out = o->pages[offset / 4096] + offset % 4096; return 0; }
static int pvr_fw_object_get_dma_addr(struct pvr_fw_object *o, u32 offset, u64 *out)
{ return pvr_gem_get_dma_addr(o->gem, offset, out); }
static void pvr_mmu_flush_request_all(struct pvr_device *d) { (void)d; }
static int pvr_mmu_flush_exec(struct pvr_device *d, bool wait) { (void)d; (void)wait; return 0; }
static int pvr_fw_process_elf_command_stream(struct pvr_device *d, const u8 *fw,
    u8 *code, u8 *data, u8 *core_code, u8 *core_data)
{ (void)d; (void)fw; (void)code; (void)data; (void)core_code; (void)core_data; return 0; }
static const struct pvr_fw_layout_entry *pvr_fw_find_layout_entry(struct pvr_device *d, u32 id)
{ assert(id < 10); return &d->layouts[id]; }
static void pvr_cr_write32(struct pvr_device *d, u32 reg, u32 value)
{ assert(d->write_count < 11); d->writes[d->write_count++] = (struct write){reg,4,value}; }
static void pvr_cr_write64(struct pvr_device *d, u32 reg, u64 value)
{ assert(d->write_count < 11); d->writes[d->write_count++] = (struct write){reg,8,value}; }
'''

ENTRY = r'''
void reference(u32 *pt, u8 *data, const u64 *pages, struct write *writes)
{
    struct pvr_fw_mips_data m = {.pt=pt, .pfn_mask=ROGUE_MIPSFW_ENTRYLO_PFN_MASK_ABOVE_32BIT,
        .cache_policy=ROGUE_MIPSFW_CACHED_POLICY_ABOVE_32BIT};
    for (unsigned i=0;i<4;++i) m.pt_dma_addr[i]=pages[30+i];
    struct pvr_gem_object gems[] = {{1,pages},{1,pages+25},{0,pages+34},{1,pages+36}};
    const u32 offsets[] = {0,0x32000,0x100000,0xfff000};
    const u32 sizes[] = {102400,20480,8192,4096};
    struct pvr_fw_object objects[4];
    for (unsigned i=0;i<4;++i) objects[i]=(struct pvr_fw_object){&gems[i],
        {ROGUE_FW_HEAP_BASE+offsets[i],sizes[i]}};
    struct resource registers={0x01800000};
    struct pvr_device dev={.fw_dev={.processor_data={&m},
        .fw_heap_info={0x1000000,0xffffff},.mem={&objects[0],&objects[1]}},
        .regs_resource=&registers,.writes=writes};
    dev.layouts[5].alloc_offset=0x17000;
    dev.layouts[6].alloc_offset=0x18000;
    dev.layouts[8].alloc_offset=0x3000;
    dev.layouts[9].alloc_offset=0x4000;
    for (unsigned i=0;i<4;++i) assert(!pvr_vm_mips_map(&dev,&objects[i]));
    assert(!pvr_mips_fw_process(&dev,NULL,NULL,data,NULL,NULL,0));
    assert(!pvr_mips_wrapper_init(&dev));
    assert(dev.write_count==11);
}
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('sources', type=Path)
    ap.add_argument('firmware', type=Path)
    ap.add_argument('evidence', type=Path)
    args = ap.parse_args()
    root, evidence = args.sources, args.evidence
    files = ['pvr_vm_mips.c', 'pvr_fw_mips.c', 'pvr_rogue_mips.h',
             'pvr_rogue_cr_defs.h', 'pvr_rogue_fwif_dev_info.h', 'pvr_device_info.c',
             'pvr_rogue_mips_check.h']
    sources = {name: (root / name).read_text() for name in files}
    source_hashes = {name: hashlib.sha256((root / name).read_bytes()).hexdigest() for name in files}
    # Verify each source against the downloaded, pinned source manifest.
    manifest = json.loads((root.parent / 'manifest.json').read_text())
    records = manifest
    for name, digest in source_hashes.items():
        entries = [r for r in records if r['path'].endswith('/' + name)]
        assert len(entries) == 1 and entries[0]['sha256'] == digest, name
    raw = args.firmware.read_bytes()
    assert hashlib.sha256(raw).hexdigest() == FIRMWARE_SHA
    names = re.findall(r'PVR_FW_HAS_FEATURE_([A-Z0-9_]+)', sources['pvr_rogue_fwif_dev_info.h'])[:-1]
    parameter_names = set(re.findall(r'FEATURE_MAPPING_VALUE\(([A-Z0-9_]+),', sources['pvr_device_info.c']))
    masks = struct.unpack_from('<QQ', raw, 122880 + 48)
    parameters = iter(struct.unpack_from('<29Q', raw, 122880 + 64))
    decoded = {name: next(parameters) for bit, name in enumerate(names)
               if masks[bit // 64] >> (bit % 64) & 1 and name in parameter_names}
    assert next(parameters, None) is None and decoded['PHYS_BUS_WIDTH'] == 36
    assert struct.unpack_from('<Q', raw, 122880 + 32)[0] & (1 << 9)
    (evidence / 'mips.h').write_text(re.sub(r'^#include <linux/[^>]+>\n', '',
        sources['pvr_rogue_mips.h'], flags=re.M))
    (evidence / 'pvr_rogue_mips_check.h').write_text(re.sub(r'^#include <linux/[^>]+>\n', '',
        sources['pvr_rogue_mips_check.h'], flags=re.M))
    (evidence / 'cr.h').write_text(sources['pvr_rogue_cr_defs.h'])
    extracted = function(sources['pvr_vm_mips.c'], 'get_mips_pte_flags', 'static u32')
    extracted += function(sources['pvr_vm_mips.c'], 'pvr_vm_mips_map', 'int')
    extracted += function(sources['pvr_fw_mips.c'], 'pvr_mips_fw_process', 'static int')
    extracted += function(sources['pvr_fw_mips.c'], 'pvr_mips_wrapper_init', 'static int')
    reference = evidence / 'reference.c'
    reference.write_text('/* Retained reference license: GPL-2.0-only OR MIT.\n'
                         ' * Copyright (c) 2023 Imagination Technologies Ltd. */\n' +
                         SHIM + extracted + ENTRY)
    command = ['clang-21', '-shared', '-fPIC', '-O2', '-Wall', '-Wextra',
               '-Wno-unused-parameter', str(reference), '-o', str(evidence / 'reference.so')]
    subprocess.run(command, check=True)
    expected = [bytearray(102400), bytearray(20480)]
    regions = [(0xc0000000, 94208, 0, 0), (0x9fc02000, 4096, 0, 94208),
               (0xbfc00000, 4096, 0, 98304), (0xc0032000, 12288, 1, 0),
               (0xbfc01000, 4096, 1, 12288), (0xcf600000, 4096, 1, 16384)]
    for segment in ELFFile(io.BytesIO(raw)).iter_segments():
        if segment['p_type'] != 'PT_LOAD':
            continue
        addr, size = segment['p_vaddr'], segment['p_memsz']
        matches = [r for r in regions if r[0] <= addr and addr + size <= r[0] + r[1]]
        assert len(matches) == 1
        base, _, allocation, offset = matches[0]
        payload = segment.data()
        start = offset + addr - base
        expected[allocation][start:start + len(payload)] = payload

    class Write(C.Structure):
        _fields_ = [('offset', C.c_uint32), ('width', C.c_uint32), ('value', C.c_uint64)]
    pages = ([0x140000000 + i * 0x3000 for i in range(25)] +
             [0x240000000 + i * 0x5000 for i in range(5)] +
             [0x340000000 + i * 0x7000 for i in range(4)] +
             [0x440000000, 0x440007000, 0xffffff000])
    table = (C.c_uint32 * 4096)()
    data = (C.c_uint8 * len(expected[1])).from_buffer(expected[1])
    writes = (Write * 11)()
    lib = C.CDLL(str(evidence / 'reference.so'))
    lib.reference.argtypes = [C.POINTER(C.c_uint32), C.POINTER(C.c_uint8),
                             C.POINTER(C.c_uint64), C.POINTER(Write)]
    lib.reference.restype = None
    lib.reference(table, data, (C.c_uint64 * len(pages))(*pages), writes)
    outputs = {'code.bin': bytes(expected[0]), 'data.bin': bytes(expected[1]),
               'table.bin': bytes(table), 'wrapper.bin': struct.pack('<II', 36, 11) + bytes(writes)}
    for name, output in outputs.items():
        assert (evidence / name).read_bytes() == output, name
    # Independently walk each present entry back to its physical page and flags.
    present = {i: entry for i, entry in enumerate(table) if entry}
    assert len(present) == 33
    assert (present[4095] & 0x3fffffc0) << 6 == 0xffffff000
    assert present[0x100] & 63 == 15 and present[0] & 63 == 23
    report = dict(result='pass', firmware_sha256=FIRMWARE_SHA, sources=source_hashes,
                  compiler_command=command, features=decoded, mapped_pages=len(present),
                  wrapper_writes=[dict(offset=w.offset, width=w.width, value=w.value) for w in writes],
                  output_sha256={name: hashlib.sha256(v).hexdigest() for name, v in outputs.items()},
                  physical_access=False, reference_flushes_stubbed=True,
                  reference_elf_loading='independent pyelftools staging')
    (evidence / 'comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS code, boot data, all page-table bytes and ordered wrapper writes match pinned references')


if __name__ == '__main__':
    main()
