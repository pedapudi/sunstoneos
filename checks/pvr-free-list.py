#!/usr/bin/env python3
"""Check native free-list ownership and responses against pinned driver code."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
if not __debug__:raise SystemExit('checks require assertions; unset PYTHONOPTIMIZE and do not pass -O')

SHIM = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef uint64_t aligned_u64 __attribute__((aligned(8)));
#define __aligned(n) __attribute__((aligned(n)))
#define ALIGN(x,a) (((x)+(a)-1)&~((a)-1))
#define BIT(n) (1u << (n))
#define BIT_ULL(n) (1ull << (n))
#define GENMASK(h,l) ((~0u << (l)) & (~0u >> (31-(h))))
#include "pvr_rogue_fwif.h"
#define FREE_LIST_ENTRY_SIZE sizeof(u32)
#define FREE_LIST_ALIGNMENT ((ROGUE_BIF_PM_FREELIST_BASE_ADDR_ALIGNSIZE/FREE_LIST_ENTRY_SIZE)-1)
#define lockdep_assert_held(p) ((void)(p))
#define mutex_lock(p) ((void)(p))
#define mutex_unlock(p) ((void)(p))
#define min(a,b) ((a)<(b)?(a):(b))
#define IS_ERR(p) ((void)(p),0)
#define PTR_ERR(p) ((void)(p),-1)
#define WARN_ON_ONCE(p) assert(!(p))
#define BUILD_BUG_ON(p) _Static_assert(!(p),#p)
#define PAGE_SIZE 4096u
#define wmb() ((void)0)
typedef uint64_t dma_addr_t;
struct pvr_free_list {
    void *obj;
    unsigned lock,current_pages,max_pages,grow_pages,grow_threshold,initial_num_pages,fw_id;
    uint64_t free_list_gpu_addr;
};
struct sg_table {const uint64_t *pages;unsigned count;};
struct sg_dma_page_iter {const struct sg_table *table;unsigned index;};
#define for_each_sgtable_dma_page(sgt,it,start) \
    for((it)->table=(sgt),(it)->index=(start);(it)->index<(it)->table->count;++(it)->index)
static uint64_t sg_page_iter_dma_address(const struct sg_dma_page_iter *it)
{return it->table->pages[it->index];}
static void *pvr_gem_object_vmap(void *obj) {return obj;}
static void pvr_gem_object_vunmap(void *obj) {(void)obj;}
'''


def extract(source, name):
    match = re.search(r'static (?:u32|int|void)\n' + name + r'\(', source)
    if not match:
        raise ValueError('Missing reference function: ' + name)
    start = source.index('{', match.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end] + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sources', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--compiler', default='clang-21')
    parser.add_argument('--case', choices=['reference', 'validation'], default='reference')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(__file__).resolve().parent.parent
    manifest = json.loads((args.sources.parent / 'manifest.json').read_text())
    headers = ['pvr_rogue_fwif.h', 'pvr_rogue_defs.h', 'pvr_rogue_cr_defs.h',
               'pvr_rogue_fwif_common.h', 'pvr_rogue_fwif_shared.h',
               'pvr_rogue_fwif_shared_check.h', 'pvr_rogue_fwif_check.h']
    inputs = []
    for name in headers + ['pvr_free_list.c']:
        data = (args.sources / name).read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        pinned = [r for r in manifest if r['path'] == 'imagination/' + name]
        if len(pinned) != 1 or pinned[0]['sha256'] != digest:
            raise ValueError('Pinned source mismatch: ' + name)
        inputs.append(dict(path=str(args.sources / name), sha256=digest))
        if name in headers:
            text = re.sub(r'^#include <linux/[^>]+>\n', '', data.decode(), flags=re.M)
            (args.output / name).write_text(text)
    source = (args.sources / 'pvr_free_list.c').read_text()
    functions = ['calculate_free_list_ready_pages_locked', 'calculate_free_list_ready_pages',
                 'free_list_fw_init', 'pvr_free_list_insert_pages_locked']
    reference = '// SPDX-License-Identifier: GPL-2.0-only OR MIT\n'
    reference += '/* Copyright (c) 2023 Imagination Technologies Ltd. */\n' + SHIM
    reference += '\n'.join(extract(source, name) for name in functions)
    (args.output / 'reference.h').write_text(reference)
    binary = args.output / 'check'
    sources = ['checks/pvr-free-list.c', 'drivers/gpu/pvr_free_list.c',
               'drivers/gpu/pvr_gpu_events.c', 'drivers/gpu/pvr_ccb.c']
    command = [args.compiler, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer',
               '-I' + str(args.output), '-I' + str(root / 'include')]
    command += [str(root / name) for name in sources] + ['-o', str(binary)]
    (args.output / 'compile-command.json').write_text(json.dumps(command, indent=2) + '\n')
    subprocess.run(command, check=True)
    run_command = [str(binary)] + (['validation'] if args.case == 'validation' else [])
    run = subprocess.run(run_command, text=True, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, timeout=10)
    (args.output / 'check.log').write_text(run.stdout)
    for name in sources + ['checks/pvr-free-list.py', 'include/pvr_free_list.h',
                           'include/pvr_gpu_events.h', 'include/pvr_ccb.h']:
        data = (root / name).read_bytes()
        inputs.append(dict(path=str(root / name), sha256=hashlib.sha256(data).hexdigest()))
        target = args.output / 'as-built' / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    expected = 'PASS GPU free-list validation:' if args.case == 'validation' else 'PASS GPU free lists:'
    passed = run.returncode == 0 and expected in run.stdout
    result = dict(passed=passed, case=args.case, returncode=run.returncode, inputs=inputs,
                  binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                  reference_sha256=hashlib.sha256(reference.encode()).hexdigest(),
                  reference_functions=functions,
                  compiler=subprocess.check_output([args.compiler, '--version'], text=True),
                  physical_access=False, firmware_execution=False)
    (args.output / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(run.stdout, end='')
    if not passed:
        raise RuntimeError('GPU free-list check failed')


if __name__ == '__main__':
    main()
