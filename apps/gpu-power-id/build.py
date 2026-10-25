#!/usr/bin/env python3
"""Build the GPU identity consumer, or with --power-only the power-only
variant that never accesses the GPU register window, with --wrapper-read the
wrapper-read variant, with --probe-read the probe-read variant or with
--fw-start the firmware-start variant, on the qualified returning PMIC
platform. Every variant runs the default GPU power
path (docs/designs/gpu-vendor-default.md), so the build compiles the current
monitor, PMIC provider and supply consumer with the GPU units."""
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


def consistent_types(path):
    """Every named structure, union and enumeration that two compile units of
    the image define in a header must agree: size and member offsets, or enumerator
    names and values. Reused and new objects meet in one image, so this
    compares each definition a reused object makes with every new object's.
    A renamed member at the same offset, such as boot_result's reserved
    word that became replaced_status, is not a layout change. A type that a
    .c file defines is private to that unit and two units may reuse its
    name (struct profile in the power owner and the PMIC provider). Returns the
    number of names defined in more than one unit."""
    from elftools.elf.elffile import ELFFile
    seen, shared = {}, set()
    tags = {'DW_TAG_structure_type': 'struct', 'DW_TAG_union_type': 'union', 'DW_TAG_enumeration_type': 'enum'}
    with path.open('rb') as stream:
        dwarf = ELFFile(stream).get_dwarf_info()
        for cu in dwarf.iter_CUs():
            unit = cu.get_top_DIE().attributes['DW_AT_name'].value.decode()
            program = dwarf.line_program_for_CU(cu)
            files = program['file_entry']
            base = 0 if program.header['version'] >= 5 else 1
            for die in cu.iter_DIEs():
                attrs = die.attributes
                if die.tag not in tags or 'DW_AT_name' not in attrs or 'DW_AT_declaration' in attrs:
                    continue
                if 'DW_AT_decl_file' not in attrs or \
                        not files[attrs['DW_AT_decl_file'].value - base].name.decode().endswith('.h'):
                    continue
                if die.tag == 'DW_TAG_enumeration_type':
                    value = tuple((c.attributes['DW_AT_name'].value.decode(), c.attributes['DW_AT_const_value'].value)
                                  for c in die.iter_children() if c.tag == 'DW_TAG_enumerator')
                else:
                    value = (attrs['DW_AT_byte_size'].value if 'DW_AT_byte_size' in attrs else None,
                             tuple(c.attributes['DW_AT_data_member_location'].value
                                   if 'DW_AT_data_member_location' in c.attributes else 0
                                   for c in die.iter_children() if c.tag == 'DW_TAG_member'))
                key = (tags[die.tag], attrs['DW_AT_name'].value.decode())
                if key in seen:
                    shared.add(key)
                    assert seen[key][0] == value, (key, seen[key][1], unit)
                else:
                    seen[key] = (value, unit)
    return len(shared)


# Units the default GPU power path changed beyond the GPU units: the
# monitor's EL3 security setup, and the PMIC provider and supply consumer's
# admission of DCDC4 at 960 mV. The monitor compiles with the monitor's flags.
SHARED_UNITS = [('platform/a733/monitor.c', 'monitor_init'), ('drivers/power/a733_pmic.c', 'a733_pmic_init'),
                ('drivers/power/a733_pmic_consumer.c', 'a733_pmic_consumer_init')]
# Units that hold a structure whose layout changed after the checkpoint and
# so must compile with the new units: struct bb_power (power.c, the only
# other object that names bb_power) embeds struct a733_counter_clock and
# struct a733_power_domains with its rows, and the lock-free counter clock
# (8431f4b0) and the power service's row wake and settled supply changed
# all three. counter_clock.c and a733_pmic_native.c define the counter
# clock's structure.
LAYOUT_UNITS = [('apps/board-bundles-power/power.c', 'bb_power'),
                ('platform/a733/counter_clock.c', 'a733_counter_clock_init'),
                ('drivers/power/a733_pmic_native.c', 'a733_pmic_native_init'),
                # The current power.c calls trace.c's sample and XADDR helpers.
                ('apps/board-bundles-power/trace.c', 'bb_twi_observe')]
# Structures the reused objects may not define: every checkpoint object
# whose DWARF defines one is replaced by a new compile.
CHANGED_LAYOUTS = ('a733_gpu_power', 'a733_gpu_platform', 'a733_power_domains', 'a733_power_domain_row',
                   'a733_counter_clock', 'bb_power')
PROBE_EXPECT = ('COMPLETE GPU probe read: identity 36.56.104.183 before and after the secure configuration, '
                'restored, OFF and provider parked')
FWS_EXPECT = ('COMPLETE GPU firmware start: started word 1 after the timer record and the secure configuration, '
              'GPU left running for the power removal')
# The firmware-start variant adds the firmware preparation and start units,
# which no checkpoint object defines, and the pinned open firmware.
FWS_UNITS = ['drivers/gpu/pvr_firmware.c', 'drivers/gpu/pvr_mips.c', 'drivers/gpu/pvr_fw_state.c',
             'drivers/gpu/pvr_ccb.c', 'drivers/gpu/pvr_gpu_start.c', 'platform/a733/gpu_memory.c']
FIRMWARE = 'sources/gpu/imagination-firmware/powervr/rogue_36.56.104.183_v1.fw'
FIRMWARE_SHA256 = '1db1c399c17401d1f79d46c880db81c724d748c784d4639433b076aba2f9c0d2'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--power-only', action='store_true',
                        help='TOP and CORE ON, CORE status 8, OFF and release; no GPU register access')
    parser.add_argument('--wrapper-read', action='store_true',
                        help='one ON, then four reads of GPU wrapper words; no other GPU register access')
    parser.add_argument('--probe-read', action='store_true',
                        help="one ON, the vendor's probe-time identity read, its secure configuration, the read again")
    parser.add_argument('--fw-start', action='store_true',
                        help='one ON, the timer record and secure configuration, then the firmware start')
    args = parser.parse_args()
    if args.power_only + args.wrapper_read + args.probe_read + args.fw_start > 1:
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
    # The monitor's own flags (-mgeneral-regs-only).
    monitor_flags = base['compile'][1][:base['compile'][1].index('-MD')]
    assert base['compile'][1][-1].endswith('/monitor.obj') and '-mgeneral-regs-only' in monitor_flags
    monitor_flags = [v.replace(oldroot, str(ROOT)) for v in monitor_flags]
    units = [
        ('apps/gpu-power-id/main.c', 'kernel_main'),
        ('drivers/gpu/a733_gpu_platform.c', 'a733_gpu_platform_init'),
        ('drivers/gpu/a733_gpu_platform_native.c', 'a733_gpu_platform_native_init'),
        ('drivers/gpu/a733_gpu_power.c', 'a733_gpu_power_init'),
        ('drivers/gpu/a733_gpu_resources.c', 'a733_gpu_resources_init'),
        ('drivers/gpu/a733_gpu_resources_native.c', 'a733_gpu_resources_native_init'),
        ('drivers/power/a733_power_domains.c', 'a733_power_domains_init'),
    ] + SHARED_UNITS + LAYOUT_UNITS
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
        # The probe-read variant compiles probe-read.c the same way.
        if source == 'apps/gpu-power-id/main.c' and args.probe_read:
            source = 'apps/gpu-power-id/probe-read.c'
        if source == 'apps/gpu-power-id/main.c' and args.fw_start:
            source = 'apps/gpu-power-id/fw-start.c'
        unit_flags = monitor_flags if source == 'platform/a733/monitor.c' else flags
        variant = ['-DGPU_POWER_ONLY=1'] if args.power_only and source == 'apps/gpu-power-id/main.c' else []
        command = unit_flags + variant + ['-MD', '-MF', str(obj) + '.d', '-c', str(ROOT / source), '-o', str(obj)]
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
    assert (len(reused), len(new)) == (23, 14), (len(reused), len(new))
    added = []
    if args.fw_start:
        firmware = harness.CACHE / FIRMWARE
        assert sha(firmware) == FIRMWARE_SHA256
        defined = set()
        for item in old_objects:
            with Path(item['object']).open('rb') as stream:
                defined |= {s.name for s in ELFFile(stream).get_section_by_name('.symtab').iter_symbols()
                            if s['st_shndx'] != 'SHN_UNDEF' and s['st_info']['bind'] == 'STB_GLOBAL'}
        assembly = out / 'firmware.S'
        assembly.write_text('.section .rodata.gpu_firmware,"a"\n.balign 16\n.global gpu_firmware\ngpu_firmware:\n'
                            f'.incbin "{firmware}"\n.global gpu_firmware_end\ngpu_firmware_end:\n')
        for i, source in enumerate(FWS_UNITS + [str(assembly)]):
            obj = out / f'added-{i}.obj'
            path = Path(source) if source.startswith('/') else ROOT / source
            command = flags + ['-MD', '-MF', str(obj) + '.d', '-c', str(path), '-o', str(obj)]
            subprocess.run(command, check=True, cwd=ROOT)
            commands.append(command)
            with obj.open('rb') as stream:
                symbols = {s.name for s in ELFFile(stream).get_section_by_name('.symtab').iter_symbols()
                           if s['st_shndx'] != 'SHN_UNDEF' and s['st_info']['bind'] == 'STB_GLOBAL'}
            # Each added unit is new to the image: no checkpoint object defines its symbols.
            assert symbols and not symbols & defined, (source, symbols & defined)
            added.append({'source': 'firmware.S' if source.startswith('/') else source,
                          'source_sha256': sha(path), 'object': str(obj), 'sha256': sha(obj)})
        output_at = link.index('-o')
        link[output_at:output_at] = [a['object'] for a in added] + ['-Wl,--defsym=__uncached_pool_size=2097152']
    # Audit every input object, not a guessed list of layout consumers. A
    # complete DWARF declaration of a changed structure identifies the eight
    # objects that use one; all must be replaced in this candidate.
    layout_users = []
    for item in old_objects:
        with Path(item['object']).open('rb') as stream:
            for cu in ELFFile(stream).get_dwarf_info().iter_CUs():
                types = {}
                for die in cu.iter_DIEs():
                    attrs = die.attributes
                    if die.tag == 'DW_TAG_structure_type' and 'DW_AT_name' in attrs and 'DW_AT_byte_size' in attrs:
                        name = attrs['DW_AT_name'].value.decode()
                        if name in CHANGED_LAYOUTS:
                            types[name] = attrs['DW_AT_byte_size'].value
                if types:
                    assert item['object'] in replacements, item
                    original = item.get('original', item['object'])
                    depfile = Path(original + '.d')
                    layout_users.append({'object': item['object'], 'unit': cu.get_top_DIE().attributes['DW_AT_name'].value.decode(),
                        'old_layouts': types, 'replacement': replacements[item['object']],
                        'depfile': str(depfile) if depfile.is_file() else None,
                        'depfile_sha256': sha(depfile) if depfile.is_file() else None})
    assert len(layout_users) == 8, [u['unit'] for u in layout_users]
    subprocess.run(link, check=True, cwd=ROOT)
    subprocess.run(['/usr/lib/llvm-21/bin/llvm-objcopy', '-O', 'binary', str(out / 'image.elf'), str(out / 'probe.bin')], check=True)
    # These shared layouts cross old/new object boundaries. The reused image
    # supplies their actual DWARF declarations; changed layouts refuse the build.
    names = ['bb_power', 'a733_gpu_resource_manager', 'a733_gpu_resource_native',
             'a733_pmic', 'a733_rail_leases', 'a733_platform_locks', 'a733_power_domains',
             'a733_power_domain_row', 'a733_pmic_lifetime', 'a733_pmic_consumer',
             'a733_pmic_consumer_config']
    # Every compile unit of the new image agrees on each layout; the ones
    # that reused objects still define equal the checkpoint's.
    current = shared_layouts(out / 'image.elf', names)
    consistent = consistent_types(out / 'image.elf')
    kept = [n for n in names if n not in CHANGED_LAYOUTS]
    old = shared_layouts(baseline / 'image.elf', names)
    assert {n: old[n] for n in kept} == {n: current[n] for n in kept}, 'Shared old/new object layouts differ'
    record = {'physical_access': False, 'execution_admitted': False,
              'base_build': {'path': str(baseline / 'build.json'), 'sha256': sha(baseline / 'build.json')},
              'monitor_evidence': base['monitor_evidence'],
              'monitor_evidence_sha256': base['monitor_evidence_sha256'],
              'compile': commands, 'link': link, 'new_objects': new, 'added_objects': added,
              'firmware': {'path': str(harness.CACHE / FIRMWARE), 'sha256': FIRMWARE_SHA256} if args.fw_start else None,
              'reused_objects': reused,
              'layout_users_rebuilt': layout_users,
              'shared_layouts_equal': [n for n in names if n not in CHANGED_LAYOUTS],
              'layouts_rebuilt': list(CHANGED_LAYOUTS), 'types_consistent_across_units': consistent, 'image_sha256': sha(out / 'image.elf'),
              'binary_sha256': sha(out / 'probe.bin'), 'binary_bytes': (out / 'probe.bin').stat().st_size,
              'variant': ('power-only' if args.power_only else 'wrapper-read' if args.wrapper_read
                          else 'probe-read' if args.probe_read else 'fw-start' if args.fw_start else 'identity'),
              'expect': ('COMPLETE GPU power only: CORE status 8, OFF and provider parked' if args.power_only
                         else 'COMPLETE GPU wrapper read: CORE status 8, four wrapper words, OFF and provider parked'
                         if args.wrapper_read else PROBE_EXPECT if args.probe_read else FWS_EXPECT if args.fw_start
                         else 'COMPLETE GPU power identity: 36.56.104.183 in two ON/OFF cycles, provider parked'),
              'local_authority_required': ('Exclusive GPU CCU, PLL_GPU0 (CCU 0x0e0) and PCK CORE6 policy/PWCR; TOP5 '
                                           'remains shared row; DCDC4 selector 0x15 to 0x2e (960 mV); at EL3, the '
                                           'vendor BL31 cold-boot security writes')}
    (out / 'build.json').write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps({k: record[k] for k in ['image_sha256', 'binary_sha256', 'binary_bytes']}))


if __name__ == '__main__':
    main()
