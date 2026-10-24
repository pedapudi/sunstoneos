#!/usr/bin/env python3
"""Check the SunstoneOS exact-sum hook against a model of the mimus engine.

Builds checks/gpu-recognition-engine.cc with apps/os/gpu_hook.cc and
apps/os/dispatch.c for the host, with AddressSanitizer and
UndefinedBehaviorSanitizer, twice: without the engine's hook header (the
recognizer must not open the session) and with the header as
checks/gpu-recognition-hook.h declares it, beside a kernels.h that declares
the engine's SetExactSumOrdinals. Each build runs a generated table of eight
tensors through the engine model and compares every output bit for bit with
the engine's CPU path: SiLU and GLU block sinks with n_gpu < N and n_gpu = N,
int32 and int64 sums, a residual call, calls of several jobs, an ordinal
that names another tensor of the same shape, a tensor outside the placed
ordinals, and refused, lost and incomplete jobs. With --mimus-dir, it first
requires the declarations of checks/gpu-recognition-hook.h to equal the
engine's exact_sum_hook.h token for token, comments aside. No hardware is
accessed.

  gpu-recognition-engine.py --output DIR [--mimus-dir MIMUS_DIR]
"""
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
# ordinal: K, N, n_gpu, job_rows. job_rows is below each tensor's worker bound
# and small enough that the 222-row calls take several jobs.
TENSORS = [(176, 704, 320, 64), (176, 352, 160, 96), (704, 176, 80, 128), (176, 352, 352, 832),
           (176, 704, 704, 416), (1584, 176, 176, 224), (176, 176, 0, 0), (176, 176, 80, 3264)]


def headers(out):
    rows, admitted, engine, offset = [], [], [], 0
    placed = bytearray((len(TENSORS) + 7) // 8)
    for ordinal, (k, n, n_gpu, job_rows) in enumerate(TENSORS):
        stride = k
        rows.append('{%d,%d}' % (n_gpu, job_rows))
        if n_gpu:
            admitted.append('{%d,%d,%d,%d,%d,0}' % (offset, ordinal, stride, k, n))
            placed[ordinal // 8] |= 1 << (ordinal % 8)
        engine.append('{%d,%d,%d,%d}' % (k, n, stride, offset))
        offset += (n * stride + 4095) // 4096 * 4096
    assert offset <= 8 << 20
    (out / 'placement.h').write_text(
        '#define OS_PLACEMENT_TENSORS %du\n#define OS_PLACEMENT_GPU_TENSORS %du\n' % (len(rows), len(admitted)) +
        'const struct os_placement os_placement_table[OS_PLACEMENT_TENSORS]={%s};\n' % ','.join(rows) +
        'const struct os_placement_tensor os_placement_tensors[OS_PLACEMENT_GPU_TENSORS]={%s};\n' % ','.join(admitted) +
        'const uint8_t os_placement_gpu_ordinals[%d]={%s};\n' % (len(placed), ','.join(map(str, placed))))
    (out / 'engine-tensors.h').write_text('static const Tensor engine_tensors[]={%s};\n' % ','.join(engine))


def tokens(path):
    """C++ tokens of a header with its comments removed."""
    text = re.sub(r'//[^\n]*|/\*.*?\*/', ' ', path.read_text(), flags=re.S)
    return re.findall(r'\w+|[^\w\s]', text)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--mimus-dir', type=Path, help='mimus tree whose exact_sum_hook.h the copy must match')
    parser.add_argument('--thermal-only', action='store_true', help='only the new thermal ownership boundary cases')
    args = parser.parse_args()
    if args.mimus_dir:
        engine = args.mimus_dir / 'firmware/components/asr/include/mimus/asr/exact_sum_hook.h'
        if engine.is_file() and tokens(engine) != tokens(ROOT / 'checks/gpu-recognition-hook.h'):
            raise SystemExit('FAIL checks/gpu-recognition-hook.h differs from %s' % engine)
        print('hook header: %s' % ('equal to the engine\'s, token for token' if engine.is_file() else
                                   'the engine has none'))
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    headers(out)
    hook = out / 'engine-include/mimus/asr'
    hook.mkdir(parents=True)
    shutil.copy2(ROOT / 'checks/gpu-recognition-hook.h', hook / 'exact_sum_hook.h')
    # The one kernels.h declaration the SunstoneOS hook uses, as the engine
    # declares it (firmware/components/asr/include/mimus/asr/kernels.h).
    (hook / 'kernels.h').write_text('#include <cstdint>\n#include <span>\nnamespace mimus::asr {\n'
                                    'void SetExactSumOrdinals(std::span<const uint8_t> placed);\n}\n')
    common = ['-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
              '-ffp-contract=off', '-Iinclude', '-Iapps/os', '-I' + str(out), '-DOS_RECOGNITION_CORE=6',
              '-DSUNSTONE_A733_OS_GPU_LINEAR=1']
    results = []
    for name, extra in (('no-hook', []), ('hook', ['-I' + str(out / 'engine-include'), '-DENGINE_HOOK=1'])):
        if args.thermal_only and name == 'no-hook': continue
        build = out / name
        build.mkdir()
        subprocess.run(['clang', '-std=c11'] + common + extra + ['-c', 'apps/os/dispatch.c', '-o', str(build / 'dispatch.o')],
                       cwd=ROOT, check=True)
        subprocess.run(['clang++', '-std=c++20'] + common + extra +
                       ['apps/os/gpu_hook.cc', 'checks/gpu-recognition-engine.cc', str(build / 'dispatch.o'),
                        '-o', str(build / 'engine')], cwd=ROOT, check=True)
        run = subprocess.run([str(build / 'engine')] + (['--thermal-only'] if args.thermal_only else []), capture_output=True, text=True,
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
