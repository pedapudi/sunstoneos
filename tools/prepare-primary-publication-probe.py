#!/usr/bin/env python3
"""Bind a returning primary-setup probe to the reviewed complete image, offline."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

from elftools.elf.elffile import ELFFile

ROOT = Path(__file__).resolve().parents[1]
NATIVE = 'a8228eaa8a664a22479b6d358654ced6563ede10366231c04a3a43a8418a4920'
OVERLAY = (0x41020000, 0x4000)
REPORT = (0x42018000, 256)
MAGIC = 0x4137335052494d31


def require(ok, message):
    if not ok:
        raise ValueError(message)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def decode(raw, nonce):
    require(len(raw) == REPORT[1] and len(nonce) == 16, 'Invalid report length.')
    words = struct.unpack('<32Q', raw)
    require(words[:4] == (MAGIC, 1, REPORT[1], 4) and raw[32:48] == nonce,
            'Primary preparation or restoration did not complete with this nonce.')
    require(words[6] == 12 and not words[7] & ~0xc1000000 and
            words[8:12] == (0x412fd050, 0x41, 0x073fc237, 24000000),
            'Report has an unreviewed CPU identity.')
    require(words[13] >= words[12], 'Counter regressed.')
    require(words[20] == words[14] | 1 and words[21] == words[15] | 0x800080,
            'Production walk blocking was not observed.')
    require(words[22:26] == words[16:20] and words[26:28] == words[14:16],
            'Translation control restoration differs.')
    require(not any(words[29:]), 'Unexpected reserved report fields.')
    return {'primary_preparation_completed': True, 'controls_restored': True,
            'counter_before': words[12], 'counter_after': words[13],
            'el1_before': list(words[14:16]), 'el1_after': list(words[20:22]),
            'el3_before': list(words[16:20]), 'el3_after': list(words[22:26]),
            'el1_restored': list(words[26:28]), 'stack': words[28]}


def prepare(args):
    require(digest(args.image) == NATIVE, 'Unreviewed native image.')
    with args.image.open('rb') as stream:
        elf = ELFFile(stream)
        entries = list(elf.get_section_by_name('.symtab').iter_symbols())
        def symbol(name):
            found = [s for s in entries if s.name == name and s['st_shndx'] != 'SHN_UNDEF']
            require(len(found) == 1, 'Missing or ambiguous symbol: '+name)
            return found[0]
        segments = [{'address': p['p_paddr'], 'memory_bytes': p['p_memsz'],
                     'data': p.data()} for p in elf.iter_segments() if p['p_type'] == 'PT_LOAD']
        sections = [{'name': s.name, 'address': s['sh_addr'], 'bytes': s['sh_size']}
                    for s in elf.iter_sections() if s['sh_flags'] & 2 and s['sh_size']]
        boot = symbol('a733_smp_boot')
        target = symbol('a733_cached_prepare')['st_value']
        code = next(s for s in segments if s['address'] <= boot['st_value'] and
                    boot['st_value']+boot['st_size'] <= s['address']+len(s['data']))
        start = boot['st_value']-code['address']
        calls = []
        for offset in range(start, start+boot['st_size'], 4):
            word = struct.unpack_from('<I', code['data'], offset)[0]
            displacement = word & 0x3ffffff
            if displacement & 0x2000000:
                displacement -= 0x4000000
            if word & 0xfc000000 == 0x94000000 and code['address']+offset+4*displacement == target:
                calls.append((offset, word))
        require(len(calls) == 1, 'Expected one direct boot call to production preparation.')
        layout = {name: symbol(name)['st_value'] for name in (
            '__bss_start', '__bss_end', '__monitor_bss_start', '__monitor_bss_end',
            '__tables_start', '__tables_end', '__a733_smp_boot_stack_top', 'a733_smp_entry')}
    for address, size in (OVERLAY, REPORT):
        require(all(address+size <= s['address'] or s['address']+s['bytes'] <= address
                    for s in sections), 'Probe storage overlaps native ELF storage.')
    args.output.mkdir(parents=True, exist_ok=False)
    # Reuse the existing overlay linker bounds; only its entry symbol differs.
    commands = [
        [args.clang, '--target=aarch64-none-elf', '-march=armv8-a', '-c',
         ROOT/'boot/a733/primary-publication-probe.S', '-o', args.output/'overlay.o'],
        [args.lld, '-T', ROOT/'boot/a733/native-startup-probe.ld', '-e', 'primary_publication_probe',
         '--defsym=probe_production_prepare='+hex(target), args.output/'overlay.o', '-o', args.output/'overlay.elf'],
        [args.objcopy, '-O', 'binary', args.output/'overlay.elf', args.output/'overlay.bin']]
    for command in commands:
        result = subprocess.run(list(map(str, command)), capture_output=True, text=True)
        with (args.output/'build.log').open('a') as stream:
            stream.write(result.stdout+result.stderr)
        require(result.returncode == 0, 'Overlay build failed; inspect build.log.')
    with (args.output/'overlay.elf').open('rb') as stream:
        hooks = {s.name: s['st_value'] for s in ELFFile(stream).get_section_by_name('.symtab').iter_symbols()
                 if s.name.startswith('primary_publication_')}
    require(0 < (args.output/'overlay.bin').stat().st_size <= OVERLAY[1], 'Overlay exceeds reservation.')
    offset, original = calls[0]
    address = code['address']+offset
    displacement = hooks['primary_publication_probe']-address
    require(displacement % 4 == 0 and -(1 << 27) <= displacement < (1 << 27), 'Branch out of range.')
    changed = 0x94000000 | ((displacement//4) & 0x3ffffff)
    patched = bytearray(code['data'])
    struct.pack_into('<I', patched, offset, changed)
    code['data'] = bytes(patched)
    payloads = []
    for index, segment in enumerate(segments):
        if segment['data']:
            path = args.output/f'segment-{index}.bin'
            path.write_bytes(segment['data'])
            payloads.append({'name': path.name, 'address': segment['address'],
                             'bytes': len(segment['data']), 'sha256': digest(path)})
    source_names = ('boot/a733/primary-publication-probe.S', 'boot/a733/native-startup-probe.ld',
                    'tools/prepare-primary-publication-probe.py')
    manifest = {'schema': 'a733-primary-publication-probe-v1', 'physical_access': False,
                'native_sha256': NATIVE, 'production_prepare': target, 'layout': layout,
                'patch': {'address': address, 'original': original, 'replacement': changed},
                'hooks': hooks, 'overlay': {'address': OVERLAY[0], 'reservation_bytes': OVERLAY[1],
                                          'sha256': digest(args.output/'overlay.bin')},
                'report': {'address': REPORT[0], 'bytes': REPORT[1], 'nonce_offset': 32},
                'native_reservations': sections,
                'payloads': payloads, 'sources': {name: digest(ROOT/name) for name in source_names},
                'commands': [[str(x) for x in c] for c in commands]}
    (args.output/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    print('Prepared primary publication overlay; no board access or launch authority.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('image', 'output', 'clang', 'lld', 'objcopy'):
        parser.add_argument('--'+name, type=Path, required=True)
    prepare(parser.parse_args())
