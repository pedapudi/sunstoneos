#!/usr/bin/env python3
"""Check NPU mailbox wakeups, SDK dispatch and idle health without hardware."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = [
    'runtime/npu_app.c', 'include/sunstone_npu_service.h', 'include/sunstone_app.h',
    'apps/os/npu.c', 'apps/os/npu-client.c', 'apps/npu-sdk/host-check.c',
    'apps/npu-sdk/interleave-check.c', 'apps/npu-sdk/event-check.c',
    'checks/os-npu-sdk-events.c', 'checks/os-npu-health-events.c',
]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    cache = Path.home() / '.cache/sunstoneos'
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cc', type=Path, default=Path('/usr/bin/clang-21'))
    parser.add_argument('--case', action='append', choices=('host', 'interleave', 'event', 'sdk', 'health'),
                        help='Run only the named fixtures; repeat for independent cases')
    parser.add_argument('--freertos', type=Path, default=cache / 'sources/FreeRTOS-Kernel-97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2')
    parser.add_argument('--tcp', type=Path, default=cache / 'sources/radio/stacks/FreeRTOS-Plus-TCP-b86ad086e9870099bb0ac18a75dcb7f6dd508cc7')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    (output / 'tmp').mkdir()
    environment = dict(os.environ, TMPDIR=str(output / 'tmp'))
    commands = []

    def run(label, command):
        command = list(map(str, command))
        result = subprocess.run(command, cwd=ROOT, env=environment, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
        (output / (label + '.log')).write_text(result.stdout)
        commands.append({'label': label, 'command': command, 'returncode': result.returncode})
        (output / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
        result.check_returncode()

    common = [args.cc, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
              '-fsanitize=address,undefined', '-ffunction-sections', '-fdata-sections']
    os_flags = ['-DSUNSTONE_A733_OS_NPU=1', '-Wno-unused-function', '-Iapps/board',
                '-Iplatform/a733-smp-cached', '-Inetwork/include',
                '-I' + str(args.freertos / 'include'),
                '-I' + str(args.freertos / 'portable/GCC/ARM_CR82'),
                '-I' + str(args.tcp / 'source/include'),
                '-I' + str(args.tcp / 'source/portable/Compiler/GCC')]
    sources = {
        'host': ['runtime/npu_app.c', 'apps/npu-sdk/host-check.c'],
        'interleave': ['apps/npu-sdk/interleave-check.c'],
        'event': ['apps/npu-sdk/event-check.c'],
        'sdk': ['checks/os-npu-sdk-events.c'],
        'health': ['checks/os-npu-health-events.c'],
    }
    if args.case:
        sources = {name: units for name, units in sources.items() if name in args.case}
    dependencies = {Path(__file__).resolve()}
    for name, units in sources.items():
        objects = []
        for index, unit in enumerate(units):
            obj = output / f'{name}-{index}.o'
            dep = output / f'{name}-{index}.d'
            run(f'build-{name}-{index}', common + (os_flags if name in ('sdk', 'health') else []) + ['-Iinclude'] +
                (['-pthread'] if name == 'event' else []) + ['-MD', '-MF', dep, '-c', unit, '-o', obj])
            dependencies.update((ROOT / p).resolve() for p in shlex.split(dep.read_text().replace('\\\n', ' ').split(':', 1)[1]))
            objects.append(obj)
        run('link-' + name, [args.cc, '-fsanitize=address,undefined', '-Wl,--gc-sections',
                            *objects, *(['-pthread'] if name == 'event' else []), '-o', output / name])
    cases = [('host', []), ('host', ['failure']), ('host', ['refused']), ('host', ['jobfault']), ('host', ['graph']),
             *[('interleave', [str(i)]) for i in range(6)],
             ('event', []), ('sdk', []), ('health', [])]
    for index, (name, arguments) in enumerate(cases):
        if name not in sources:
            continue
        run(f'case-{index}-{name}', [output / name, *arguments])
    count = sum(name in sources for name, unused in cases)
    record = {'passed': True, 'physical_access': False, 'cases': count,
              'inputs': {str(path): sha(path) for path in sorted(dependencies)},
              'tools': {str(path): sha(path) for path in (args.cc.resolve(), Path(sys.executable).resolve())},
              'executables': {name: sha(output / name) for name in sources}}
    (output / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
    print('PASS', count, 'NPU mailbox and SDK event cases')


if __name__ == '__main__':
    main()
