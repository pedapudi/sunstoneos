#!/usr/bin/env python3
"""Compare fixed compute job bytes and check bounded completion ownership."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import runpy
import subprocess


SOURCE_HASHES = {'src/imagination/vulkan/pvr_arch_cmd_buffer.c': '9b81fe28911aaba1335fbabfc7179660f4ae413b876cffbf82039d34be32761b',
 'src/imagination/vulkan/pvr_arch_job_compute.c': '7dcb866e4a28ee956fd16331934a6e1e2d4c28ab5a515b1863f6b1ce12027c6c',
 'src/imagination/vulkan/pvr_csb.h': '0bc1566f3d2df9cd5e998ce3cfaac6bee88fd19c683bd435b2c626c363b3b89a',
 'src/imagination/vulkan/pvr_common.h': 'b0c3eb4a948246a96c18406fe3f762f3b66de79835bd4ca91ea922bea8a33b2b',
 'src/imagination/vulkan/pvr_arch_border.c': 'b99075178996bc38510872c18072dd0294a06be6dbacdbd580cfb72e0d47bdb7',
 'src/imagination/vulkan/pvr_border.h': '942b32e55e89c72e9c71c86ae1d141d6ca97ad9069138e120f50cece9e4a755a',
 'src/imagination/common/pvr_limits.h': '4b93f37f63c4307cb4e143068818a04886f5894306d90586d7efe4f1b909a182',
 'src/imagination/csbgen/rogue/cdm.xml': '935d92af420bfa32f90d1bedbdcb6673cee52e3ee7dcd9aecee19559ebae5fa7',
 'src/imagination/csbgen/rogue/kmd_stream.xml': '9d8a5d5776196754094551d226a34e84a2821e17e47e8eb8ca2499c007fbb420',
 'src/imagination/csbgen/rogue/texstate.xml': '063f9b2aede685f77fd18ee7867a3cab31c1295e1e3a3f3dffc2e0805ba3841c',
 'src/imagination/pco/pco_data.h': '062dec5efcbc802230a0261ca90e0a88b0aa7082d9acba954508d039e1259178',
 'src/imagination/pco/uscgen/pco_clc.c': '1281aa7e7b909d496a25f145ebc72bcdebc2755aec83bdde351b700091e9a1d4',
 'src/imagination/pco/usclib/libcl.h': '73dd0bb42437091180502c00aaffbc7476147cf472fd8e3569e7e82144677ad7'}

def extract(s, prefix):
    begin = s.index(prefix)
    end = s.index('{', begin) + 1
    depth = 1
    while depth:
        depth += (s[end] == '{') - (s[end] == '}')
        end += 1
    return s[begin:end] + '\n'


def run(command, cwd, output, name):
    result = subprocess.run(command, cwd=cwd, capture_output=True, text=True)
    (output / (name + '.log')).write_text(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(f'{name}: {result.stdout[-1000:]} {result.stderr[-4000:]}')
    return command


SHIM = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include "pvr_pds.h"
#include "common/device_info/bxm-4-64.h"
#include "pvr_types.h"
#define __pvr_address_type pvr_dev_addr_t
#define __pvr_get_address(a) (a).addr
#define __pvr_make_address(a) PVR_DEV_ADDR(a)
#include "csbgen/rogue/cdm.h"
#include "csbgen/rogue/cr.h"
#include "csbgen/rogue/kmd_stream.h"
#include "hwdef/rogue_hw_defs.h"
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint64_t aligned_u64 __attribute__((aligned(8)));
typedef int32_t s32;
#define __aligned(n) __attribute__((aligned(n)))
#define BIT_MASK(n) (1u<<(n))
#define BIT(n) (1u<<(n))
#define _BITULL(n) (UINT64_C(1)<<(n))
#define U32_MAX UINT32_MAX
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a)/sizeof(*(a)))
#endif
#define ALIGN(a,b) (((a)+(b)-1)&~((b)-1))
#include "pvr_rogue_fwif_client.h"
#include "pvr_rogue_fwif_stream.h"
#include "pvr_stream.h"
struct pvr_device {u32 stream_musthave_quirks[PVR_STREAM_TYPE_MAX][8];};
static bool pvr_device_has_uapi_quirk(struct pvr_device *d,u32 q)
{(void)d;(void)q;return false;}
#define PVR_WORKGROUP_DIMENSIONS 3U
struct pvr_csb {uint32_t *cursor;};
struct pvr_sub_cmd_compute {uint32_t num_shared_regs;};
#define pvr_csb_set_relocation_mark(c) ((void)(c))
#define pvr_csb_clear_relocation_mark(c) ((void)(c))
#define pvr_csb_emit(c,type,var) \
 for(struct ROGUE_##type var={ROGUE_##type##_header},*once=&var;once; \
 ROGUE_##type##_pack((c)->cursor,&var),(c)->cursor+=ROGUE_##type##_length,once=NULL)
'''


def main():
    a = argparse.ArgumentParser(description=__doc__)
    a.add_argument('--mesa-source', type=Path, required=True)
    a.add_argument('--mesa-build', type=Path, required=True)
    a.add_argument('--board-source', type=Path, required=True)
    a.add_argument('--output', type=Path, required=True)
    args = a.parse_args()
    root = Path(__file__).resolve().parents[1]
    src, build, board, out = [p.resolve() for p in
                              (args.mesa_source, args.mesa_build, args.board_source, args.output)]
    out.mkdir(parents=True, exist_ok=True)
    # Reuse the exact upstream PDS object already checked by the context producer.
    old = out.parent / 'gpu-compute-programs-20261016'
    assert hashlib.sha256((old / 'mesa-pds.o').read_bytes()).hexdigest() == \
        '233efe758751c3d3a17c1e9ca0df682399be41bfe9d6cc4f5ab096e57d8f9147', 'previously qualified Mesa PDS object'
    recorded = json.loads((old / 'manifest.json').read_text())
    flags = recorded['commands'][0]
    flags = flags[:flags.index('-c')]
    flags = [v for v in flags if not v.startswith('-I' + str(old))]
    flags += ['-I' + str(out)]
    pins = runpy.run_path(str(root / 'checks/pvr-compute-programs.py'))['SOURCE_HASHES']
    pins.update(SOURCE_HASHES)
    for name, sha in pins.items():
        assert hashlib.sha256((src / name).read_bytes()).hexdigest() == sha, name
    headers = ['pvr_rogue_fwif_client.h', 'pvr_rogue_fwif_client_check.h',
               'pvr_rogue_fwif_shared.h', 'pvr_rogue_fwif_shared_check.h',
               'pvr_rogue_fwif_stream.h', 'pvr_stream.h']
    board_manifest = json.loads((board.parent / 'manifest.json').read_text())
    used_board = headers + ['pvr_stream.c', 'pvr_stream_defs.c', 'pvr_job.c',
                           'pvr_queue.c', 'pvr_context.c', 'pvr_device_info.h', 'include/pvr_drm.h']
    source_hashes = {str(src / name): sha for name, sha in pins.items()}
    for name in used_board:
        data = (board / name).read_bytes()
        sha = hashlib.sha256(data).hexdigest()
        entry = [x for x in board_manifest if x['path'] == 'imagination/' + name]
        assert len(entry) == 1 and entry[0]['sha256'] == sha, name
        source_hashes[str(board / name)] = sha
        if name in headers:
            (out / name).write_text(re.sub(r'^#include <linux/[^>]+>\n', '', data.decode(), flags=re.M))
    ref = '/* SPDX-License-Identifier: GPL-2.0-only OR MIT */\n/* Copyright (c) 2023 Imagination Technologies Ltd. */\n' + SHIM
    info = (board / 'pvr_device_info.h').read_text()
    ref += extract(info, 'enum {\n\tPVR_FEATURE_CDM_USER_MODE_QUEUE').rstrip() + ';\n'
    ref += '''static bool pvr_device_has_feature(struct pvr_device *d,u32 f)
{(void)d;return f==PVR_FEATURE_GPU_MULTICORE_SUPPORT||f==PVR_FEATURE_TPU_DM_GLOBAL_REGISTERS;}\n'''
    defs = (board / 'pvr_stream_defs.c').read_text()
    ref += defs[defs.index('#define PVR_STREAM_DEF_SET'):defs.index('/*\n * When adding new parameters')]
    start = defs.index('static const struct pvr_stream_def rogue_fwif_cmd_compute_stream[]')
    end = defs.index('static const struct pvr_stream_def rogue_fwif_cmd_transfer_stream[]')
    ref += defs[start:end]
    stream = (board / 'pvr_stream.c').read_text()
    for name in ['static __always_inline bool\nstream_def_is_supported(', 'static int\npvr_stream_get_data(',
                 'static int\npvr_stream_process_1(', 'static int\npvr_stream_process_ext_stream(', 'int\npvr_stream_process(']:
        ref += extract(stream, name)
    drm = (board / 'include/pvr_drm.h').read_text()
    for name in ['PREVENT_ALL_OVERLAP', 'SINGLE_CORE']:
        ref += re.search(r'^#define DRM_PVR_SUBMIT_JOB_COMPUTE_CMD_' + name + r' .+$', drm, re.M)[0] + '\n'
    ref += extract((board / 'pvr_job.c').read_text(), 'static u32\nconvert_compute_flags(')
    cmd = (src / 'src/imagination/vulkan/pvr_arch_cmd_buffer.c').read_text()
    ref += extract(cmd, 'struct pvr_compute_kernel_info {').rstrip() + ';\n'
    ref += extract(cmd, 'static void\npvr_compute_generate_control_stream(')
    generated = (out / 'probe.c').read_text()
    ref += re.search(r'typedef struct PACKED _pco_precomp_data \{.*?\} pco_precomp_data;',
                     (src / 'src/imagination/pco/pco_data.h').read_text(), re.S)[0] + '\n'
    ref += re.search(r'static const uint32_t cs_sunstone_probe_common_0_common\[72\] = \{.*?\};', generated, re.S)[0] + '\n'
    (out / 'reference.h').write_text(ref)
    inputs = ['checks/pvr-compute-job.c', 'drivers/gpu/pvr_compute_job.c', 'drivers/gpu/pvr_compute_programs.c',
              'drivers/gpu/pvr_compute_context.c', 'drivers/gpu/pvr_gpu_vm.c', 'drivers/gpu/pvr_cccb.c', 'drivers/gpu/pvr_ccb.c']
    cmds = []
    # All emitted job bytes are compared once; lower-layer standalone suites are reused.
    cmds.append(run(flags + ['-DUPSTREAM', str(root / inputs[0])] + [str(root / p) for p in inputs[1:]] +
                    [str(old / 'mesa-pds.o'), str(build / 'src/util/libmesa_util.a'), '-Wl,--gc-sections',
                     '-pthread', '-lm', '-o', str(out / 'reference')], build, out, 'reference-build'))
    cmds.append(run(['clang-21', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-I' + str(root / 'include')] +
                   [str(root / p) for p in inputs] + ['-o', str(out / 'native')], root, out, 'native-build'))
    comparisons = []
    for variant in [0, 1]:
        for mode in ['reference', 'native']:
            cmds.append(run([str(out / mode), str(variant), str(out / f'{mode}-{variant}.bin')], root, out, f'{mode}-{variant}'))
        native, reference = [(out / f'{mode}-{variant}.bin').read_bytes() for mode in ['native', 'reference']]
        if native != reference:
            index = next(n for n, (x, y) in enumerate(zip(native, reference)) if x != y)
            raise RuntimeError(f'byte mismatch {index}: native {native[index]:02x}, reference {reference[index]:02x}')
        comparisons.append({'variant': variant, 'bytes': len(native), 'sha256': hashlib.sha256(native).hexdigest()})
    cmds.append(run([str(out / 'native'), '2', str(out / 'unused.bin')], root, out, 'native-init'))
    for name in inputs + ['include/pvr_compute_job.h', 'drivers/gpu/pvr_compute_probe.inc', 'shaders/pvr_compute_probe.cl', 'checks/pvr-compute-job.py']:
        source_hashes[str(root / name)] = hashlib.sha256((root / name).read_bytes()).hexdigest()
    manifest = {'scope': 'Source bytes and host ownership/completion model; no GPU execution', 'commands': cmds,
                'comparisons': comparisons, 'inputs': source_hashes,
                'artifacts': {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in
                              [out / 'native', out / 'reference', out / 'probe.spv', out / 'probe.c', old / 'mesa-pds.o']}}
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps(comparisons, indent=2))


if __name__ == '__main__':
    main()
