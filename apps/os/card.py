#!/usr/bin/env python3
"""Offline typed-session checks used by backup-first card packaging.

Card allocation, backup identity and writes remain in tools/boot-slots.py and
its storage update path. This module grants no storage or physical authority.
"""
import importlib.util
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('card_host_records', ROOT / 'apps/os/host.py')
host = importlib.util.module_from_spec(spec)
spec.loader.exec_module(host)
require = host.require


def canonical_session(raw):
    decoded = host.session_decode(raw)
    result = bytearray(raw)
    result[24:40] = bytes(16)
    offset = decoded['offsets'].get(host.CPU_GRANT)
    if offset is not None:
        result[offset + 32:offset + 48] = bytes(16)
        result[offset + 64:offset + 96] = bytes(32)
    return bytes(result)


def template_check(raw, image):
    info = host.image_manifest(image)
    decoded = host.session_decode(raw, info['manifest'], info['sha256'])
    require(struct.unpack_from('<2Q', raw, 24) == (0, 0), 'Cold session requires zero epoch and generation.')
    if decoded['flags'] & host.BIND_SID:
        require(struct.unpack_from('<Q', raw, 312)[0] and any(raw[320:352]), 'Cold SID binding required.')
    grant = decoded['extents'].get(host.CPU_GRANT)
    if grant is not None:
        require(struct.unpack_from('<2Q', grant, 32) == (0, 0), 'Cold CPU grant requires zero epoch and generation.')
    return decoded


def flat_runtime(image):
    """Validate allocated sections and flatten only initialized runtime bytes.

    Load segments can span linker gaps and the NOBITS session reservation.
    The named monitor reservations also have no file bytes to upload.
    """
    import io
    from elftools.elf.elffile import ELFFile
    raw = Path(image).read_bytes() if isinstance(image, (str, Path)) else bytes(image)
    elf = ELFFile(io.BytesIO(raw))
    loads = [p for p in elf.iter_segments() if p['p_type'] == 'PT_LOAD']
    allocated = [s for s in elf.iter_sections() if s['sh_flags'] & 2 and s['sh_size']]
    session = [s for s in allocated if s.name == '.storage_session']
    require(len(session) <= 1, 'Duplicate session reservation.')
    reservations = {'.storage_session': (0x41f00000, 0x41f01000),
                    '.result': (0x42000000, 0x42004000),
                    '.contract': (0x42004000, 0x42005000),
                    '.monitor_bss': (0x42005000, 0x421f0000),
                    '.boot_stack': (0x42005000, 0x421f0000),
                    '.tables': (0x421f0000, 0x42200000)}
    occupied = []
    for section in allocated:
        low, high = int(section['sh_addr']), int(section['sh_addr'] + section['sh_size'])
        if section.name in reservations:
            start, end = reservations[section.name]
            valid = (section['sh_type'] == 'SHT_NOBITS' and section['sh_flags'] & 3 == 3 and
                     not section['sh_flags'] & 4 and start <= low < high <= end)
            if section.name in ('.storage_session', '.result', '.contract'):
                valid = valid and (low, high) == (start, end)
            elif section.name == '.monitor_bss':
                valid = valid and low == start
            elif section.name == '.boot_stack':
                valid = valid and low % 4096 == 0 and high - low == 16384
            elif section.name == '.tables':
                valid = valid and low == start
            require(valid, 'Invalid fixed runtime reservation: ' + section.name)
        else:
            require(0x41000000 <= low < high <= 0x41f00000,
                    'Allocated section overlaps reserved memory: ' + section.name)
        require(all(high <= start or end <= low for start, end in occupied),
                'Overlapping allocated runtime sections.')
        occupied.append((low, high))
        require(any(p['p_vaddr'] <= low and high <= p['p_vaddr'] + p['p_memsz'] for p in loads),
                'Allocated runtime section has no load segment: ' + section.name)
        if section['sh_type'] != 'SHT_NOBITS':
            require(any(p['p_vaddr'] <= low and high <= p['p_vaddr'] + p['p_filesz'] and
                        section['sh_offset'] - p['p_offset'] == low - p['p_vaddr'] for p in loads),
                    'Initialized runtime section has no file backing: ' + section.name)
    occupied = []
    require(loads, 'Runtime has no load segment.')
    for p in loads:
        low, high = int(p['p_vaddr']), int(p['p_vaddr'] + p['p_memsz'])
        require(0 <= p['p_filesz'] <= p['p_memsz'], 'Invalid runtime load size.')
        if high == low:
            continue
        image_end = 0x41f01000 if session else 0x41f00000
        require((0x41000000 <= low < high <= image_end) or
                (not p['p_filesz'] and 0x42000000 <= low < high <= 0x42200000),
                'Runtime load overlaps reserved memory.')
        require(not p['p_filesz'] or low + p['p_filesz'] <= 0x41f00000,
                'Initialized runtime bytes overlap reserved memory.')
        require(all(high <= start or end <= low for start, end in occupied), 'Overlapping load memory.')
        occupied.append((low, high))
    segments = [p for p in loads if p['p_filesz']]
    require(segments, 'Runtime has no initialized load segment.')
    end = max(int(p['p_vaddr'] + p['p_filesz']) for p in segments)
    result = bytearray(end - 0x41000000)
    for p in segments:
        address, data = int(p['p_vaddr']), p.data()
        require(len(data) == p['p_filesz'], 'Truncated runtime load bytes.')
        offset = address - 0x41000000
        result[offset:offset + len(data)] = data
    return bytes(result)
