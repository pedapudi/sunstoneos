#!/usr/bin/env python3
"""Build the GPU identity consumer on the qualified returning PMIC platform."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'checks'))
import harness


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def shared_layouts(path, names):
    from elftools.elf.elffile import ELFFile
    layouts = {}
    with path.open('rb') as stream:
        dwarf = ELFFile(stream).get_dwarf_info()
        for cu in dwarf.iter_CUs():
            for die in cu.iter_DIEs():
                attrs = die.attributes
                if die.tag != 'DW_TAG_structure_type' or 'DW_AT_name' not in attrs or 'DW_AT_byte_size' not in attrs:
                    continue
                name = attrs['DW_AT_name'].value.decode()
                if name not in names:
                    continue
                fields = {}
                for member in die.iter_children():
                    if member.tag != 'DW_TAG_member':
                        continue
                    key = member.attributes['DW_AT_name'].value.decode()
                    # The accepted PMIC image contains this documented private
                    # member rename; its callback order, type and offset did not change.
                    if name == 'a733_pmic' and key == 'ops':
                        key = 'pmic_bus_ops'
                    fields[key] = member.attributes['DW_AT_data_member_location'].value
                value = (attrs['DW_AT_byte_size'].value, fields)
                assert name not in layouts or layouts[name] == value, name
                layouts[name] = value
    assert set(layouts) == set(names)
    return layouts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    baseline = harness.CACHE / 'evidence/npu-supply-snapshot-20261024'
    base = json.loads((baseline / 'build.json').read_text())
    assert sha(baseline / 'image.elf') == base['image_sha256'] == 'a95eff6baca811b1b28826b0be066ff76c94d1923f17a4dc18450238128e2b02'
    runpath = harness.CACHE / 'evidence/processor-completion-20261024/port-3-1/pmic-supply/session.json'
    run = json.loads(runpath.read_text())
    assert run['complete'] and run['result']['status'] == 0 and run['image_sha256'] == base['binary_sha256']
    oldroot = '/home/sunil/git/sunstoneos-worktrees/npu-prereq-1024'
    flags = base['compile'][-1][:base['compile'][-1].index('-MD')]
    flags = [v.replace(oldroot, str(ROOT)) for v in flags]
    sources = ['apps/gpu-power-id/main.c', 'drivers/gpu/a733_gpu_platform.c',
               'drivers/gpu/a733_gpu_platform_native.c', 'drivers/gpu/a733_gpu_power.c',
               'runtime/memmove.c', 'drivers/power/a733_power_domains.c']
    commands, new = [], []
    for i, source in enumerate(sources):
        obj = out / f'new-{i}.obj'
        command = flags + ['-MD', '-MF', str(obj) + '.d', '-c', str(ROOT / source), '-o', str(obj)]
        subprocess.run(command, check=True, cwd=ROOT)
        commands.append(command)
        new.append({'source': source, 'source_sha256': sha(ROOT / source), 'object': str(obj), 'sha256': sha(obj)})
    # Retain the accepted initializer, CPU monitor and provider. Replace the old
    # power owner with its reviewed deadline fix, because this probe issues PCK commands.
    old_owner = next(x['object'] for x in base['reused_objects'] if x['original'].endswith('/pmic/new-2.obj'))
    replace = {str(baseline / 'build/npu-supply.obj'): new[0]['object'], old_owner: new[-1]['object']}
    expected = {x['sha256'] for x in base['reused_objects']} | {x['sha256'] for x in base['new_objects']}
    link, reused = [], []
    for value in base['link']:
        if value in replace:
            value = replace[value]
        elif value.endswith('.obj'):
            assert sha(value) in expected, value
            dest = out / ('reused-' + sha(value) + '.obj')
            shutil.copyfile(value, dest)
            reused.append({'original': value, 'object': str(dest), 'sha256': sha(dest)})
            value = str(dest)
        elif value == str(baseline / 'image.elf'):
            value = str(out / 'image.elf')
        elif value.startswith('-Wl,-Map,'):
            value = '-Wl,-Map,' + str(out / 'image.map')
        link.append(value.replace(oldroot, str(ROOT)))
    link += [x['object'] for x in new[1:-1]]
    subprocess.run(link, check=True, cwd=ROOT)
    subprocess.run(['/usr/lib/llvm-21/bin/llvm-objcopy', '-O', 'binary', str(out / 'image.elf'), str(out / 'probe.bin')], check=True)
    # These shared layouts cross old/new object boundaries. The reused image
    # supplies their actual DWARF declarations; changed layouts refuse the build.
    names = ['bb_power', 'a733_gpu_resource_manager', 'a733_gpu_resource_native',
             'a733_pmic', 'a733_rail_leases', 'a733_platform_locks', 'a733_power_domains',
             'a733_power_domain_row', 'a733_pmic_lifetime', 'a733_pmic_consumer',
             'a733_pmic_consumer_config']
    old = shared_layouts(baseline / 'image.elf', names)
    current = shared_layouts(out / 'image.elf', names)
    assert old == current, 'Shared old/new object layouts differ'
    record = {'physical_access': False, 'execution_admitted': False,
              'base_build': {'path': str(baseline / 'build.json'), 'sha256': sha(baseline / 'build.json')},
              'base_physical_pass': {'path': str(runpath), 'sha256': sha(runpath)},
              'compile': commands, 'link': link, 'new_objects': new, 'reused_objects': reused,
              'shared_layouts_equal': names, 'image_sha256': sha(out / 'image.elf'),
              'binary_sha256': sha(out / 'probe.bin'), 'binary_bytes': (out / 'probe.bin').stat().st_size,
              'expect': 'COMPLETE GPU power identity: OFF and provider parked',
              'host_tools': base['host_tools']}
    (out / 'build.json').write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps({k: record[k] for k in ['image_sha256', 'binary_sha256', 'binary_bytes']}))


if __name__ == '__main__':
    main()
