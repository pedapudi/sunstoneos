#!/usr/bin/env python3
"""Freeze a provider-readiness image from the prior GPU diagnostic objects."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'checks'))
import harness


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    out = parser.parse_args().output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    base_dir = harness.CACHE / 'evidence/gpu-power-id-20261024/final-image'
    base = json.loads((base_dir / 'build.json').read_text())
    assert sha(base_dir / 'image.elf') == base['image_sha256'] == 'fd3d5839f700b5e0f569b0ce722e186c33beba65512815b136d9c969a7341afe'
    accepted = harness.CACHE / 'evidence/cpu-monitor-cleanup-20261024/snapshot'
    monitor_record = json.loads((accepted / 'manifest.json').read_text())
    for source in ['platform/a733/monitor.c', 'platform/a733/boot.h']:
        assert sha(ROOT / source) == monitor_record['sources'][source]['sha256'], source
    flags_record = harness.CACHE / 'build/usb-session-takeover/central-2/CMakeFiles/a733-power-probe.dir/flags.make'
    values = dict(line.split(' = ', 1) for line in flags_record.read_text().splitlines() if line.startswith(('C_DEFINES = ', 'C_INCLUDES = ', 'C_FLAGS = ')))
    monitor_flags = ['/usr/bin/clang', '--target=aarch64-none-elf'] + shlex.split(' '.join(values[k] for k in ['C_DEFINES', 'C_INCLUDES', 'C_FLAGS'])) + ['-mgeneral-regs-only', '-Werror']
    monitor_flags = [v.replace('/home/sunil/git/sunstoneos-worktrees/audit-usb-session-takeover', str(ROOT)) for v in monitor_flags]
    oldroot = '/home/sunil/git/sunstoneos-worktrees/gpu-power-id-1024'
    client_flags = [v.replace(oldroot, str(ROOT)) for v in base['compile'][0][:base['compile'][0].index('-MD')]]
    commands, new = [], []
    for source, flags in [('apps/gpu-provider-ready/main.c', client_flags), ('platform/a733/monitor.c', monitor_flags)]:
        obj = out / (Path(source).stem + '.obj')
        command = flags + ['-MD', '-MF', str(obj) + '.d', '-c', str(ROOT / source), '-o', str(obj)]
        subprocess.run(command, check=True, cwd=ROOT)
        commands.append(command)
        new.append({'source': source, 'source_sha256': sha(ROOT / source), 'object': str(obj), 'sha256': sha(obj)})
    old_main = next(x['object'] for x in base['new_objects'] if x['source'] == 'apps/gpu-power-id/main.c')
    old_monitor = next(x['object'] for x in base['reused_objects'] if x['sha256'] == '8a590a0dd0fded16c7c6db8eb088834a760d74b455d3c14f5239bd45fc0847a3')
    replacement = {old_main: new[0]['object'], old_monitor: new[1]['object']}
    expected = {x['object']: x['sha256'] for x in base['new_objects'] + base['reused_objects']}
    reused, link = [], []
    for value in base['link']:
        if value in replacement:
            value = replacement[value]
        elif value.endswith('.obj'):
            assert sha(value) == expected[value], value
            target = out / ('reused-' + sha(value) + '.obj')
            shutil.copyfile(value, target)
            reused.append({'original': value, 'object': str(target), 'sha256': sha(target)})
            value = str(target)
        elif value == str(base_dir / 'image.elf'):
            value = str(out / 'image.elf')
        elif value.startswith('-Wl,-Map,'):
            value = '-Wl,-Map,' + str(out / 'image.map')
        link.append(value.replace(oldroot, str(ROOT)))
    assert len(reused) == 35
    subprocess.run(link, check=True, cwd=ROOT)
    subprocess.run(['/usr/lib/llvm-21/bin/llvm-objcopy', '-O', 'binary', str(out / 'image.elf'), str(out / 'checkpoint.bin')], check=True)
    result = {'physical_access': False, 'execution_admitted': False, 'base_build': str(base_dir / 'build.json'),
              'base_build_sha256': sha(base_dir / 'build.json'), 'base_image_sha256': base['image_sha256'],
              'monitor_evidence': str(accepted / 'manifest.json'), 'monitor_evidence_sha256': sha(accepted / 'manifest.json'),
              'monitor_flags_record': str(flags_record), 'monitor_flags_sha256': sha(flags_record),
              'compile': commands, 'link': link, 'new_objects': new, 'reused_objects': reused,
              'image_sha256': sha(out / 'image.elf'), 'binary_sha256': sha(out / 'checkpoint.bin'),
              'binary_bytes': (out / 'checkpoint.bin').stat().st_size,
              'expect': 'COMPLETE GPU provider readiness: supplies verified, parked, no GPU power request'}
    (out / 'build.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({k: result[k] for k in ['image_sha256', 'binary_sha256', 'binary_bytes']}))


if __name__ == '__main__':
    main()
