#!/usr/bin/env python3
"""Write the recovery-region beacon words into the linked memory qualification image.

Each of the four recovery pages that apps/memtest/link.ld places from
__memtest_recovery_start holds a beacon in every 64-bit word at offsets
0x7c0-0xfff: the word's own physical address XOR MEMTEST_K (memtest.h). The
linker leaves those bytes zero. The tool refuses a nonzero byte that is not
already the beacon, then patches the ELF in place; the build converts the
patched ELF to the uploaded binary. Standard library only, because the build
runs it with the configure-time Python.
"""
from pathlib import Path
import struct
import sys

K = 0xa5c35a3c00000000
PAGES = 4
FIRST, END = 0x7c0, 0x1000


def segments_and_symbol(data, name):
    assert data[:4] == b'\x7fELF' and data[4] == 2 and data[5] == 1, 'expected a little-endian ELF64 file'
    phoff, shoff = struct.unpack_from('<QQ', data, 0x20)
    phentsize, phnum, shentsize, shnum = struct.unpack_from('<HHHH', data, 0x36)
    loads = []
    for i in range(phnum):
        kind, _, offset, vaddr, _, filesz, _, _ = struct.unpack_from('<IIQQQQQQ', data, phoff + i * phentsize)
        if kind == 1:
            loads.append((vaddr, offset, filesz))
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * shentsize) for i in range(shnum)]
    symtab = next(s for s in sections if s[1] == 2)
    strtab = sections[symtab[6]]
    for offset in range(symtab[4], symtab[4] + symtab[5], 24):
        name_offset, _, _, _, value, _ = struct.unpack_from('<IBBHQQ', data, offset)
        start = strtab[4] + name_offset
        if data[start:data.index(b'\0', start)].decode() == name:
            return loads, value
    raise SystemExit(f'{name} is not defined')


def file_offset(loads, address):
    for vaddr, offset, filesz in loads:
        if vaddr <= address and address + 8 <= vaddr + filesz:
            return offset + address - vaddr
    raise SystemExit(f'beacon word {address:#x} is not loaded from the file')


def main():
    if len(sys.argv) != 2:
        raise SystemExit('usage: beacons.py IMAGE.elf')
    path = Path(sys.argv[1])
    data = bytearray(path.read_bytes())
    loads, base = segments_and_symbol(data, '__memtest_recovery_start')
    assert base % 4096 == 0, 'recovery region must be page aligned'
    for page in range(PAGES):
        for word in range(base + page * 4096 + FIRST, base + page * 4096 + END, 8):
            offset = file_offset(loads, word)
            current, = struct.unpack_from('<Q', data, offset)
            if current not in (0, word ^ K):
                raise SystemExit(f'beacon word {word:#x} holds {current:#x}, not padding')
            struct.pack_into('<Q', data, offset, word ^ K)
    path.write_bytes(data)


if __name__ == '__main__':
    main()
