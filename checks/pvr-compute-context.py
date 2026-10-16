#!/usr/bin/env python3
"""Compare offline compute contexts with pinned queue and context initializers."""
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
#include <stdlib.h>
#include <string.h>
#include <errno.h>
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
#define BIT(n) (1u<<(n))
#define BIT_ULL(n) (1ull<<(n))
#define GENMASK(h,l) ((~0u<<(l))&(~0u>>(31-(h))))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof(*(a)))
#define U32_MAX UINT32_MAX
#include "pvr_rogue_fwif.h"
#include "pvr_rogue_fwif_stream.h"
#include "pvr_stream.h"
struct pvr_device {u32 stream_musthave_quirks[PVR_STREAM_TYPE_MAX][8];};
static bool pvr_device_has_feature(struct pvr_device *p, u32 feature)
{(void)p;(void)feature;assert(0);return false;}
static bool pvr_device_has_uapi_quirk(struct pvr_device *p, u32 quirk)
{(void)p;(void)quirk;assert(0);return false;}
enum drm_pvr_job_type {DRM_PVR_JOB_TYPE_GEOMETRY, DRM_PVR_JOB_TYPE_FRAGMENT,
    DRM_PVR_JOB_TYPE_COMPUTE, DRM_PVR_JOB_TYPE_TRANSFER_FRAG};
struct pvr_fw_object {u32 address;};
struct pvr_context {struct pvr_device *pvr_dev;struct pvr_fw_object *vm_ctx;u32 priority,ctx_id;};
#define pvr_cccb reference_cccb
struct pvr_cccb {u32 ctrl_fw_addr,cccb_fw_addr;};
struct pvr_queue {struct pvr_context *ctx;struct pvr_cccb cccb;u32 ctx_offset;
    enum drm_pvr_job_type type;struct pvr_fw_object *reg_state_obj;u64 callstack_addr;};
static struct pvr_fw_object *pvr_vm_get_fw_mem_context(struct pvr_fw_object *o){return o;}
static void pvr_fw_object_get_fw_addr(struct pvr_fw_object *o,u32 *out){*out=o->address;}
static u32 reference_pid;
#define current (&reference_pid)
static u32 task_tgid_nr(u32 *p){return *p;}
struct drm_pvr_ioctl_create_context_args {u64 static_context_state;u32 static_context_state_len;};
#define u64_to_user_ptr(x) ((void *)(uintptr_t)(x))
#define IS_ERR(x) (!(x))
#define PTR_ERR(x) (-ENOMEM)
static void *memdup_user(const void *p,size_t n){void *out=malloc(n);if(out)memcpy(out,p,n);return out;}
#define kfree free
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
               'pvr_rogue_fwif_shared_check.h', 'pvr_rogue_fwif_check.h',
               'pvr_stream.h', 'pvr_rogue_fwif_stream.h']
    names = headers + ['pvr_queue.c', 'pvr_context.c', 'pvr_stream.c', 'pvr_stream_defs.c', 'pvr_fw_mips.c']
    inputs = []
    for name in names:
        data = (args.sources / name).read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        pinned = [r for r in manifest if r['path'] == 'imagination/' + name]
        if len(pinned) != 1 or pinned[0]['sha256'] != digest:
            raise ValueError('Pinned source mismatch: ' + name)
        inputs.append(dict(path=str(args.sources / name), sha256=digest))
        if name in headers:
            text = re.sub(r'^#include <linux/[^>]+>\n', '', data.decode(), flags=re.M)
            (args.output / name).write_text(text)
    queue = (args.sources / 'pvr_queue.c').read_text()
    context = (args.sources / 'pvr_context.c').read_text()
    stream = (args.sources / 'pvr_stream.c').read_text()
    definitions = (args.sources / 'pvr_stream_defs.c').read_text()
    reference = '// SPDX-License-Identifier: GPL-2.0-only OR MIT\n'
    reference += '/* Copyright (c) 2023 Imagination Technologies Ltd. */\n' + SHIM
    reference += re.search(r'^#define MAX_DEADLINE_MS .+$', queue, re.M)[0] + '\n'
    for prefix in ['static u32 get_dm_type(', 'static void init_fw_context(', 'static void reg_state_init(']:
        reference += extract(queue, prefix)
    reference += definitions[definitions.index('#define PVR_STREAM_DEF_SET'):definitions.index('#define PVR_STREAM_DEF_FEATURE')]
    for prefix in ['static const struct pvr_stream_def rogue_fwif_static_compute_context_state_stream[]',
                   'const struct pvr_stream_cmd_defs pvr_static_compute_context_state_stream =']:
        reference += extract(definitions, prefix).rstrip() + ';\n'
    for prefix in ['static __always_inline bool\nstream_def_is_supported(', 'static int\npvr_stream_get_data(',
                   'static int\npvr_stream_process_1(', 'static int\npvr_stream_process_ext_stream(',
                   'int\npvr_stream_process(']:
        reference += extract(stream, prefix)
    reference += extract(context, 'static int\nprocess_static_context_state(')
    reference += extract(context, 'static int init_compute_fw_objs(')
    reference += '#undef pvr_cccb\n'
    (args.output / 'reference.h').write_text(reference)
    sources = ['checks/pvr-compute-context.c', 'drivers/gpu/pvr_compute_context.c',
               'drivers/gpu/pvr_gpu_vm.c', 'drivers/gpu/pvr_cccb.c', 'drivers/gpu/pvr_ccb.c']
    binary = args.output / 'check'
    command = [args.compiler, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
               '-I' + str(args.output), '-I' + str(root / 'include')]
    command += [str(root / name) for name in sources] + ['-o', str(binary)]
    (args.output / 'compile-command.json').write_text(json.dumps(command, indent=2) + '\n')
    subprocess.run(command, check=True)
    run = subprocess.run([str(binary)], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=10)
    (args.output / 'check.log').write_text(run.stdout)
    for name in sources + ['checks/pvr-compute-context.py', 'include/pvr_compute_context.h',
                           'include/pvr_cccb.h', 'include/pvr_ccb.h', 'include/pvr_gpu_vm.h', 'include/pvr_firmware.h']:
        data = (root / name).read_bytes()
        inputs.append(dict(path=str(root / name), sha256=hashlib.sha256(data).hexdigest()))
        target = args.output / 'as-built' / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    passed = run.returncode == 0 and 'PASS offline compute context:' in run.stdout
    result = dict(passed=passed, returncode=run.returncode, inputs=inputs,
                  binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                  reference_sha256=hashlib.sha256(reference.encode()).hexdigest(),
                  compiler=subprocess.check_output([args.compiler, '--version'], text=True),
                  physical_access=False, firmware_execution=False)
    (args.output / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(run.stdout, end='')
    if not passed:
        raise RuntimeError('Compute context check failed')


if __name__ == '__main__':
    main()
