#!/usr/bin/env python3
"""Build the GPU identity consumer, or with --power-only the power-only
variant that never accesses the GPU register window, with --wrapper-read the
wrapper-read variant, with --wrapper-write the wrapper-write variant or with
--vendor-order the vendor-order variant, on the qualified returning PMIC
platform."""
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
    parser.add_argument('--power-only', action='store_true',
                        help='TOP and CORE ON, CORE status 8, OFF and release; no GPU register access')
    parser.add_argument('--wrapper-read', action='store_true',
                        help='one ON, then four reads of GPU wrapper words; no other GPU register access')
    parser.add_argument('--wrapper-write', action='store_true',
                        help='one ON, then three stages of GPU wrapper writes, restore, OFF and park')
    parser.add_argument('--vendor-order', action='store_true',
                        help='one ON, the vendor secure configuration, one core identity read, restore, OFF and park')
    args = parser.parse_args()
    if args.power_only + args.wrapper_read + args.wrapper_write + args.vendor_order > 1:
        parser.error('choose one variant')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    baseline = harness.CACHE / 'evidence/gpu-provider-ready-20261024/image-1'
    base = json.loads((baseline / 'build.json').read_text())
    assert sha(baseline / 'image.elf') == base['image_sha256'] == '3adfa4ad6ba386ce8d39f76646f540107c2a5d02ce42db6c1c35c90de68a75ed'
    # Reuse the reviewed monitor restoration-readback fix from this checkpoint.
    # The failed fd3d power/ID image did not contain that fix.
    oldroot = '/home/sunil/git/sunstoneos-worktrees/gpu-provider-ready-1024'
    flags = base['compile'][0][:base['compile'][0].index('-MD')]
    flags = [v.replace(oldroot, str(ROOT)) for v in flags]
    units = [
        ('apps/gpu-power-id/main.c', 'kernel_main'),
        ('drivers/gpu/a733_gpu_platform.c', 'a733_gpu_platform_init'),
        ('drivers/gpu/a733_gpu_platform_native.c', 'a733_gpu_platform_native_init'),
        ('drivers/gpu/a733_gpu_power.c', 'a733_gpu_power_init'),
        ('drivers/gpu/a733_gpu_resources.c', 'a733_gpu_resources_init'),
        ('drivers/gpu/a733_gpu_resources_native.c', 'a733_gpu_resources_native_init'),
        ('drivers/power/a733_power_domains.c', 'a733_power_domains_init'),
    ]
    from elftools.elf.elffile import ELFFile
    replacements = {}
    commands, new = [], []
    old_objects = base['new_objects'] + base['reused_objects']
    for i, (source, symbol) in enumerate(units):
        matches = []
        for item in old_objects:
            assert sha(item['object']) == item['sha256']
            with Path(item['object']).open('rb') as stream:
                symbols = ELFFile(stream).get_section_by_name('.symtab').get_symbol_by_name(symbol) or []
                if any(x['st_shndx'] != 'SHN_UNDEF' for x in symbols):
                    matches.append(item)
        assert len(matches) == 1, (source, symbol, matches)
        obj = out / f'new-{i}.obj'
        # The wrapper-read variant compiles wrapper.c, which includes main.c
        # as the power-only variant, in place of main.c.
        if source == 'apps/gpu-power-id/main.c' and args.wrapper_read:
            source = 'apps/gpu-power-id/wrapper.c'
        # The wrapper-write and vendor-order variants compile their client in
        # place of main.c, and the controller and platform sources with the
        # abandon calls added (abandon.h); the shared sources stay unchanged.
        if args.wrapper_write or args.vendor_order:
            client = 'wrapper-write.c' if args.wrapper_write else 'vendor-order.c'
            source = {'apps/gpu-power-id/main.c': 'apps/gpu-power-id/' + client,
                      'drivers/gpu/a733_gpu_power.c': 'apps/gpu-power-id/power-abandon.c',
                      'drivers/gpu/a733_gpu_platform.c': 'apps/gpu-power-id/platform-abandon.c'}.get(source, source)
        variant = ['-DGPU_POWER_ONLY=1'] if args.power_only and source == 'apps/gpu-power-id/main.c' else []
        command = flags + variant + ['-MD', '-MF', str(obj) + '.d', '-c', str(ROOT / source), '-o', str(obj)]
        subprocess.run(command, check=True, cwd=ROOT)
        commands.append(command)
        replacements[matches[0]['object']] = str(obj)
        new.append({'source': source, 'source_sha256': sha(ROOT / source), 'object': str(obj),
                    'sha256': sha(obj), 'replaced_object': matches[0]})
    expected = {x['object']: x['sha256'] for x in old_objects}
    link, reused = [], []
    for value in base['link']:
        if value in replacements:
            value = replacements[value]
        elif value.endswith('.obj'):
            assert sha(value) == expected[value], value
            dest = out / ('reused-' + sha(value) + '.obj')
            shutil.copyfile(value, dest)
            reused.append({'original': value, 'object': str(dest), 'sha256': sha(dest)})
            value = str(dest)
        elif value == str(baseline / 'image.elf'):
            value = str(out / 'image.elf')
        elif value.startswith('-Wl,-Map,'):
            value = '-Wl,-Map,' + str(out / 'image.map')
        link.append(value.replace(oldroot, str(ROOT)))
    assert len(reused) == 30 and len(new) == 7
    # Audit every input object, not a guessed list of layout consumers. A
    # complete power/platform DWARF declaration identifies the three objects
    # using the enlarged structures; all must be replaced in this candidate.
    layout_users = []
    for item in old_objects:
        with Path(item['object']).open('rb') as stream:
            for cu in ELFFile(stream).get_dwarf_info().iter_CUs():
                types = {}
                for die in cu.iter_DIEs():
                    attrs = die.attributes
                    if die.tag == 'DW_TAG_structure_type' and 'DW_AT_name' in attrs and 'DW_AT_byte_size' in attrs:
                        name = attrs['DW_AT_name'].value.decode()
                        if name in ('a733_gpu_power', 'a733_gpu_platform'):
                            types[name] = attrs['DW_AT_byte_size'].value
                if types:
                    assert item['object'] in replacements, item
                    original = item.get('original', item['object'])
                    depfile = Path(original + '.d')
                    assert depfile.is_file() and 'a733_gpu_power.h' in depfile.read_text(), depfile
                    layout_users.append({'object': item['object'], 'unit': cu.get_top_DIE().attributes['DW_AT_name'].value.decode(),
                        'old_layouts': types, 'replacement': replacements[item['object']],
                        'depfile': str(depfile), 'depfile_sha256': sha(depfile)})
    assert len(layout_users) == 3
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
              'monitor_evidence': base['monitor_evidence'],
              'monitor_evidence_sha256': base['monitor_evidence_sha256'],
              'compile': commands, 'link': link, 'new_objects': new, 'reused_objects': reused,
              'layout_users_rebuilt': layout_users,
              'shared_layouts_equal': names, 'image_sha256': sha(out / 'image.elf'),
              'binary_sha256': sha(out / 'probe.bin'), 'binary_bytes': (out / 'probe.bin').stat().st_size,
              'variant': ('power-only' if args.power_only else 'wrapper-read' if args.wrapper_read
                          else 'wrapper-write' if args.wrapper_write else 'vendor-order' if args.vendor_order
                          else 'identity'),
              'expect': ('COMPLETE GPU power only: CORE status 8, OFF and provider parked' if args.power_only
                         else 'COMPLETE GPU wrapper read: CORE status 8, four wrapper words, OFF and provider parked'
                         if args.wrapper_read else 'COMPLETE GPU wrapper write: CORE status 8, OFF and provider parked'
                         if args.wrapper_write
                         else 'COMPLETE GPU vendor order: identity 36.56.104.183, OFF and provider parked'
                         if args.vendor_order else 'COMPLETE GPU power identity: OFF and provider parked'),
              'local_authority_required': 'Exclusive GPU CCU and PCK CORE6 policy/PWCR; TOP5 remains shared row' }
    (out / 'build.json').write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps({k: record[k] for k in ['image_sha256', 'binary_sha256', 'binary_bytes']}))


if __name__ == '__main__':
    main()
