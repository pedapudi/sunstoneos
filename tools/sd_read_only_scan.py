"""Scan a linked AArch64 ELF for the card write path.

One implementation serves the runner (tools/board_probe.py) and
checks/sd-read-only.py. It reads the symbol table and the instructions of every
executable load segment with no external disassembler, and reports:

- a defined symbol, local or global, of any function or object that names the
  write path (the names in WRITE_PATTERNS);
- a branch-with-link or branch whose target lies inside such a symbol;
- a missing symbol table, because without one neither test can run;
- a missing or wrong value of the read-only core's marker.

Symbol names alone are weak evidence: a renamed or inlined function carries
none. The scan therefore backs the read-only guarantee of the source build,
whose command funnel refuses CMD24, CMD25 and CMD38 (drivers/storage/sunxi_mmc.c)
and whose marker the scan requires; it does not replace them.
"""
import io
import re
import struct
from elftools.elf.elffile import ELFFile

MARKER = 'sunxi_mmc_read_only_build'
MARKER_VALUE = 0x52444f4e
WRITE_PATTERNS = (r'^sunxi_mmc_write', r'^next_write$', r'^write_admitted$', r'^a733_storage_update_',
                  r'^sunxi_mmc_erase', r'^storage_update_')
_WRITE = re.compile('|'.join(WRITE_PATTERNS))


def is_write_name(name):
    return bool(_WRITE.search(name))


def scan(image):
    """Return a list of violation strings; an empty list means the scan is clean."""
    data = image if isinstance(image, (bytes, bytearray)) else open(image, 'rb').read()
    elf = ELFFile(io.BytesIO(data))
    table = elf.get_section_by_name('.symtab')
    if table is None:
        return ['image has no symbol table']
    problems, ranges, marker = [], [], None
    for symbol in table.iter_symbols():
        if symbol['st_shndx'] == 'SHN_UNDEF' or not symbol.name:
            continue
        if symbol.name == MARKER:
            marker = symbol
        if is_write_name(symbol.name) and symbol['st_info']['type'] in ('STT_FUNC', 'STT_OBJECT', 'STT_NOTYPE'):
            problems.append(f'defines {symbol.name}')
            if symbol['st_info']['type'] == 'STT_FUNC' and symbol['st_size']:
                ranges.append((symbol['st_value'], symbol['st_value'] + symbol['st_size'], symbol.name))
    if marker is None:
        problems.append(f'lacks {MARKER}')
    else:
        section = elf.get_section(marker['st_shndx'])
        offset = marker['st_value'] - section['sh_addr']
        value = struct.unpack_from('<I', section.data(), offset)[0] if section['sh_type'] != 'SHT_NOBITS' else None
        if marker['st_size'] != 4 or value != MARKER_VALUE:
            problems.append(f'{MARKER} is not the read-only value')
    if ranges:
        for segment in elf.iter_segments():
            if segment['p_type'] != 'PT_LOAD' or not segment['p_flags'] & 1:
                continue
            code, base = segment.data(), segment['p_vaddr']
            for i in range(0, len(code) - 3, 4):
                word = struct.unpack_from('<I', code, i)[0]
                if word & 0x7c000000 != 0x14000000:      # B (0x14...) and BL (0x94...)
                    continue
                imm = word & 0x3ffffff
                target = base + i + 4 * (imm - (1 << 26) if imm & (1 << 25) else imm)
                for low, high, name in ranges:
                    if low <= target < high and not low <= base + i < high:
                        problems.append(f'call to {name} at {base + i:#x}')
    return problems
