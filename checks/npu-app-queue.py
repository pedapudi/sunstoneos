#!/usr/bin/env python3
"""Check the EL0 NPU ring protocol and buffer ownership with modeled hardware."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['runtime/npu_app_queue.c', 'include/sunstone_npu_queue.h',
                'include/sunstone_npu_queue_service.h', 'checks/npu-app-queue.c',
                'drivers/npu/npu_queue.c', 'include/npu_queue.h', 'include/a733_npu_controller.h']

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--cc', type=Path, default=Path('/usr/bin/clang-21'))
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    temporary = a.output / 'tmp'
    temporary.mkdir()
    env = dict(os.environ, TMPDIR=str(temporary.resolve()))
    exe = a.output.resolve() / 'npu-app-queue'
    commands = []
    def run(label, command):
        command = list(map(str, command))
        r = subprocess.run(command, cwd=ROOT, env=env, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True, timeout=60)
        (a.output / (label + '.log')).write_text(r.stdout)
        commands.append(dict(label=label, command=command, returncode=r.returncode))
        (a.output / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
        r.check_returncode()
    run('build', [a.cc, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                  '-fsanitize=address,undefined', '-Iinclude', 'checks/npu-app-queue.c', '-o', exe])
    # The ring depth is the image's: a build for 2000 jobs in flight accepts 2000 and refuses 2001, the default build 64 and 65.
    run('build-deep', [a.cc, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-DSS_NPU_QUEUE_MAX_DEPTH=2000u',
                       '-fsanitize=address,undefined', '-Iinclude', 'checks/npu-app-queue.c', '-o', a.output.resolve() / 'npu-app-queue-deep'])
    run('depth-default', [exe, '8'])
    run('depth-2000', [a.output.resolve() / 'npu-app-queue-deep', '8'])
    for label, args in [('normal', []), ('start-failure', ['1']), ('completion-failure', ['2']), ('linked', ['3']),
                        ('undeclared-alias', ['4']), ('bad-edge-index', ['5']),
                        ('duplicate-edge', ['6']), ('partial-alias', ['7'])]:
        run(label, [exe, *args])
    digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    record = dict(passed=True, physical_access=False, cases=10,
                  hardware='modeled; no physical qualification',
                  inputs={p: digest(ROOT / p) for p in CHECK_INPUTS},
                  executable_sha256=digest(exe), compiler_sha256=digest(a.cc.resolve()))
    (a.output / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
    print('PASS EL0 NPU ring ownership, refill, dependencies and retained faults')
if __name__ == '__main__':
    main()
