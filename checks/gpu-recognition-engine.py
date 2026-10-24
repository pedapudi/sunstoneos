#!/usr/bin/env python3
"""Check the SunstoneOS exact-sum hook against a model of the mimus engine.

Builds checks/gpu-recognition-engine.cc with apps/os/gpu_hook.cc and
apps/os/dispatch.c for the host, with AddressSanitizer and
UndefinedBehaviorSanitizer, three times: without the engine's hook header
(the recognizer must not open the session), with the header as
checks/gpu-recognition-hook.h declares it, and with the header in the
validation build. Each build runs a generated table of seven tensors through
the engine model and compares every output bit for bit with the engine's
CPU path: SiLU and GLU block sinks with n_gpu < N and n_gpu = N, int32 and
int64 sums, a residual call, calls of several jobs, and refused, lost and
incomplete jobs. No hardware is accessed.

  gpu-recognition-engine.py --output DIR
"""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
# ordinal: K, N, n_gpu, job_rows. job_rows is below each tensor's worker bound
# and small enough that the 222-row calls take several jobs.
TENSORS = [(176, 704, 320, 64), (176, 352, 160, 96), (704, 176, 80, 128), (176, 352, 352, 832),
           (176, 704, 704, 416), (1584, 176, 176, 224), (176, 176, 0, 0)]


def headers(out):
    rows, admitted, engine, offset = [], [], [], 0
    for ordinal, (k, n, n_gpu, job_rows) in enumerate(TENSORS):
        stride = k
        rows.append('{%d,%d}' % (n_gpu, job_rows))
        if n_gpu:
            admitted.append('{%d,%d,%d,%d,0}' % (offset, ordinal, stride, k))
        engine.append('{%d,%d,%d,%d}' % (k, n, stride, offset))
        offset += (n * stride + 4095) // 4096 * 4096
    assert offset <= 8 << 20
    (out / 'placement.h').write_text(
        '#define OS_PLACEMENT_TENSORS %du\n#define OS_PLACEMENT_GPU_TENSORS %du\n' % (len(rows), len(admitted)) +
        'const struct os_placement os_placement_table[OS_PLACEMENT_TENSORS]={%s};\n' % ','.join(rows) +
        'const struct os_placement_tensor os_placement_tensors[OS_PLACEMENT_GPU_TENSORS]={%s};\n' % ','.join(admitted))
    (out / 'engine-tensors.h').write_text('static const Tensor engine_tensors[]={%s};\n' % ','.join(engine))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    headers(out)
    hook = out / 'engine-include/mimus/asr'
    hook.mkdir(parents=True)
    shutil.copy2(ROOT / 'checks/gpu-recognition-hook.h', hook / 'exact_sum_hook.h')
    common = ['-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
              '-ffp-contract=off', '-Iinclude', '-Iapps/os', '-I' + str(out), '-DOS_RECOGNITION_CORE=6',
              '-DSUNSTONE_A733_OS_GPU_LINEAR=1']
    results = []
    for name, extra in (('no-hook', []), ('hook', ['-I' + str(out / 'engine-include'), '-DENGINE_HOOK=1']),
                        ('validate', ['-I' + str(out / 'engine-include'), '-DENGINE_HOOK=1', '-DSUNSTONE_A733_OS_GPU_VALIDATE=1'])):
        build = out / name
        build.mkdir()
        subprocess.run(['clang', '-std=c11'] + common + extra + ['-c', 'apps/os/dispatch.c', '-o', str(build / 'dispatch.o')],
                       cwd=ROOT, check=True)
        subprocess.run(['clang++', '-std=c++20'] + common + extra +
                       ['apps/os/gpu_hook.cc', 'checks/gpu-recognition-engine.cc', str(build / 'dispatch.o'),
                        '-o', str(build / 'engine')], cwd=ROOT, check=True)
        run = subprocess.run([str(build / 'engine')], capture_output=True, text=True,
                             env={'ASAN_OPTIONS': 'detect_leaks=0', 'UBSAN_OPTIONS': 'print_stacktrace=1'})
        (build / 'output.log').write_text(run.stdout + run.stderr)
        print(run.stdout + run.stderr, end='')
        results.append({'build': name, 'returncode': run.returncode, 'output': run.stdout})
        if run.returncode or 'PASS' not in run.stdout:
            raise SystemExit('FAIL engine model build ' + name)
    (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
