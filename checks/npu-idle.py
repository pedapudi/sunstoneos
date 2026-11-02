#!/usr/bin/env python3
"""Exercise production NPU idle transitions with delayed, access-limited MMIO.

The fixture models register effects and a monotonic counter. It establishes
software ordering and retention; it supplies no physical timing or power proof.
"""
import argparse
import hashlib
from pathlib import Path
import subprocess

import host_link
from harness import CLANG, SANITIZE, environment, save

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['include/npu_idle.h', 'include/a733_npu_idle.h', 'drivers/npu/npu_idle.c',
                'checks/npu-idle.c', 'checks/register_latency.h',
                'checks/npu-idle.py', 'checks/harness.py', 'checks/host_link.py', 'checks/host/strict_mmio.c',
                'checks/host/strict_mmio.h']
CASES = ('nominal', 'threshold', 'cancellation', 'admission',
         'delayed-effects', 'partial-failure', 'readback', 'retention', 'budget', 'idle-mask', 'unmodeled')


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
    def fingerprints():
        return {name: hashlib.sha256(((ROOT if name.startswith('checks/') else
                args.source_root) / name).read_bytes()).hexdigest() for name in CHECK_INPUTS}
    inputs = fingerprints()
    # The register layer is the strict model of checks/host/strict_mmio.c: it refuses any offset
    # the fixture does not define with a cited source (checks/host_link.py).
    command = ['host_link.build', 'checks/npu-idle.c', 'drivers/npu/npu_idle.c']
    try:
        executable = host_link.build(args.output.resolve(), 'npu-idle', 'checks/npu-idle.c',
            [str(args.source_root / 'drivers/npu/npu_idle.c')],
            includes=[args.source_root / 'include', ROOT / 'checks'])
        (args.output / 'build.log').write_text('built through checks/host_link.py\n')
    except subprocess.CalledProcessError as error:
        raise AssertionError('build failed: ' + str(error))
    results = []
    for case in args.case or CASES:
        run = subprocess.run([str(executable), case], env=env, capture_output=True, text=True)
        (args.output / (case + '.log')).write_text(run.stdout + run.stderr)
        results.append({'case': case, 'passed': run.returncode == 0})
        assert fingerprints() == inputs, 'fixture or production inputs changed during execution'
        save(args.output / 'result.json', {'passed': all(row['passed'] for row in results),
             'physical_access': False, 'cases': results, 'build_command': command,
             'inputs': inputs,
             'limits': 'Production idle C; modeled MMIO, time, admission and interrupts. No board qualification.'})
        assert run.returncode == 0, run.stdout + run.stderr
    print('PASS NPU idle transitions, forbidden gated access and retained failures')


if __name__ == '__main__':
    main()
