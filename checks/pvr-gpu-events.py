#!/usr/bin/env python3
"""Check native event decoding and dispatch against the pinned firmware ABI."""
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
'''


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
    records = []
    for name in headers + ['pvr_free_list.c', 'pvr_ccb.c']:
        data = (args.sources / name).read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        pinned = [r for r in manifest if r['path'] == 'imagination/' + name]
        if len(pinned) != 1 or pinned[0]['sha256'] != digest:
            raise ValueError('Pinned source mismatch: ' + name)
        records.append(dict(path=str(args.sources / name), sha256=digest))
        if name in headers:
            text = re.sub(r'^#include <linux/[^>]+>\n', '', data.decode(), flags=re.M)
            (args.output / name).write_text(text)
    shim = args.output / 'shim.h'
    shim.write_text(SHIM)
    binary = args.output / 'check'
    command = [args.compiler, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
               '-include', str(shim), '-I' + str(args.output), '-I' + str(root / 'include'),
               str(root / 'checks/pvr-gpu-events.c'), str(root / 'drivers/gpu/pvr_gpu_events.c'),
               '-o', str(binary)]
    (args.output / 'compile-command.json').write_text(json.dumps(command, indent=2) + '\n')
    subprocess.run(command, check=True)
    run = subprocess.run([str(binary)], text=True, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, timeout=10)
    (args.output / 'check.log').write_text(run.stdout)
    if run.returncode or 'PASS GPU events:' not in run.stdout:
        print(run.stdout, end='')
        raise RuntimeError('GPU event check failed')
    integration_binary = args.output / 'control-check'
    integration_command = [args.compiler, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
        '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-I' + str(root / 'include'),
        str(root / 'checks/pvr-gpu-events-control.c'), str(root / 'drivers/gpu/pvr_gpu_events.c'),
        str(root / 'drivers/gpu/pvr_gpu_control.c'), str(root / 'drivers/gpu/pvr_ccb.c'),
        '-o', str(integration_binary)]
    (args.output / 'control-compile-command.json').write_text(json.dumps(integration_command, indent=2) + '\n')
    subprocess.run(integration_command, check=True)
    integration = subprocess.run([str(integration_binary)], text=True, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, timeout=10)
    (args.output / 'control-check.log').write_text(integration.stdout)
    for name in ['checks/pvr-gpu-events.py', 'checks/pvr-gpu-events.c', 'checks/pvr-gpu-events-control.c',
                 'checks/pvr-gpu-control.c', 'drivers/gpu/pvr_gpu_control.c', 'drivers/gpu/pvr_ccb.c',
                 'include/pvr_gpu_control.h', 'include/pvr_gpu_start.h', 'include/pvr_mips.h', 'include/pvr_firmware.h',
                 'drivers/gpu/pvr_gpu_events.c', 'include/pvr_gpu_events.h', 'include/pvr_ccb.h']:
        data = (root / name).read_bytes()
        records.append(dict(path=str(root / name), sha256=hashlib.sha256(data).hexdigest()))
        target = args.output / 'as-built' / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    result = dict(returncode=run.returncode, passed=run.returncode == 0 and 'PASS GPU events:' in run.stdout,
                  binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(), inputs=records,
                  compiler=subprocess.check_output([args.compiler, '--version'], text=True),
                  integration_returncode=integration.returncode,
                  integration_passed=integration.returncode == 0 and 'PASS GPU event/control integration:' in integration.stdout,
                  integration_binary_sha256=hashlib.sha256(integration_binary.read_bytes()).hexdigest(),
                  physical_access=False, firmware_execution=False)
    (args.output / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(run.stdout, end='')
    print(integration.stdout, end='')
    if not result['passed'] or not result['integration_passed']:
        raise RuntimeError('GPU event check failed')


if __name__ == '__main__':
    main()
