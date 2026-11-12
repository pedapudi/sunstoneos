#!/usr/bin/env python3
"""Check compiled capture-profile admission, immutable binding and recognizer pin isolation."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess

from harness import CACHE, CLANG, SANITIZE, environment

ROOT = Path(__file__).resolve().parents[1]
BASELINE = '1694d667158b158a85efb5e214d8a676892dc1d4'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    kernel = CACHE / 'sources/FreeRTOS-Kernel-97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2'
    tcp = CACHE / 'sources/radio/stacks/FreeRTOS-Plus-TCP-b86ad086e9870099bb0ac18a75dcb7f6dd508cc7/source'
    # Synthetic application pins exercise the external recognizer comparison;
    # neither the generic provider nor this check loads application model data.
    (out / 'recognition-models.h').write_text(
        '#include <stdint.h>\nstatic const uint64_t bluetooth_model_bytes[2]={4096,8192};\n'
        'static const uint8_t bluetooth_model_sha256[2][32]={{1},{2}};\n')
    before = out / 'before.c'
    baseline = subprocess.check_output(['git', 'show', BASELINE + ':apps/bluetooth/bluetooth.c'], cwd=ROOT).decode()
    # Preserve the baseline's original wire size while sharing the current
    # C declaration. Otherwise the appended v4 tail would make the baseline
    # reject v3 at its size check before exercising the capture policy.
    geometry = 'p->version==3&&p->bytes==sizeof(*p)'
    assert baseline.count(geometry) == 1
    before.write_text(baseline.replace(geometry, 'p->version==3&&p->bytes==832'))
    common = ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', *SANITIZE,
              '-ffunction-sections', '-fdata-sections', '-DSUNSTONE_AIC8800_RSN=1',
              '-DINCLUDE_xSemaphoreGetMutexHolder=1', '-DSUNSTONE_BLUETOOTH_HEADSET=1',
              '-I' + str(ROOT / 'network/include'), '-I' + str(ROOT / 'include'),
              '-I' + str(ROOT / 'apps/bluetooth'), '-I' + str(kernel / 'include'),
              '-I' + str(kernel / 'portable/GCC/ARM_AARCH64_SRE'), '-I' + str(tcp / 'include'),
              '-I' + str(tcp / 'portable/Compiler/GCC'), '-I' + str(out)]
    variants = {
        'before-capture': (before, ['-DSUNSTONE_BLUETOOTH_CAPTURE_PROFILE=1', '-DTEST_CAPTURE_REFUSED=1']),
        'capture-cvsd': (ROOT / 'apps/bluetooth/bluetooth.c', ['-DSUNSTONE_A733_OS=1', '-DSUNSTONE_BLUETOOTH_CAPTURE_PROFILE=1']),
        'capture-msbc': (ROOT / 'apps/bluetooth/bluetooth.c', ['-DSUNSTONE_A733_OS=1', '-DSUNSTONE_BLUETOOTH_CAPTURE_PROFILE=1', '-DSUNSTONE_BLUETOOTH_MSBC=1']),
        'standalone-headset': (ROOT / 'apps/bluetooth/bluetooth.c', []),
        'recognizer': (ROOT / 'apps/bluetooth/bluetooth.c', ['-DSUNSTONE_BLUETOOTH_RECOGNITION=1']),
        'recognizer-capture': (ROOT / 'apps/bluetooth/bluetooth.c', ['-DSUNSTONE_BLUETOOTH_RECOGNITION=1', '-DSUNSTONE_BLUETOOTH_CAPTURE_PROFILE=1']),
    }
    commands, results, dependencies = [], {}, set()

    def run(label, command):
        command = list(map(str, command)); commands.append(command)
        (out / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
        result = subprocess.run(command, cwd=ROOT, env=environment(), capture_output=True, text=True)
        (out / (label + '.log')).write_text(result.stdout + result.stderr)
        assert result.returncode == 0, label + ': see ' + str(out / (label + '.log'))
        return result.stdout

    # Compile every selected configuration before executing any case.
    binaries = {}
    for name, (provider, definitions) in variants.items():
        obj = out / (name + '.o'); dep = out / (name + '.d')
        run(name + '-compile', [CLANG, *common, *definitions, '-MMD', '-MF', dep, '-c', provider, '-o', obj])
        binary = out / name
        run(name + '-link', [CLANG, *common, *definitions, '-MMD', '-MF', out / (name + '-fixture.d'),
                             ROOT / 'checks/bluetooth-capture-profile.c', ROOT / 'drivers/storage/storage_digest.c', obj,
                             '-Wl,--gc-sections', '-Wl,--wrap=storage_digest_bytes', '-o', binary])
        binaries[name] = binary
    for dep in out.glob('*.d'):
        dependencies.update((ROOT / n).resolve() for n in shlex.split(dep.read_text().replace('\\\n', ' ').split(':', 1)[1]))
    for name, binary in binaries.items():
        results[name] = json.loads(run(name + '-run', [binary]))
    assert results['before-capture']['version3_admitted'] is False
    assert results['capture-cvsd']['digest_calls'] == results['capture-msbc']['digest_calls'] == 1
    assert results['capture-cvsd']['digest_bytes'] == results['capture-msbc']['digest_bytes'] == 888
    assert all(results[name]['stop_refusals'] == 11 for name in ('capture-cvsd', 'capture-msbc', 'recognizer', 'recognizer-capture'))
    dependencies.update([Path(__file__).resolve(), ROOT / 'checks/harness.py', Path(CLANG).resolve()])
    record = dict(passed=True, physical_access=False, baseline_revision=BASELINE, variants=results,
                  scope='Actual provider validator and SHA-256; scheduling, transports and hardware are not executed.',
                  inputs={str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(dependencies)})
    (out / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
    print('PASS capture profiles, immutable binding, HFP bounds and external recognizer pins in six compiled configurations')


if __name__ == '__main__':
    main()
