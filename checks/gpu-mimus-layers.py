#!/usr/bin/env python3
"""Build and run the host model of exact mimus linear layers on the GPU.

Compiles shaders/pvr_mimus_linear.cl as C over its grid (each OpenCL vector
literal "(uint2)(" becomes a call), the session code in
drivers/gpu/pvr_compute_mimus.c with the linked program constant, the read-out
entry and mimus's own CPU path (gemm_combine_portable.cc from --mimus), then
runs checks/gpu-mimus-layers.cc twice: every case except the bench recording
with AddressSanitizer and UndefinedBehaviorSanitizer, and every case with
optimization. The bench run writes the call list that
checks/gpu-mimus-time-model.py reads (--calls). No hardware is accessed."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
if not __debug__:raise SystemExit('checks require assertions; unset PYTHONOPTIMIZE and do not pass -O')

ROOT = Path(__file__).resolve().parents[1]
DRIVERS = ['drivers/gpu/pvr_compute_mimus.c', 'drivers/gpu/pvr_compute_mimus_usc.c', 'drivers/gpu/pvr_compute_job.c',
           'drivers/gpu/pvr_compute_package.c', 'drivers/gpu/pvr_compute_programs.c', 'drivers/gpu/pvr_compute_context.c',
           'drivers/gpu/pvr_gpu_vm.c', 'drivers/gpu/pvr_cccb.c', 'drivers/gpu/pvr_ccb.c']
INPUTS = DRIVERS + ['shaders/pvr_mimus_linear.cl', 'checks/gpu-mimus-layers.cc', 'checks/gpu-mimus-layers-shim.h',
                    'checks/gpu-mimus-layers.py', 'include/pvr_compute_mimus.h', 'include/pvr_compute_mimus_readout.h']


def run(command, out, name, timeout=1800):
    p = subprocess.run(command, cwd=out, capture_output=True, text=True, timeout=timeout)
    (out / (name + '.log')).write_text(p.stdout + p.stderr)
    if p.returncode:
        raise RuntimeError(f'{name} failed:\n{p.stdout[-3000:]}{p.stderr[-3000:]}')
    return p.stdout


def kernel_as_c(out):
    source = (ROOT / 'shaders/pvr_mimus_linear.cl').read_text()
    include = '#include "pco/usclib/libcl.h"\n'
    assert source.count(include) == 1
    text = source.replace(include, '').replace('(uint2)(', 'mk2(')
    assert '(uint4)(' not in text and 'mem_fence(' not in text
    text += '\nvoid gpu_mimus_lane(void) { cs_mimus_linear_common(); }\n'
    path = out / 'kernel.c'
    path.write_text(text)
    return path


def build(out, name, mimus, flags):
    asr = mimus / 'firmware/components/asr'
    common = ['-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'checks'), '-I' + str(asr / 'include'),
              '-ffp-contract=off', '-g', '-Wall', '-Wextra', '-Werror'] + flags
    objects = []
    sources = [(out / 'kernel.c', ['-include', str(ROOT / 'checks/gpu-mimus-layers-shim.h'), '-fwrapv',
                                   '-Wno-unused-function'])]
    sources += [(ROOT / p, []) for p in DRIVERS]
    for source, extra in sources:
        obj = out / f'{name}-{source.stem}.o'
        run(['clang-21', '-std=c11', '-c', str(source), '-o', str(obj)] + common + extra, out, f'{name}-{source.stem}')
        objects.append(obj)
    for source in (asr / 'src/gemm_combine_portable.cc', ROOT / 'checks/gpu-mimus-layers.cc'):
        obj = out / f'{name}-{source.stem}.o'
        run(['clang++-21', '-std=c++20', '-c', str(source), '-o', str(obj)] + common, out, f'{name}-{source.stem}')
        objects.append(obj)
    binary = out / name
    run(['clang++-21', '-o', str(binary)] + [str(o) for o in objects] + flags, out, f'{name}-link')
    return binary


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--mimus', type=Path, required=True, help='mimus port checkout (MIMUS_DIR)')
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    out = a.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    mimus = a.mimus.resolve()
    kernel_as_c(out)
    sanitized = build(out, 'layers-sanitized', mimus,
                      ['-O1', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer'])
    optimized = build(out, 'layers', mimus, ['-O2'])
    quick = run([str(sanitized), '--quick'], out, 'run-sanitized')
    full = run([str(optimized), '--full', str(out / 'calls.txt')], out, 'run')
    bench = dict(zip(*[iter(re.search(r'^bench (.*)$', full, re.M).group(1).split())] * 2))
    revision = subprocess.run(['git', '-C', str(mimus), 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
    result = {
        'scope': 'Host model: kernel source as C over its grid, session code, read-out entry and mimus CPU path; '
                 'no USC execution',
        'mimus_revision': revision,
        'cases': [line for line in (quick + full).splitlines() if line.startswith('case ')],
        'bench': {k: float(v) if '.' in v else int(v) for k, v in bench.items()},
        'inputs': {p: hashlib.sha256((ROOT / p).read_bytes()).hexdigest() for p in INPUTS},
    }
    (out / 'layers-check.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result['bench']))
    print('PASS exact GPU sums and read-out equal mimus CPU path in every case; calls written to', out / 'calls.txt')


if __name__ == '__main__':
    main()
