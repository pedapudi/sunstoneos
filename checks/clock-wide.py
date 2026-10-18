#!/usr/bin/env python3
"""Check high clock scopes once and compile affected native consumers."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
CACHE = Path.home() / '.cache/sunstoneos'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--evidence', type=Path, required=True)
    parser.add_argument('--freertos-port', type=Path, required=True)
    parser.add_argument('--resume-native', type=int,
                        help='Resume at the failed native unit; retain completed objects and logs')
    args = parser.parse_args()
    out = args.evidence.resolve()
    resume = args.resume_native is not None
    out.mkdir(parents=True, exist_ok=resume)
    (out / 'inputs').mkdir(exist_ok=resume)
    (out / 'objects').mkdir(exist_ok=resume)
    spec = importlib.util.spec_from_file_location('snapshot', ROOT / 'tools/evidence-snapshot.py')
    snapshot = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(snapshot)
    inputs = {}
    commands = []
    if resume:
        inputs = json.loads((out / 'inputs.json').read_text())
        commands = json.loads((out / 'commands.json').read_text())
        assert commands[-1]['returncode'] and (
            commands[-1]['name'].split('-resume')[0] == f'native-{args.resume_native:02}' or
            commands[-1]['name'].startswith('compat-'))
        assert not any(c['name'] == 'host-result' for c in commands)
        (out / f'interrupted-inputs-{len(commands)}.json').write_text(json.dumps(inputs, indent=2) + '\n')
        key = str(Path(__file__).resolve())
        inputs[key + f'#driver-before-{len(commands)}'] = inputs.pop(key)

    def save():
        (out / 'inputs.json').write_text(json.dumps(inputs, indent=2) + '\n')
        (out / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')

    def capture(path):
        path = path.resolve()
        if str(path) in inputs:
            assert hashlib.sha256(path.read_bytes()).hexdigest() == inputs[str(path)]['sha256']
            return
        target = out / 'inputs' / str(len(inputs)) / path.name
        row = snapshot.copy_file(path, target)
        row['snapshot'] = str(target.relative_to(out))
        inputs[str(path)] = row

    def run(name, argv):
        argv = list(map(str, argv))
        result = subprocess.run(argv, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT)
        (out / (name + '.log')).write_text(result.stdout)
        commands.append({'name': name, 'argv': argv, 'returncode': result.returncode})
        save()
        if result.returncode:
            raise RuntimeError(f'{name} failed: {result.stdout}')

    include = [ROOT / p for p in ['platform/a733-smp-cached', 'include',
               'runtime/include', 'platform/a733', 'platform/a733-smp-observe']]
    target = ROOT / 'include/a733_clock_leases.h'

    def closure(path, seen):
        path = path.resolve()
        if path in seen:
            return
        seen.add(path)
        for name in re.findall(r'^\s*#\s*include\s*"([^"]+)"', path.read_text(), re.M):
            for candidate in [path.parent / name] + [p / name for p in include]:
                if candidate.is_file():
                    closure(candidate, seen)
                    break

    units = []
    for directory in ['drivers', 'platform', 'runtime', 'apps']:
        for path in (ROOT / directory).rglob('*.c'):
            dependencies = set()
            closure(path, dependencies)
            if target in dependencies:
                units.append(path)
                for dependency in sorted(dependencies):
                    capture(dependency)
    units.sort()
    if resume and commands[-1]['name'].startswith('compat-'):
        assert args.resume_native == len(units)
    compatibility = [ROOT / 'checks' / p for p in
                     ['clock-width.c', 'mmc-clock-leases.c', 'a733-pcie.c']]
    for path in [Path(__file__), ROOT / 'tools/evidence-snapshot.py',
                 ROOT / 'checks/clock-wide.c', *compatibility]:
        dependencies = set()
        closure(path, dependencies)
        for dependency in sorted(dependencies):
            capture(dependency)
    freertos = CACHE / 'sources/FreeRTOS-Kernel-97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2/include'
    for directory in [freertos, args.freertos_port]:
        for path in directory.glob('*.h'):
            capture(path)
    # The relative base configuration is included by the actual eight-core header.
    capture(ROOT / 'platform/a733-smp-cached/FreeRTOSConfig.h')
    capture(ROOT / 'include/FreeRTOSConfig.h')
    capture(ROOT / 'platform/a733/boot.h')
    compiler = Path('/usr/bin/clang').resolve()
    capture(compiler)
    capture(Path(sys.executable))
    save()

    def dependencies(dep):
        for name in shlex.split(dep.read_text().replace('\\\n', '').split(':', 1)[1]):
            path = Path(name)
            capture(path if path.is_absolute() else ROOT / path)
        save()

    native = [compiler, '--target=aarch64-none-elf', '-std=c11', '-march=armv8-a',
              '-mstrict-align', '-ffreestanding', '-fno-builtin', '-O2', '-g',
              '-Wall', '-Wextra', '-Werror', '-DSUNSTONE_A733_SMP_DIAGNOSTIC=1',
              '-DSUNSTONE_SECURE_MONITOR=1', '-DSUNSTONE_A733_CACHED_DEVICES=1',
              '-DINCLUDE_xSemaphoreGetMutexHolder=1', '-DINCLUDE_xTaskGetCurrentTaskHandle=1',
              '-DINCLUDE_xTaskGetSchedulerState=1']
    native += ['-I' + str(p) for p in include + [freertos, args.freertos_port]]
    for index, path in enumerate(units):
        stem = str(path.relative_to(ROOT)).replace('/', '_')
        dep = out / 'objects' / (stem + '.d')
        if resume and index < args.resume_native:
            assert any(c['name'].split('-resume')[0] == f'native-{index:02}' and not c['returncode'] for c in commands)
            assert (out / 'objects' / (stem + '.o')).is_file()
            continue
        extra = ['-DA733_SMP_CONTRACT_MAGIC=0x415337334f424331ULL'] if path.parent.name == 'dma-copy' else []
        # These existing adapters use the physical counter/MMIO helpers in the
        # single-core boot header. Their FreeRTOS configuration remains eight-core.
        if path.name in ['a733_gpu_platform_native.c', 'a733_gpu_resources_native.c']:
            extra += ['-iquote', str(ROOT / 'platform/a733')]
        if path.name == 'a733_radio_session.c':
            tcp = CACHE / 'sources/radio/stacks/FreeRTOS-Plus-TCP-b86ad086e9870099bb0ac18a75dcb7f6dd508cc7/source'
            extra += ['-DSUNSTONE_AIC8800_RSN=1']
            capture(ROOT / 'cmake/radio-security.cmake')
            capture(ROOT / 'cmake/radio-network.cmake')
            extra += ['-I' + str(p) for p in [ROOT / 'network/include', tcp / 'include',
                      tcp / 'portable/Compiler/GCC']]
            extra += ['-include', str(ROOT / 'network/include/FreeRTOSConfig.h'),
                      '-idirafter', str(CACHE / 'host/newlib-smp-1824bb825b46c932/aarch64-none-elf/include')]
            for directory in [ROOT / 'network/include', tcp / 'include', tcp / 'portable/Compiler/GCC']:
                for header in directory.glob('*.h'):
                    capture(header)
            save()
        if path.parent.name == 'a733-storage-update' and path.name == 'usb.c':
            tinyusb = CACHE / 'sources/tinyusb-d668b0abb7219b9ce324fa88d46604bb9e869973/src'
            extra += ['-I' + str(path.parent), '-I' + str(tinyusb)]
            capture(path.parent / 'tusb_config.h')
            for header in tinyusb.rglob('*.h'):
                capture(header)
            save()
        name = f'native-{index:02}'
        attempts = sum(c['name'].split('-resume')[0] == name for c in commands)
        if attempts:
            name += f'-resume{attempts}'
        run(name, [*native, *extra, '-MD', '-MF', dep, '-c', path,
                                 '-o', out / 'objects' / (stem + '.o')])
        dependencies(dep)
    host = [compiler, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Iinclude']
    for path in compatibility:
        if resume and any(c['name'] == 'compat-' + path.stem and not c['returncode'] for c in commands):
            assert (out / 'objects' / (path.stem + '.o')).is_file()
            continue
        dep = out / 'objects' / (path.stem + '.d')
        name = 'compat-' + path.stem
        extra = ['-std=gnu11'] if path.stem == 'a733-pcie' else []
        if any(c['name'] == name for c in commands):
            name += '-resume'
        run(name, [*host, *extra, '-MD', '-MF', dep, '-c', path,
                                  '-o', out / 'objects' / (path.stem + '.o')])
        dependencies(dep)
    executable = out / 'clock-wide'
    run('host-build', [*host, '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                      ROOT / 'checks/clock-wide.c', ROOT / 'drivers/clock/a733_clock_leases.c',
                      '-o', executable])
    # All inputs and the executable are preserved before the sole behavior execution.
    capture(executable)
    save()
    run('host-result', [executable])
    objects = {str(p.relative_to(out)): hashlib.sha256(p.read_bytes()).hexdigest()
               for p in (out / 'objects').glob('*.o')}
    result = {'result': 'passed', 'native_units': [str(p.relative_to(ROOT)) for p in units],
              'compatibility_compiled_only': [str(p.relative_to(ROOT)) for p in compatibility],
              'objects': objects, 'executable_sha256': hashlib.sha256(executable.read_bytes()).hexdigest(),
              'scope': 'One high-bit ownership sanitizer execution; affected native compilation only. '
                       'No previous workloads, native image execution, board operation or performance measurement.'}
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(f'PASS: high-bit ownership and {len(units)} native consumer compilations; {out}')


if __name__ == '__main__':
    main()
