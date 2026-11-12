#!/usr/bin/env python3
"""Check the fixed USB NOLOAD arena exception in the returning-image parser."""
import argparse
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys

import harness

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
GATE_RUNS = [['--output', '{output}']]
CHECK_INPUTS = ['tools/check-a733-smp.py', 'apps/os/usb-memory-link.patch',
                'platform/a733-smp-cached/link.ld', 'platform/a733-storage-maintenance/link.patch',
                'include/a733_usb_memory.h', 'apps/os/session.c', 'apps/os/usb-service.c']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--usb-elf', type=Path, help='also check a complete native USB image')
    parser.add_argument('--control-elf', type=Path, action='append', default=[], help='also check an existing ordinary image')
    args = parser.parse_args()
    out = args.output
    out.mkdir(parents=True, exist_ok=True)
    spec = importlib.util.spec_from_file_location('image_parts_check', ROOT / 'tools/check-a733-smp.py')
    tool = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(tool)

    def run(command):
        result = subprocess.run([str(p) for p in command], cwd=ROOT, capture_output=True, text=True)
        assert result.returncode == 0, result.stdout + result.stderr

    # This small ELF exercises the real linker reservations, without pretending
    # that its stub services establish native USB behavior or physical authority.
    source = out / 'image.S'
    source.write_text('''.section .text.a733_smp_boot,"ax"
.global a733_smp_entry
a733_smp_entry: ret
.global usb_memory_extent_valid, os_usb_service_init
.type usb_memory_extent_valid,%function
usb_memory_extent_valid: ret
.size usb_memory_extent_valid,.-usb_memory_extent_valid
.type os_usb_service_init,%function
os_usb_service_init: ret
.size os_usb_service_init,.-os_usb_service_init
.section .rodata,"a"
.global a733_smp_build_id
.type a733_smp_build_id,%object
a733_smp_build_id: .fill 32,1,1
.size a733_smp_build_id,32
''')
    run([harness.CLANG, '--target=aarch64-none-elf', '-c', source, '-o', out / 'image.o'])
    run(['patch', '--fuzz=0', '-o', out / 'storage.ld', ROOT / 'platform/a733-smp-cached/link.ld',
         ROOT / 'platform/a733-storage-maintenance/link.patch'])
    run(['patch', '--fuzz=0', '-o', out / 'usb.ld', out / 'storage.ld', ROOT / 'apps/os/usb-memory-link.patch'])
    for name, script in (('control', 'storage.ld'), ('usb', 'usb.ld')):
        run([harness.LLD, '-T', out / script, out / 'image.o', '-o', out / (name + '.elf')])
    tool.image_parts(out / 'control.elf')
    for path in args.control_elf:
        tool.image_parts(path)
    originals = [out / 'usb.elf'] + ([args.usb_elf] if args.usb_elf else [])
    results = {}
    for number, original in enumerate(originals):
        raw = original.read_bytes()
        segments, _, identity = tool.image_parts(original)
        arena = [s for s in segments if s['address'] == 0x42200000]
        assert len(arena) == 1 and arena[0]['memory_bytes'] == 0x800000 and arena[0]['data'] == b''
        assert len(identity) == 32
        header = struct.unpack_from('<HHIQQQIHHHHHH', raw, 16)
        phoff, shoff, phsize, phnum, shsize, shnum = header[4], header[5], header[8], header[9], header[10], header[11]
        headers = [(phoff + i * phsize, struct.unpack_from('<IIQQQQQQ', raw, phoff + i * phsize)) for i in range(phnum)]
        arena_header = next(p for p, h in headers if h[0] == 1 and h[3] == 0x42200000)
        ordinary_header = next(p for p, h in headers if h[0] == 1 and h[3] == 0x42000000)
        sections = [(shoff + i * shsize, struct.unpack_from('<IIQQQQIIQQ', raw, shoff + i * shsize)) for i in range(shnum)]
        arena_section = next(p for p, s in sections if s[3] == 0x42200000 and s[5] == 0x800000)
        arena_index = (arena_section - shoff) // shsize
        other_section = next(p for p, s in sections if s[3] == 0x41f00000 and s[5] == 0x1000)
        symbols = {}
        for _, section in sections:
            if section[1] != 2:
                continue
            strings = sections[section[6]][1]
            names = raw[strings[4]:strings[4] + strings[5]]
            for pos in range(section[4], section[4] + section[5], 24):
                name = struct.unpack_from('<I', raw, pos)[0]
                symbols[names[name:].split(b'\0', 1)[0].decode()] = pos
        mutations = {
            'file bytes in arena': [('Q', arena_header + 32, 1)],
            'executable arena': [('I', arena_header + 4, 5)],
            'writable executable arena': [('I', arena_header + 4, 7)],
            'readonly arena': [('I', arena_header + 4, 4)],
            'larger arena': [('Q', arena_header + 40, 0xa00000)],
            'shifted arena': [('Q', arena_header + 16, 0x42201000), ('Q', arena_header + 24, 0x42201000)],
            'physical alias': [('Q', arena_header + 24, 0x42400000)],
            'file-backed section': [('I', arena_section + 4, 1)],
            'readonly section': [('Q', arena_section + 8, 2)],
            'shifted section': [('Q', arena_section + 16, 0x42201000)],
            'short section': [('Q', arena_section + 32, 0x600000)],
            'wrong linker end': [('Q', symbols['__uncached_pool_end'] + 8, 0x42c00000)],
            'wrong linker start': [('Q', symbols['__uncached_pool_start'] + 8, 0x42201000)],
            'unbound linker end': [('H', symbols['__uncached_pool_end'] + 6, 0)],
            'missing memory validator': [('I', symbols['usb_memory_extent_valid'], 0)],
            'missing USB service': [('I', symbols['os_usb_service_init'], 0)],
            'undefined memory validator': [('H', symbols['usb_memory_extent_valid'] + 6, 0)],
            'data memory validator': [('B', symbols['usb_memory_extent_valid'] + 4, 0x11)],
            'unloaded memory validator': [('H', symbols['usb_memory_extent_valid'] + 6, arena_index),
                                          ('Q', symbols['usb_memory_extent_valid'] + 8, 0x42200000)],
            'overlapping allocated section': [('Q', other_section + 16, 0x42200000)],
            'missing load reservation': [('I', arena_header, 0)],
            'changed load offset': [('Q', arena_header + 8, 0)],
            'ordinary allocation exceeds old limit': [('Q', ordinary_header + 40, 0x210000)],
            'duplicate arena section': [('I', other_section, struct.unpack_from('<I', raw, arena_section)[0])],
        }
        rejected = []
        path = out / ('mutation-%d.elf' % number)
        for label, edits in mutations.items():
            mutated = bytearray(raw)
            for fmt, offset, value in edits:
                struct.pack_into('<' + fmt, mutated, offset, value)
            path.write_bytes(mutated)
            try:
                tool.image_parts(path)
            except ValueError:
                rejected.append(label)
            else:
                raise AssertionError('accepted ' + label)
        results[str(original)] = rejected
    (out / 'result.json').write_text(json.dumps({'passed': True, 'physical_access': False,
        'usb_images': results, 'ordinary_images': [str(out / 'control.elf'), *map(str, args.control_elf)]}, indent=2) + '\n')
    print('PASS %d USB images, %d ordinary images, %d rejected mutations per USB image' %
          (len(originals), 1 + len(args.control_elf), len(mutations)))


if __name__ == '__main__':
    main()
