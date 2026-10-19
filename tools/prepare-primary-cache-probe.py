#!/usr/bin/env python3
"""Extend the accepted returning primary probe with CPU0 EL3 cache adoption."""
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
require, digest = primary.require, primary.digest
NATIVE = primary.probe.NATIVE
REPORT = (0x42018000, 0x2100)
EXTENSION = 0x41020800
SOURCES = ('boot/a733/primary-cache-probe.S', 'boot/a733/native-el3-cache-probe.S',
           'tools/prepare-primary-cache-probe.py')


def decode(raw, nonce):
    require(len(raw) == REPORT[1], 'Primary cache report length differs.')
    result = primary.probe.decode(raw[:256], nonce)
    require(not any(raw[256:0x2000]) and not any(raw[0x20a0:]), 'Unused report storage changed.')
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
    return result


def prepare(args):
    parent_path = args.primary/'manifest.json'
    parent = json.loads(parent_path.read_text())
    frozen, reservations = primary.load(args.primary, args.image, parent)
    require(len(frozen['overlay.bin']) <= EXTENSION-primary.probe.OVERLAY[0],
            'Primary overlay overlaps cache extension.')
    with args.image.open('rb') as stream:
        elf = ELFFile(stream)
        symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()
                   if s['st_shndx'] != 'SHN_UNDEF'}
        aliases = {name: symbols[target] for name, target in {
            'probe_production_allow_ns_code': 'a733_cached_allow_ns_code',
            'probe_production_enable_el3': 'a733_cpu_coherence_enable_el3',
            'probe_production_el3_root': 'a733_cached_el3_l1'}.items()}
    require(all(REPORT[0]+REPORT[1] <= a or a+n <= REPORT[0]
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
    for source, name in ((SOURCES[0], 'entry.o'), (SOURCES[1], 'cache.o')):
        commands.append([args.clang, '--target=aarch64-none-elf', '-march=armv8-a',
            '-DSUNSTONE_SECONDARY_CACHE=1', '-c', ROOT/source, '-o', args.output/name])
    commands += [[args.lld, '-T', link, '--gc-sections',
        *[f'--defsym={name}={value:#x}' for name, value in aliases.items()],
        f'--defsym=primary_cache_original_next={address+4:#x}',
        args.output/'entry.o', args.output/'cache.o', '-o', args.output/'cache.elf'],
        [args.objcopy, '-O', 'binary', args.output/'cache.elf', args.output/'cache.bin']]
    for command in commands:
        run = subprocess.run(list(map(str, command)), capture_output=True, text=True)
        with (args.output/'build.log').open('a') as log:
            log.write(run.stdout+run.stderr)
        require(run.returncode == 0, 'Cache overlay build failed; inspect build.log.')
    with (args.output/'cache.elf').open('rb') as stream:
        hooks = {s.name: s['st_value'] for s in ELFFile(stream).get_section_by_name('.symtab').iter_symbols()
                 if s.name.startswith(('primary_cache_', 'native_el3_cache_probe'))}
    overlay = bytearray(frozen['overlay.bin'])
    replacement = 0x14000000 | (((hooks['primary_cache_probe']-address)//4) & 0x3ffffff)
    struct.pack_into('<I', overlay, offset, replacement)
    overlay += bytes(EXTENSION-primary.probe.OVERLAY[0]-len(overlay))
    overlay += (args.output/'cache.bin').read_bytes()
    (args.output/'overlay.bin').write_bytes(overlay)
    for name, data in frozen.items():
        if name != 'overlay.bin':
            (args.output/name).write_bytes(data)
    manifest = {'schema': 'a733-primary-cache-probe-v1', 'physical_access': False,
        'native_sha256': NATIVE, 'primary_manifest': {'path': str(parent_path.resolve()),
        'sha256': digest(parent_path)}, 'sources': {name: digest(ROOT/name) for name in SOURCES},
        'native_symbols': aliases, 'hooks': hooks, 'parent_hooks': parent['hooks'],
        'patch': {'address': address, 'original': original, 'replacement': replacement},
        'overlay': {**parent['overlay'], 'sha256': digest(args.output/'overlay.bin')},
        'payloads': parent['payloads'], 'report': {'address': REPORT[0], 'bytes': REPORT[1]},
        'commands': [list(map(str, c)) for c in commands]}
    (args.output/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    print('Prepared CPU0 EL3 cache adoption and exact restoration; no hardware access.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('image', 'primary', 'output', 'clang', 'lld', 'objcopy'):
        parser.add_argument('--'+name, type=Path, required=True)
    prepare(parser.parse_args())
