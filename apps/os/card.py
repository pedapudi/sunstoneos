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
    """Flatten initialized PT_LOAD bytes below the reserved session page."""
    import io
    from elftools.elf.elffile import ELFFile
    raw = Path(image).read_bytes() if isinstance(image, (str, Path)) else bytes(image)
    elf = ELFFile(io.BytesIO(raw))
    segments = [p for p in elf.iter_segments() if p['p_type'] == 'PT_LOAD' and p['p_filesz']]
    require(segments and all(0x41000000 <= p['p_vaddr'] and p['p_vaddr'] + p['p_memsz'] <= 0x41f00000
                            for p in segments), 'Runtime overlaps reserved memory.')
    end = max(int(p['p_vaddr'] + p['p_filesz']) for p in segments)
    result = bytearray(end - 0x41000000)
    occupied = []
    for p in segments:
        address, data = int(p['p_vaddr']), p.data()
        require(all(address + len(data) <= low or high <= address for low, high in occupied), 'Overlapping load segments.')
        occupied.append((address, address + len(data)))
        offset = address - 0x41000000
        result[offset:offset + len(data)] = data
    return bytes(result)
