#!/usr/bin/env python3
"""Exercise production NPU idle transitions with delayed, access-limited MMIO.

The fixture models register effects and a monotonic counter. It establishes
software ordering and retention; it supplies no physical timing or power proof.
"""
import argparse
import hashlib
from pathlib import Path
import subprocess

from harness import CLANG, SANITIZE, environment, save

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['include/npu_idle.h', 'drivers/npu/npu_idle.c',
                'checks/npu-idle.c', 'checks/register_latency.h']
CASES = ('nominal', 'threshold', 'cancellation', 'admission',
         'delayed-effects', 'partial-failure', 'readback', 'retention')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--source-root', type=Path, default=ROOT)
    parser.add_argument('--case', choices=CASES, action='append')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    temporary = args.output / 'tmp'
    temporary.mkdir()
    env = environment()
    env['TMPDIR'] = str(temporary.resolve())
    executable = args.output.resolve() / 'npu-idle'
    command = [str(CLANG), '-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
               *SANITIZE, '-I' + str(args.source_root / 'include'),
               '-I' + str(ROOT / 'checks'), str(ROOT / 'checks/npu-idle.c'),
               str(args.source_root / 'drivers/npu/npu_idle.c'), '-o', str(executable)]
    build = subprocess.run(command, env=env, capture_output=True, text=True)
    (args.output / 'build.log').write_text(build.stdout + build.stderr)
    assert build.returncode == 0, build.stdout + build.stderr
    results = []
    for case in args.case or CASES:
        run = subprocess.run([str(executable), case], env=env, capture_output=True, text=True)
        (args.output / (case + '.log')).write_text(run.stdout + run.stderr)
        results.append({'case': case, 'passed': run.returncode == 0})
        save(args.output / 'result.json', {'passed': all(row['passed'] for row in results),
             'physical_access': False, 'cases': results, 'build_command': command,
             'inputs': {name: hashlib.sha256(((ROOT if name.startswith('checks/') else
                        args.source_root) / name).read_bytes()).hexdigest() for name in CHECK_INPUTS},
             'limits': 'Production idle C; modeled MMIO, time, admission and interrupts. No board qualification.'})
        assert run.returncode == 0, run.stdout + run.stderr
    print('PASS NPU idle transitions, forbidden gated access and retained failures')


if __name__ == '__main__':
    main()
