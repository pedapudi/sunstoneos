#!/usr/bin/env python3
"""Check that build patches apply exactly and supervised profiles require an explicit board mode.

The patch cases run cmake/patch.cmake in CMake script mode on small files and on
the pinned fuzzy patches. The board-mode cases configure the central project
only far enough to reach its profile guard. Clang 21 and ld.lld must be on PATH,
or pass toolchain cache options such as -DSUNSTONE_LLD=... after "--".
"""
import argparse
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('harness', ROOT / 'checks/harness.py')
harness = importlib.util.module_from_spec(spec)
spec.loader.exec_module(harness)


def run(command):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True, env=harness.environment())
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    return result.stdout + result.stderr


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True, help='new directory for scratch files and results.json')
    parser.add_argument('cmake_args', nargs='*', help='toolchain cache options for the board-mode cases')
    args = parser.parse_args()
    out = args.output
    out.mkdir(parents=True)
    script = out / 'apply.cmake'
    script.write_text('include("${PATCH_MODULE}")\napply_build_patch("${OUT}" "${SRC}" "${PATCH}")\n')
    results = {}

    def apply(name, source, patch):
        output = out / (name + '.out')
        run(['cmake', f'-DPATCH_MODULE={ROOT}/cmake/patch.cmake', f'-DOUT={output}', f'-DSRC={source}',
             f'-DPATCH={patch}', '-P', script])
        return output.read_bytes()

    # One hunk in the middle of a 40-line file; each variant changes one property.
    base = ''.join(f'line {n}\n' for n in range(1, 41))
    changed = base.replace('line 20\n', 'changed 20\n')
    (out / 'base.c').write_text(base)
    (out / 'changed.c').write_text(changed)
    patch = out / 'change.patch'
    diff = subprocess.run(['diff', '-u', out / 'base.c', out / 'changed.c'], capture_output=True, text=True)
    assert diff.returncode == 1 and diff.stdout, diff.stderr
    patch.write_text(diff.stdout)
    assert apply('exact', out / 'base.c', patch) == changed.encode()
    results['exact'] = 'applied'
    (out / 'offset.c').write_text('added\n' + base)
    assert apply('offset', out / 'offset.c', patch) == ('added\n' + changed).encode()
    results['offset'] = 'applied'
    # GNU patch's default fuzz accepts this mismatched context line.
    (out / 'fuzz.c').write_text(base.replace('line 18\n', 'line 18 edited\n'))
    run(['patch', '--silent', '--output', out / 'default-fuzz.out', out / 'fuzz.c', patch])
    harness.refused(lambda: apply('fuzz', out / 'fuzz.c', patch), 'Could not apply', RuntimeError)
    results['fuzz'] = 'refused'
    assert not (out / 'fuzz.out').exists() and not (out / 'fuzz.out.tmp').exists()

    # A pinned fuzzy patch applies only while its output keeps the pinned digest.
    pinned = ROOT / 'apps/pwm-single-core/main.patch'
    apply('pinned', ROOT / 'apps/board/main.c', pinned)
    results['pinned'] = 'applied'
    (out / 'main.c').write_text((ROOT / 'apps/board/main.c').read_text() + '/* changed base */\n')
    harness.refused(lambda: apply('pinned-changed', out / 'main.c', pinned), 'Could not apply', RuntimeError)
    results['pinned-changed-base'] = 'refused'

    # Board mode: a supervised-only profile never switches the mode on itself.
    def configure(name, *options):
        build = out / name
        return run(['cmake', '-S', ROOT, '-B', build, f'-DCMAKE_TOOLCHAIN_FILE={ROOT}/cmake/aarch64-clang.cmake',
                    *args.cmake_args, *options])

    def mode(name):
        cache = (out / name / 'CMakeCache.txt').read_text()
        return [line for line in cache.splitlines() if line.startswith('SUNSTONE_BOARD_SUPERVISED:')]

    harness.refused(lambda: configure('thermal', '-DSUNSTONE_A733_THERMAL=ON'),
                    'SUNSTONE_A733_THERMAL builds on the supervised board', RuntimeError)
    assert mode('thermal') == [], mode('thermal')
    harness.refused(lambda: configure('pwm-armed', '-DSUNSTONE_A733_PWM=ON', '-DSUNSTONE_BOARD_SUPERVISED=OFF'),
                    'SUNSTONE_A733_PWM builds on the supervised board', RuntimeError)
    assert mode('pwm-armed') == ['SUNSTONE_BOARD_SUPERVISED:UNINITIALIZED=OFF'], mode('pwm-armed')
    # With the mode requested, configuration passes the guard and stops at the
    # first missing dependency instead.
    harness.refused(lambda: configure('pwm-supervised', '-DSUNSTONE_A733_PWM=ON', '-DSUNSTONE_BOARD_SUPERVISED=ON'),
                    'Set FREERTOS_DIR', RuntimeError)
    results['board-mode'] = 'explicit'
    for name in ('thermal', 'pwm-armed', 'pwm-supervised'):
        shutil.rmtree(out / name)
    harness.save(out / 'results.json', results)
    print(json.dumps(results, indent=2))


if __name__ == '__main__':
    main()
