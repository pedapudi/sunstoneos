#!/usr/bin/env python3
"""Build the early FEL gate's metadata and an unqualified ordinary boot0 file.

No device is opened. The pinned image changes only its initial ARM branch,
length/checksum and one appended sector. The stock storage loader is unchanged.
"""
import argparse
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import stat
import struct
import uuid
from elftools.elf.elffile import ELFFile

ROOT = Path(__file__).resolve().parents[1]
PIN = '829ea8e3b65d71a02f2d0e094bc073de3a10869f948f62db49dbf023d07fabe3'
BASE, ORIGINAL_BYTES = 0x47000, 0x3c000


def require(ok, message):
    if not ok:
        raise ValueError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def checksum(data):
    require(len(data) % 4 == 0, 'boot0 checksum requires whole words.')
    return (sum(struct.unpack('<%dI' % (len(data) // 4), data)) -
            struct.unpack_from('<I', data, 12)[0] + 0x5f0a6c39) & 0xffffffff


def gate_info(elf_raw, raw):
    with io.BytesIO(elf_raw) as stream:
        elf = ELFFile(stream)
        require(elf['e_machine'] == 'EM_ARM' and elf['e_type'] == 'ET_EXEC', 'AArch32 gate ELF required.')
        table = elf.get_section_by_name('.symtab')
        names = ('a733_card_fel_gate', 'a733_card_fel_gate_state', 'a733_card_fel_gate_test',
                 'a733_card_fel_gate_end')
        symbols = {}
        for symbol in table.iter_symbols():
            if symbol.name in names and symbol['st_shndx'] != 'SHN_UNDEF':
                require(symbol.name not in symbols, 'Ambiguous gate symbol.')
                symbols[symbol.name] = int(symbol['st_value'])
        require(set(symbols) == set(names), 'Gate symbols missing.')
        start, end = symbols[names[0]], symbols[names[3]]
        require(elf['e_entry'] == start == 0x41001000 and end - start == len(raw) and
                0 < len(raw) <= 512, 'Gate entry or appended-sector budget differs.')
        sections = [s for s in elf.iter_sections() if s['sh_flags'] & 2 and s['sh_size']]
        require(len(sections) == 1 and sections[0]['sh_type'] == 'SHT_PROGBITS' and
                sections[0]['sh_addr'] == start and sections[0].data() == raw, 'Gate binary differs from ELF.')
        state, test = symbols[names[1]] - start, symbols[names[2]] - start
        require(state != test and all(0 <= offset <= len(raw) - 4 and offset % 4 == 0 and
                                    raw[offset:offset+4] == bytes(4) for offset in (state, test)),
                'Gate observation words differ.')
        return {'bytes': len(raw), 'sha256': sha(raw), 'state_offset': state, 'test_offset': test}


def first_stage(original, gate):
    require(len(original) == ORIGINAL_BYTES and sha(original) == PIN and
            original[4:12] == b'eGON.BT0' and struct.unpack_from('<I', original, 16)[0] == len(original) and
            struct.unpack_from('<I', original, 12)[0] == checksum(original), 'Pinned boot0 identity/header differs.')
    entry, = struct.unpack_from('<I', original)
    require(entry == 0xea0002be, 'Pinned boot0 initial branch differs.')
    require(0 < len(gate) <= 512, 'Gate exceeds its appended sector.')
    result = bytearray(original + gate + bytes(512 - len(gate)))
    struct.pack_into('<I', result, 0, 0xea000000 | ((len(original) - 8) // 4))
    struct.pack_into('<I', result, 16, len(result))
    struct.pack_into('<I', result, 12, checksum(result))
    return bytes(result)


def write_generated(path, raw):
    """Replace only an ordinary build artifact, keeping complete writes atomic."""
    if path.exists() or path.is_symlink():
        require(stat.S_ISREG(path.lstat().st_mode), 'Generated destination must be an ordinary file.')
    temporary = path.with_name(path.name + '.new-' + uuid.uuid4().hex)
    with temporary.open('xb') as stream:
        stream.write(raw)
        stream.flush()
        os.fsync(stream.fileno())
    temporary.replace(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('gate-elf', 'gate-bin', 'boot0', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location('fel_gate_package', ROOT/'tools/prepare-microsd-boot.py')
    package = importlib.util.module_from_spec(spec); spec.loader.exec_module(package)
    gate = package.read_regular(args.gate_bin, 512)
    info = gate_info(package.read_regular(args.gate_elf, 1 << 20), gate)
    original = package.read_regular(args.boot0, ORIGINAL_BYTES)
    image = first_stage(original, gate)
    args.output.mkdir(parents=True, exist_ok=True)
    parent = package.directory(args.output); os.close(parent)
    header = (f'#define CARD_FEL_GATE_BYTES {len(gate)}\n'
              f'#define CARD_FEL_STATE_OFFSET {info["state_offset"]}\n'
              f'#define CARD_FEL_TEST_OFFSET {info["test_offset"]}\n'
              '#define CARD_FEL_SHA256 {' + ','.join('0x'+info['sha256'][i:i+2] for i in range(0,64,2)) + '}\n')
    write_generated(args.output/'gate-info.h', header.encode())
    write_generated(args.output/'boot0-fel-first-stage.fex', image)
    manifest = {'schema': 'sunstone-card-fel-image-v1', 'physical_qualified': False,
                'original_sha256': PIN, 'gate': info,
                'first_stage': {'bytes': len(image), 'sha256': sha(image),
                                'first_sector': 16, 'sectors': len(image)//512},
                'changed_original_ranges': [[0,4],[12,8]],
                'storage_loader': 'Unchanged pinned vendor loader; no installation authority',
                'sources': {'boot/a733/card-fel-gate.S': sha((ROOT/'boot/a733/card-fel-gate.S').read_bytes()),
                            'tools/card-fel-image.py': sha(Path(__file__).read_bytes())}}
    write_generated(args.output/'first-stage.json', (json.dumps(manifest, indent=2)+'\n').encode())
    print(json.dumps(manifest, indent=2))


if __name__ == '__main__':
    main()
