#!/usr/bin/env python3
"""Extend the accepted returning primary probe with CPU0 cache adoption and optional EL1 entry."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
from elftools.elf.elffile import ELFFile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('primary_host', ROOT/'tools/primary-publication-host.py')
primary = importlib.util.module_from_spec(spec)
spec.loader.exec_module(primary)
# Shared snapshot status masks and distributor record checks.
spec = importlib.util.spec_from_file_location('identity_builder', ROOT/'tools/prepare-secondary-identity-probe.py')
survey = importlib.util.module_from_spec(spec)
spec.loader.exec_module(survey)
require, digest = primary.require, primary.digest
NATIVE = primary.probe.NATIVE
REPORT = (0x42018000, 0x2100)
EL1_REPORT = (0x42018000, 0x2900)
IRQ_REPORT = (0x42018000, 0x2940)
IRQ_SOURCES = ('boot/a733/primary-interrupt-probe.S',)
CONTEXT_REPORT = (0x42018000, 0x2960)
CONTEXT_SOURCES = ('boot/a733/primary-context-probe.S',)
EXTENSION = 0x41020800
SOURCES = ('boot/a733/primary-cache-probe.S', 'boot/a733/native-el3-cache-probe.S',
           'tools/prepare-primary-cache-probe.py')

EL1_SOURCES = ('boot/a733/primary-local-cache-probe.c', 'boot/a733/secondary-distributor-probe.c',
               'boot/a733/secondary-local-probe.h', 'boot/a733/native-el1-cache-probe.S',
               'include/a733_cpu_continue.h', 'include/a733_cpu_entry.h', 'include/a733_cpu_start.h')
HOOK_PREFIXES = ('primary_cache_', 'primary_local_', 'primary_irq_', 'primary_context_', 'native_el', 'el1_')


def sources(el1, interrupts=False, context=False):
    return SOURCES + (EL1_SOURCES if el1 else ()) + (IRQ_SOURCES if interrupts else ()) + (CONTEXT_SOURCES if context else ())


def report_spec(manifest):
    return CONTEXT_REPORT if manifest.get('context', False) else IRQ_REPORT if manifest.get('interrupts', False) else EL1_REPORT if manifest.get('el1', False) else REPORT


def native_aliases(el1, context=False):
    result = {'probe_production_allow_ns_code': 'a733_cached_allow_ns_code',
              'probe_production_enable_el3': 'a733_cpu_coherence_enable_el3',
              'probe_production_el3_root': 'a733_cached_el3_l1'}
    if el1:
        result.update(probe_production_enter_el1='a733_cached_enter_el1',
                      probe_production_el1_root='a733_cached_el1_l1',
                      probe_production_el1_stacks='a733_cpu_el1_stacks')
        result.update({n: n for n in ('a733_cpu_index', 'a733_cpu_continue_init',
                                    'a733_cpu_continue_poll', 'memset')})
    if context:
        result.update(probe_context_vectors='a733_cached_el1_vectors',
                      probe_context_nesting='ullPortInterruptNestings',
                      probe_context_yield='ullPortYieldRequired')
    return result


def branch(address, target):
    displacement = target - address
    require(displacement % 4 == 0 and -(1 << 27) <= displacement < (1 << 27), 'Branch out of range.')
    return 0x14000000 | ((displacement // 4) & 0x3ffffff)


def patch_points(symbols, context=False):
    entry = symbols['a733_cached_enter_el1'] + 0x144
    vector = symbols['a733_cached_el3_vectors'] + 8 * 128 + 4
    return ((entry, 0xd518cc9f, 'el1_observe_checkpoint'),
            (vector, branch(vector, symbols['a733_cached_el3_fault']), 'el1_return_checkpoint')) + (
                ((symbols['vApplicationIRQHandler'], 0xa9be7bfd, 'primary_context_dispatch'),) if context else ())


def patch_payloads(frozen, payloads, points, hooks):
    result, records, patches = {}, [], []
    for item in payloads:
        data = bytearray(frozen[item['name']]); start = item['address']
        for address, original, hook in points:
            if start <= address and address + 4 <= start + len(data):
                require(struct.unpack_from('<I', data, address-start)[0] == original,
                        'Production EL1 checkpoint instruction differs.')
                replacement = branch(address, hooks[hook])
                struct.pack_into('<I', data, address-start, replacement)
                patches.append({'address': address, 'original': original, 'replacement': replacement})
        result[item['name']] = bytes(data)
        records.append({**item, 'sha256': hashlib.sha256(data).hexdigest()})
    require(len(patches) == len(points), 'EL1 patches did not match exactly one payload each.')
    return result, records, patches


def decode(raw, nonce, manifest=None):
    manifest = manifest or {}
    el1 = manifest.get("el1", False)
    interrupts = manifest.get('interrupts', False)
    require(len(raw) == report_spec(manifest)[1], 'Primary cache report length differs.')
    result = primary.probe.decode(raw[:256], nonce)
    spans = ((256, 0xa00), (0xc00, 0x1a00), (0x1c00, 0x2000), (0x20a0, 0x2800)) if el1 else ((256, 0x2000), (0x20a0, len(raw)))
    require(all(not any(raw[a:b]) for a, b in spans), 'Unused report storage changed.')
    if el1:
        local = struct.unpack_from('<64Q', raw, 0xa00)
        if local[0] == 4:
            require(local[1] and local[2] > result['counter_before'] and local[4] and
                    not any(raw[0x2000:]) and not any(local[34:]), 'Incomplete local refusal.')
            if local[4] in (0x1001, 0x1002):
                require(local[3] == 0 and not any(local[5:10]) and not any(local[20:]) and
                        not any(raw[0x1a00:0x1c00]), 'Pre-write refusal changed controller state.')
            else:
                require(survey.stable(local[10:20]) == survey.stable(local[20:30]) and local[30] == 0 and
                        local[7:10] == local[31:34], 'Refused controller did not restore local state.')
                decode_distributor(raw, local[2])
            result.update(primary_cached_el3_completed=False, primary_cached_el1_completed=False,
                          epoch=local[1], startup_deadline=local[2], local_refusal=True,
                          local_phase=local[3], local_error=local[4], local_writes=local[5])
            if interrupts: result['primary_interrupts_completed'] = False
            if manifest.get('context', False): result['primary_context_completed'] = False
            return result
    values = struct.unpack_from('<20Q', raw, 0x2000)
    require(values[0] == 3 and values[1] and values[2] > result['counter_before'] and
            values[3] == 1 and values[19] == 0, 'Primary cache adoption/restoration incomplete.')
    before, active, restored = values[4:9], values[9:14], values[14:19]
    require(before[:4] == tuple(result['el3_before']) and not before[0] & 0x02001005,
            'Cache interval inherited another EL3 state.')
    require(active == (before[0] | 0x1005, 0x80823019, 0x421f2000,
                       0x44ff00, before[4] & ~0x200), 'Unexpected primary cached controls.')
    require(restored == before, 'Primary cache controls were not restored.')
    result.update(primary_cached_el3_completed=True, epoch=values[1],
                  startup_deadline=values[2], cached_controls=list(active),
                  restored_controls=list(restored))
    if el1:
        local = struct.unpack_from('<64Q', raw, 0xa00)
        binding = (values[1], values[2])
        require(local[:3] == (3, *binding) and local[4] == 0 and local[5] > 0 and
                survey.stable(local[10:20]) == survey.stable(local[20:30]) and local[30] == 0 and
                not any(local[34:]), 'Primary local GIC setup/restoration incomplete.')
        # The baseline matches the probe's own admission: only GICR_CTLR's
        # status bits are ignored.
        require(not local[10] & ~survey.STATUS[0] and not any(local[11:15]) and not local[19] & 1 and
                ((local[19] >> 2) & 1) == ((local[19] >> 3) & 1), 'Primary baseline was not quiescent and awake.')
        require(local[7:10] == local[31:34], 'Primary CPU-interface groups or priority mask changed.')
        decode_distributor(raw, binding[1])
        entry = struct.unpack_from('<32Q', raw, 0x2800)
        require(entry[:3] == (3, *binding) and entry[4:6] == tuple(result['el1_after']),
                'EL1 entry inherited different preparation.')
        require(0x42000000 <= entry[3] < 0x421f0000 and not entry[3] & 15,
                'EL3 stack is outside reserved Device memory.')
        require(entry[9:23] == (0x733, 4, 0x30d01805, 0x200803019, 0x421f4000, 0x44ff00, 0,
                0x4124b000, 0, 24000000, 0x81000000, 0, 0x5e000733, manifest['hooks']['el1_smc_return']) and
                entry[23] & 0x3ff == 0x3c5 and entry[29:] == (0, 8, 12), 'Primary EL1/SMC observation differs.')
        require(entry[24:29] == entry[4:9], 'Primary EL1 controls were not restored.')
        result.update(primary_cached_el1_completed=True, local_gic_restored=True,
                      distributor_restored=True, el1_observed=list(entry[9:24]), local_writes=local[5])
        if interrupts:
            irq = struct.unpack_from('<8Q', raw, 0x2900)
            require(irq[0] == 3 and irq[1] in (0, 1) and irq[2] in (0, 1) and
                    irq[3] in (0, 1, 2) and not irq[4] & ~0x40000001 and
                    irq[5:] == (0, 0, 0), 'Incomplete primary IRQ cleanup.')
            require((irq[3] == 0) == (irq[1:3] == (1, 1)), 'IRQ outcome differs from delivery counts.')
            require(irq[3] != 2 or irq[1:3] == (0, 0) and irq[4] == 0, 'Pre-write IRQ refusal changed state.')
            result.update(primary_interrupts_completed=irq[3] == 0,
                          timer_interrupts=irq[1], self_interrupts=irq[2],
                          interrupt_outcome=irq[3], pending_before_cleanup=irq[4])
            if manifest.get('context', False):
                context = struct.unpack_from('<4Q', raw, 0x2940)
                expected = (3, 0, 0, 0) if irq[3] == 2 else (3, 192, 1, 0)
                require(context == expected, 'Incomplete production IRQ context cleanup.')
                result.update(primary_context_completed=irq[3] == 0, context_cache_lines=context[1],
                              context_register_checks=bool(context[2]))
    return result


def decode_distributor(raw, deadline):
    survey.check_distributor(raw, deadline)


def prepare(args):
    require(all(p.is_file() for p in (args.clang, args.lld, args.objcopy)), 'Compiler, linker and objcopy paths must exist before building.')
    context = getattr(args, 'context', False)
    interrupts = getattr(args, 'interrupts', False) or context
    el1 = getattr(args, 'el1', False) or interrupts
    report = CONTEXT_REPORT if context else IRQ_REPORT if interrupts else EL1_REPORT if el1 else REPORT
    parent_path = args.primary/'manifest.json'
    parent = json.loads(parent_path.read_text())
    frozen, reservations = primary.load(args.primary, args.image, parent)
    require(len(frozen['overlay.bin']) <= EXTENSION-primary.probe.OVERLAY[0],
            'Primary overlay overlaps cache extension.')
    with args.image.open('rb') as stream:
        elf = ELFFile(stream)
        symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()
                   if s['st_shndx'] != 'SHN_UNDEF'}
        aliases = {name: symbols[target] for name, target in native_aliases(el1, context).items()}
    require(all(report[0]+report[1] <= a or a+n <= report[0]
                for name, a, n in reservations if name != 'primary-report'),
            'Expanded report overlaps native storage.')
    address = parent['hooks']['primary_publication_after_prepare']
    offset = address-primary.probe.OVERLAY[0]
    original = struct.unpack_from('<I', frozen['overlay.bin'], offset)[0]
    require(original & 0xff00001f == 0x58000010, 'Expected displaced literal load into x16.')
    displacement = (original >> 5) & 0x7ffff
    if displacement & 0x40000:
        displacement -= 0x80000
    literal = offset+4*displacement
    require(struct.unpack_from('<Q', frozen['overlay.bin'], literal)[0] == REPORT[0],
            'Displaced instruction loads another report.')
    args.output.mkdir(parents=True, exist_ok=False)
    link = args.output/'cache.ld'
    link.write_text('ENTRY(primary_cache_probe)\nSECTIONS {\n . = 0x41020800;\n'
        ' .text : { *(.text .text.*) *(.rodata .rodata.*) }\n'
        ' .data : { *(.data .data.*) }\n .bss (NOLOAD) : { *(.bss .bss.* COMMON) }\n'
        ' ASSERT(SIZEOF(.data) == 0 && SIZEOF(.bss) == 0, "Explicit Device storage only")\n'
        ' ASSERT(. <= 0x41024000, "Cache overlay exceeds reservation")\n'
        ' /DISCARD/ : { *(.comment .note* .eh_frame*) }\n}\n')
    commands = []
    units = [(SOURCES[0], 'entry.o'), (SOURCES[1], 'cache.o')]
    if el1:
        units += [(EL1_SOURCES[0], 'local.o'), (EL1_SOURCES[1], 'distributor.o'),
                  ('boot/a733/native-el1-cache-probe.S', 'el1.o')]
    if interrupts: units.append((IRQ_SOURCES[0], 'interrupts.o'))
    if context: units.append((CONTEXT_SOURCES[0], 'context.o'))
    for source, name in units:
        commands.append([args.clang, '--target=aarch64-none-elf', '-march=armv8-a',
            '-DSUNSTONE_SECONDARY_CACHE=1',
            *([] if source in CONTEXT_SOURCES else ['-mgeneral-regs-only']), '-O2', '-ffreestanding',
            '-fno-builtin', '-fno-stack-protector', '-ffunction-sections', '-fdata-sections',
            '-Wall', '-Wextra', '-Werror', '-I', ROOT/'include', '-I', ROOT/'boot/a733',
            *(['-DSUNSTONE_PRIMARY_EL1=1', '-DSUNSTONE_PROBE_EL1_CACHE=1'] if el1 else []),
            *(['-DSUNSTONE_PRIMARY_INTERRUPTS=1'] if interrupts else []),
            *(['-DSUNSTONE_PRIMARY_CONTEXT=1'] if context else []),
            '-c', ROOT/source, '-o', args.output/name])
    commands += [[args.lld, '-T', link, '--gc-sections',
        *[f'--defsym={name}={value:#x}' for name, value in aliases.items()],
        f'--defsym=primary_cache_original_next={address+4:#x}',
        *[args.output/name for _, name in units], '-o', args.output/'cache.elf'],
        [args.objcopy, '-O', 'binary', args.output/'cache.elf', args.output/'cache.bin']]
    for command in commands:
        run = subprocess.run(list(map(str, command)), capture_output=True, text=True)
        with (args.output/'build.log').open('a') as log:
            log.write(run.stdout+run.stderr)
        require(run.returncode == 0, 'Cache overlay build failed; inspect build.log.')
    with (args.output/'cache.elf').open('rb') as stream:
        hooks = {s.name: s['st_value'] for s in ELFFile(stream).get_section_by_name('.symtab').iter_symbols()
                 if s.name.startswith(HOOK_PREFIXES)}
    overlay = bytearray(frozen['overlay.bin'])
    replacement = 0x14000000 | (((hooks['primary_cache_probe']-address)//4) & 0x3ffffff)
    struct.pack_into('<I', overlay, offset, replacement)
    overlay += bytes(EXTENSION-primary.probe.OVERLAY[0]-len(overlay))
    overlay += (args.output/'cache.bin').read_bytes()
    (args.output/'overlay.bin').write_bytes(overlay)
    payload_data, payloads, patches = patch_payloads(frozen, parent['payloads'],
                                                    patch_points(symbols, context) if el1 else (), hooks)
    for name, data in payload_data.items():
        (args.output/name).write_bytes(data)
    manifest = {'schema': 'a733-primary-cache-probe-v1', 'physical_access': False,
        'native_sha256': NATIVE, 'el1': el1, 'interrupts': interrupts, 'context': context, 'native_patches': patches, 'primary_manifest': {'path': str(parent_path.resolve()),
        'sha256': digest(parent_path)}, 'sources': {name: digest(ROOT/name) for name in sources(el1, interrupts, context)},
        'native_symbols': aliases, 'hooks': hooks, 'parent_hooks': parent['hooks'],
        'patch': {'address': address, 'original': original, 'replacement': replacement},
        'overlay': {**parent['overlay'], 'sha256': digest(args.output/'overlay.bin')},
        'payloads': payloads, 'report': {'address': report[0], 'bytes': report[1]},
        'commands': [list(map(str, c)) for c in commands]}
    (args.output/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    print('Prepared CPU0 cache adoption' + (' with local GIC and EL1 entry' if el1 else '') + '; no hardware access.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('image', 'primary', 'output', 'clang', 'lld', 'objcopy'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument("--el1", action="store_true", help="Include local GIC setup, cached EL1 entry and restoration")
    parser.add_argument("--interrupts", action="store_true", help="Include bounded CPU0 timer/self-SGI delivery and cleanup")
    parser.add_argument("--context", action="store_true", help="Exercise production IRQ register save/restore and owned cached-data cleanup")
    prepare(parser.parse_args())
