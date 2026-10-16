#!/usr/bin/env python3
"""Compare offline GPU translations with pinned driver entry encoders."""
import argparse
import ctypes as C
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess


def function(source, name, result='void'):
    start = source.index('\n' + name + '(') + 1
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return 'static ' + result + '\n' + source[start:end] + '\n'


SHIM = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint64_t dma_addr_t;
typedef uint64_t aligned_u64 __attribute__((aligned(8)));
#define __aligned(n) __attribute__((aligned(n)))
#define typeof_member(type, member) __typeof__(((type *)0)->member)
#define WRITE_ONCE(destination, value) ((destination) = (value))
#define OFFSET_CHECK(t,f,n) _Static_assert(offsetof(t,f)==(n), #f)
#define SIZE_CHECK(t,n) _Static_assert(sizeof(t)==(n), #t)
#include "pvr_rogue_mmu_defs.h"
struct pvr_page_table_l2_entry_raw { u32 val; };
struct pvr_page_table_l1_entry_raw { u64 val; };
struct pvr_page_table_l0_entry_raw { u64 val; };
struct pvr_page_flags_raw { struct pvr_page_table_l0_entry_raw val; };
struct pvr_vm_context { u64 root; };
static u64 pvr_vm_get_page_table_root_addr(struct pvr_vm_context *vm) { return vm->root; }
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('sources', type=Path)
    ap.add_argument('evidence', type=Path)
    args = ap.parse_args()
    root, evidence = args.sources, args.evidence
    names = ['pvr_mmu.c', 'pvr_vm.c', 'pvr_rogue_mmu_defs.h',
             'pvr_rogue_fwif.h', 'pvr_rogue_fwif_check.h']
    sources = {name: (root / name).read_text() for name in names}
    hashes = {name: hashlib.sha256((root / name).read_bytes()).hexdigest() for name in names}
    manifest = json.loads((root.parent / 'manifest.json').read_text())
    for name, digest in hashes.items():
        records = [r for r in manifest if r['path'] == 'imagination/' + name]
        assert len(records) == 1 and records[0]['sha256'] == digest
    (evidence / 'pvr_rogue_mmu_defs.h').write_text(sources['pvr_rogue_mmu_defs.h'])
    mmu = sources['pvr_mmu.c']
    macros = mmu[mmu.index('#define PVR_PAGE_TABLE_TYPEOF_ENTRY'):]
    macros = macros[:macros.index('/**')]
    context = re.search(r'struct rogue_fwif_fwmemcontext \{.*?\} __aligned\(8\);',
                        sources['pvr_rogue_fwif.h'], re.S).group()
    invalid = re.search(r'^#define ROGUE_FW_BIF_INVALID_PCSET .*$',
                        sources['pvr_rogue_fwif.h'], re.M).group()
    assertions = '\n'.join(line for line in sources['pvr_rogue_fwif_check.h'].splitlines()
                           if line.startswith(('OFFSET_CHECK(', 'SIZE_CHECK('))
                           and 'struct rogue_fwif_fwmemcontext,' in line)
    assert assertions.count('CHECK(') == 7
    code = ('/* SPDX-License-Identifier: MIT\n'
            ' * Copyright (c) 2023 Imagination Technologies Ltd.\n'
            ' * Retained terms: LICENSE.pvr. */\n' + SHIM + macros + context + '\n' +
            invalid + '\n' + assertions + '\n')
    code += function(mmu, 'pvr_page_table_l2_entry_raw_set')
    code += function(mmu, 'pvr_page_flags_raw_create', 'struct pvr_page_flags_raw')
    code += function(sources['pvr_vm.c'], 'fw_mem_context_init')
    code += '''
u64 root_entry(u64 pa) {
    struct pvr_page_table_l2_entry_raw entry;
    pvr_page_table_l2_entry_raw_set(&entry,pa); return entry.val;
}
void context_init(void *output, u64 pa) {
    struct rogue_fwif_fwmemcontext record = {0};
    struct pvr_vm_context vm = {pa}; fw_mem_context_init(&record,&vm);
    memcpy(output,&record,sizeof(record));
}
'''
    sizes = ['4KB', '16KB', '64KB', '256KB', '1MB', '2MB']
    for i, size in enumerate(sizes):
        code += f'''\n#define ROGUE_MMUCTRL_PAGE_SIZE_X ROGUE_MMUCTRL_PAGE_SIZE_{size}
#define ROGUE_MMUCTRL_PAGE_X_RANGE_CLRMSK ROGUE_MMUCTRL_PAGE_{size}_RANGE_CLRMSK
'''
        for level in (1, 0):
            name = f'pvr_page_table_l{level}_entry_raw_set'
            code += function(mmu, name).replace(name, name + '_' + str(i))
        code += f'''
u64 directory_{i}(u64 pa) {{
    struct pvr_page_table_l1_entry_raw entry;
    pvr_page_table_l1_entry_raw_set_{i}(&entry,pa); return entry.val;
}}
u64 leaf_{i}(u64 pa, u32 flags) {{
    struct pvr_page_table_l0_entry_raw entry;
    struct pvr_page_flags_raw options = pvr_page_flags_raw_create(
        flags & 1, flags & 2, flags & 4, flags & 8);
    pvr_page_table_l0_entry_raw_set_{i}(&entry,pa,options); return entry.val;
}}
#undef ROGUE_MMUCTRL_PAGE_SIZE_X
#undef ROGUE_MMUCTRL_PAGE_X_RANGE_CLRMSK
'''
    (evidence / 'LICENSE.pvr').write_bytes((Path(__file__).resolve().parents[1] /
                                          'drivers/gpu/LICENSE.pvr').read_bytes())
    reference = evidence / 'reference.c'
    reference.write_text(code)
    command = ['clang-21', '-shared', '-fPIC', '-O2', '-Wall', '-Wextra', '-Werror',
               str(reference), '-o', str(evidence / 'reference.so')]
    subprocess.run(command, check=True)
    lib = C.CDLL(str(evidence / 'reference.so'))
    lib.root_entry.argtypes = [C.c_uint64]; lib.root_entry.restype = C.c_uint64
    lib.context_init.argtypes = [C.c_void_p, C.c_uint64]; lib.context_init.restype = None
    for i in range(6):
        directory = getattr(lib, f'directory_{i}')
        directory.argtypes = [C.c_uint64]; directory.restype = C.c_uint64
        leaf = getattr(lib, f'leaf_{i}')
        leaf.argtypes = [C.c_uint64, C.c_uint32]; leaf.restype = C.c_uint64
    fixture = json.loads((evidence / 'fixture.json').read_text())
    physical = fixture['table_pages']
    pages = [bytearray(4096)]
    directories, leaves = {}, {}
    used_sizes = set()
    for m in fixture['maps']:
        shift = m['page_shift']
        code = [12, 14, 16, 18, 20, 21].index(shift)
        used_sizes.add(code)
        for offset in range(0, m['bytes'], 1 << shift):
            address = m['address'] + offset
            pc, pd = address // (1 << 30), address // (1 << 21) % 512
            if pc not in directories:
                directories[pc] = len(pages)
                struct.pack_into('<I', pages[0], pc * 4, lib.root_entry(physical[len(pages)]))
                pages.append(bytearray(4096))
            if (pc, pd) not in leaves:
                leaves[pc, pd] = len(pages)
                struct.pack_into('<Q', pages[directories[pc]], pd * 8,
                                 getattr(lib, f'directory_{code}')(physical[len(pages)]))
                pages.append(bytearray(4096))
            index = address % (1 << 21) // (1 << shift)
            struct.pack_into('<Q', pages[leaves[pc, pd]], index * 8,
                             getattr(lib, f'leaf_{code}')(m['physical'] + offset, m['flags']))
    assert used_sizes == set(range(6)) and len(pages) == len(physical)
    expected_tables = b''.join(pages)
    expected_context = (C.c_uint8 * 32)()
    lib.context_init(expected_context, physical[0])
    outputs = {}
    for name, data in [('tables.bin', expected_tables), ('context.bin', bytes(expected_context))]:
        assert (evidence / name).read_bytes() == data, name
        outputs[name] = hashlib.sha256(data).hexdigest()
    (evidence / 'comparison.json').write_text(json.dumps(dict(result='pass', sources=hashes,
        compiler_command=command, outputs=outputs, page_sizes=sizes, table_pages=len(pages),
        directory_pages=len(directories), leaf_tables=len(leaves), physical_access=False), indent=2) + '\n')
    print('PASS complete GPU tables and firmware context match pinned encoders for all six page sizes')


if __name__ == '__main__':
    main()
