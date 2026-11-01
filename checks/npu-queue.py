#!/usr/bin/env python3
"""Exercise ordered NPU fences and retained DMA ownership in a host device model."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

CHECK_INPUTS = ['include/npu_queue.h', 'drivers/npu/npu_queue.c', 'checks/npu-queue.c',
                'include/a733_npu_sequence.h', 'drivers/npu/a733_npu_sequence.c']
ROOT = Path(__file__).resolve().parents[1]

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    tmp = a.output / 'tmp'
    tmp.mkdir()
    env = dict(os.environ, TMPDIR=str(tmp.resolve()))
    command = ['/usr/bin/clang-21', '-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=address,undefined', '-I' + str(ROOT / 'include'),
               str(ROOT / 'checks/npu-queue.c'), str(ROOT / 'drivers/npu/npu_queue.c'),
               str(ROOT / 'drivers/npu/a733_npu_sequence.c'),
               '-o', str(a.output / 'queue')]
    subprocess.run(command, env=env, check=True)
    rows = []
    for case, label in enumerate(('ordered-ten-thousand', 'device-error', 'event-before-idle', 'successor-trigger-failure', 'IRQ-during-registration')):
        run = subprocess.run([str(a.output / 'queue'), str(case)], env=env, capture_output=True, text=True)
        (a.output / (label + '.log')).write_text(run.stdout + run.stderr)
        rows.append({'case': label, 'passed': run.returncode == 0})
        assert run.returncode == 0, run.stdout + run.stderr
    value = {'passed': True, 'physical_access': False, 'cases': rows, 'build_command': command,
             'inputs': {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in CHECK_INPUTS}}
    (a.output / 'result.json').write_text(json.dumps(value, indent=2) + '\n')
    print('PASS ordered fences, ISR dispatch, delayed idle and retained ownership')

if __name__ == '__main__':
    main()
