#!/usr/bin/env python3
"""Run the focused rail ownership batch and preserve all executed inputs."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
CACHE = Path.home() / '.cache/sunstoneos'


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--evidence', type=Path, required=True)
    ap.add_argument('--freertos-port', type=Path, required=True)
    ap.add_argument('--pmic-source', type=Path, required=True)
    a = ap.parse_args()
    out = a.evidence.resolve()
    out.mkdir(parents=True, exist_ok=False)
    (out / 'inputs').mkdir()
    (out / 'artifacts').mkdir()
    records = {}

    def snapshot(path):
        path = path.resolve()
        data = path.read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        dest = out / 'inputs' / digest
        if not dest.exists():
            dest.write_bytes(data)
        records[str(path)] = digest

    def run(name, argv):
        r = subprocess.run(list(map(str, argv)), cwd=ROOT, text=True,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (out / (name + '.json')).write_text(json.dumps({
            'argv': list(map(str, argv)), 'returncode': r.returncode,
            'output': r.stdout}, indent=2) + '\n')
        if r.returncode:
            raise RuntimeError(f'{name} failed: {r.stdout}')
        return r

    include = [ROOT / 'include', ROOT / 'runtime/include', ROOT / 'platform/a733',
               ROOT / 'platform/qemu-smp']
    target = ROOT / 'include/a733_gpu_resources.h'

    def depends(path, seen):
        path = path.resolve()
        if path == target:
            return True
        if path in seen:
            return False
        seen.add(path)
        for name in re.findall(r'^\s*#\s*include\s*"([^"]+)"', path.read_text(), re.M):
            for candidate in [path.parent / name] + [p / name for p in include]:
                if candidate.is_file():
                    if depends(candidate, seen):
                        return True
                    break
        return False

    units = sorted({p for d in ['drivers', 'platform', 'runtime']
                    for p in (ROOT / d).rglob('*.c') if depends(p, set())} |
                   {ROOT / 'drivers/clock/a733_clock_leases.c'})
    # Snapshot the complete project source/header/config closure before compiling.
    for path in [Path(__file__), ROOT / 'checks/rail-leases.c', a.pmic_source]:
        snapshot(path)
    for d in ['include', 'runtime/include', 'platform/qemu-smp']:
        for path in (ROOT / d).rglob('*.h'):
            snapshot(path)
    for path in units:
        snapshot(path)
    for path in a.freertos_port.rglob('*.h'):
        snapshot(path)
    source = a.pmic_source.read_text()
    table = source.split('pmu_axp8191_ctrl_tbl[] = {', 1)[1].split('\n};', 1)[0]
    names = re.findall(r'\{\s*"([a-z0-9]+)"', table)
    expected = [f'{stem}{i}' for stem, count in [('dcdc', 9), ('aldo', 6), ('bldo', 5),
                ('cldo', 5), ('dldo', 6), ('eldo', 6), ('dc1sw', 2)] for i in range(1, count + 1)]
    assert names == expected and len(names) == 39
    for name in ['dcdc1', 'dc1sw1', 'dc1sw2']:
        assert re.search(r'\{\s*"' + name + r'",[^\n]*AXP8191_DC1OUT_VOL', table)
    enum = (ROOT / 'include/a733_rail_leases.h').read_text().split('enum a733_rail {', 1)[1].split('};', 1)[0]
    assert re.findall(r'A733_RAIL_([A-Z0-9]+)', enum) == [n.upper() for n in expected] + ['COUNT']
    (out / 'source-comparison.json').write_text(json.dumps({'outputs': names, 'shared_selector': ['dcdc1', 'dc1sw1', 'dc1sw2']}, indent=2) + '\n')
    clang = Path('/usr/bin/clang').resolve()
    snapshot(clang)
    host = out / 'artifacts/rail-leases'
    sources = [ROOT / p for p in ['checks/rail-leases.c', 'drivers/power/a733_rail_leases.c',
               'drivers/clock/a733_clock_leases.c', 'drivers/gpu/a733_gpu_resources.c',
               'drivers/gpu/a733_gpu_clock_parent.c']]
    run('host-build', [clang, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                      '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                      '-Iinclude', *sources, '-o', host])
    # Record executable identity before its one declared execution.
    host_hash = hashlib.sha256(host.read_bytes()).hexdigest()
    (out / 'executed-image.json').write_text(json.dumps({'path': str(host), 'sha256': host_hash}, indent=2) + '\n')
    run('host-result', [host])
    freertos = CACHE / 'sources/FreeRTOS-Kernel-97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2/include'
    common = [clang, '--target=aarch64-none-elf', '-std=c11', '-march=armv8-a', '-mstrict-align',
              '-ffreestanding', '-fno-builtin', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
              '-DINCLUDE_xSemaphoreGetMutexHolder=1', '-DINCLUDE_xTaskGetCurrentTaskHandle=1',
              '-DINCLUDE_xTaskGetSchedulerState=1']
    for inc in include + [freertos, a.freertos_port]:
        common.append('-I' + str(inc))
    for index, path in enumerate(units):
        stem = str(path.relative_to(ROOT)).replace('/', '_')
        obj = out / 'artifacts' / (stem + '.o')
        dep = out / 'artifacts' / (stem + '.d')
        run(f'native-{index:02}', [*common, '-MMD', '-MF', dep, '-c', path, '-o', obj])
        for name in shlex.split(dep.read_text().replace('\\\n', '').split(':', 1)[1]):
            snapshot(Path(name) if Path(name).is_absolute() else ROOT / name)
    artifacts = {str(p.relative_to(out)): hashlib.sha256(p.read_bytes()).hexdigest()
                 for p in (out / 'artifacts').iterdir()}
    (out / 'manifest.json').write_text(json.dumps({'inputs': records, 'artifacts': artifacts,
        'native_units': [str(p.relative_to(ROOT)) for p in units],
        'scope': 'One new rail/bridge/borrow/fault sanitizer execution; affected native compilation only. No board or prior suite execution.'}, indent=2) + '\n')
    print(f'PASS: focused rail batch and {len(units)} native units; {out}')


if __name__ == '__main__':
    main()
