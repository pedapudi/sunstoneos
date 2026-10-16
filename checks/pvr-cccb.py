#!/usr/bin/env python3
"""Check client command transport against pinned driver functions and real KCCB."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

SHIM = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef uint64_t aligned_u64 __attribute__((aligned(8)));
#define __aligned(n) __attribute__((aligned(n)))
#ifndef __always_inline
#define __always_inline inline
#endif
#define ALIGN(x,a) (((x)+(a)-1)&~((a)-1))
#define IS_ALIGNED(x,a) (((x)&((a)-1))==0)
#define BIT(n) (1u<<(n))
#define BIT_ULL(n) (1ull<<(n))
#define GENMASK(h,l) ((~0u<<(l))&(~0u>>(31-(h))))
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define WARN_ON(x) (assert(!(x)),0)
#include "pvr_rogue_fwif.h"
#define PADDING_COMMAND_SIZE sizeof(struct rogue_fwif_ccb_cmd_header)
#define pvr_cccb reference_cccb
struct reference_cccb {
    struct rogue_fwif_cccb_ctl *ctrl;
    u8 *cccb;
    size_t size;
    u32 write_offset, wrap_mask;
};
struct pvr_hwrt_data {u32 fw_obj;};
static void pvr_fw_object_get_fw_addr_offset(u32 base,u32 offset,u32 *out)
{*out=base+offset;}
'''


def extract(source, prefix):
    begin = source.index(prefix)
    start = source.index('{', begin)
    end, depth = start + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[begin:end] + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sources', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--compiler', default='clang-21')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(__file__).resolve().parent.parent
    manifest = json.loads((args.sources.parent / 'manifest.json').read_text())
    headers = ['pvr_rogue_fwif.h', 'pvr_rogue_defs.h', 'pvr_rogue_cr_defs.h',
               'pvr_rogue_fwif_common.h', 'pvr_rogue_fwif_shared.h',
               'pvr_rogue_fwif_shared_check.h', 'pvr_rogue_fwif_check.h']
    inputs = []
    for name in headers + ['pvr_cccb.c', 'pvr_cccb.h']:
        data = (args.sources / name).read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        pinned = [r for r in manifest if r['path'] == 'imagination/' + name]
        if len(pinned) != 1 or pinned[0]['sha256'] != digest:
            raise ValueError('Pinned source mismatch: ' + name)
        inputs.append(dict(path=str(args.sources / name), sha256=digest))
        if name in headers:
            text = re.sub(r'^#include <linux/[^>]+>\n', '', data.decode(), flags=re.M)
            (args.output / name).write_text(text)
    source = (args.sources / 'pvr_cccb.c').read_text()
    header = (args.sources / 'pvr_cccb.h').read_text()
    prefixes = ['static __always_inline u32\nget_ccb_space(',
                'static void\ncccb_ctrl_init(', 'bool pvr_cccb_cmdseq_fits(',
                'void\npvr_cccb_write_command_with_header(',
                'static void fill_cmd_kick_data(']
    reference = '// SPDX-License-Identifier: GPL-2.0-only OR MIT\n'
    reference += '/* Copyright (c) 2023 Imagination Technologies Ltd. */\n' + SHIM
    reference += extract(header, 'static __always_inline u32\npvr_cccb_get_size_of_cmd_with_hdr(')
    reference += extract(header, 'static __always_inline bool\npvr_cccb_cmdseq_can_fit(')
    reference += '\n'.join(extract(source, p) for p in prefixes)
    reference += '#undef pvr_cccb\n'
    (args.output / 'reference.h').write_text(reference)
    sources = ['checks/pvr-cccb.c', 'drivers/gpu/pvr_cccb.c', 'drivers/gpu/pvr_ccb.c']
    binary = args.output / 'check'
    command = [args.compiler, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
               '-I' + str(args.output), '-I' + str(root / 'include')]
    command += [str(root / name) for name in sources] + ['-o', str(binary)]
    (args.output / 'compile-command.json').write_text(json.dumps(command, indent=2) + '\n')
    subprocess.run(command, check=True)
    run = subprocess.run([str(binary)], text=True, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, timeout=10)
    (args.output / 'check.log').write_text(run.stdout)
    for name in sources + ['checks/pvr-cccb.py', 'include/pvr_cccb.h', 'include/pvr_ccb.h']:
        data = (root / name).read_bytes()
        inputs.append(dict(path=str(root / name), sha256=hashlib.sha256(data).hexdigest()))
        target = args.output / 'as-built' / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    passed = run.returncode == 0 and 'PASS GPU client commands:' in run.stdout
    result = dict(passed=passed, returncode=run.returncode, inputs=inputs,
                  binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                  reference_sha256=hashlib.sha256(reference.encode()).hexdigest(),
                  compiler=subprocess.check_output([args.compiler, '--version'], text=True),
                  physical_access=False, firmware_execution=False)
    (args.output / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(run.stdout, end='')
    if not passed:
        raise RuntimeError('GPU client command check failed')


if __name__ == '__main__':
    main()
