#!/usr/bin/env python3
"""Compare native staging with ELF segments from the pinned MIPS GPU firmware."""
import argparse
import hashlib
import io
import json
from pathlib import Path
from elftools.elf.elffile import ELFFile

SHA = '1db1c399c17401d1f79d46c880db81c724d748c784d4639433b076aba2f9c0d2'


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('firmware', type=Path)
    ap.add_argument('code', type=Path)
    ap.add_argument('data', type=Path)
    ap.add_argument('report', type=Path)
    args = ap.parse_args()
    raw = args.firmware.read_bytes()
    assert hashlib.sha256(raw).hexdigest() == SHA
    elf = ELFFile(io.BytesIO(raw))
    # Allocation mapping from the pinned firmware layout, independently of
    # native decoder output. Include zero padding and boot-data/stack pages.
    regions = [(0xc0000000, 94208, 0, 0), (0x9fc02000, 4096, 0, 94208),
               (0xbfc00000, 4096, 0, 98304), (0xc0032000, 12288, 1, 0),
               (0xbfc01000, 4096, 1, 12288), (0xcf600000, 4096, 1, 16384)]
    expected = [bytearray(102400), bytearray(20480)]
    loads = []
    for segment in elf.iter_segments():
        if segment['p_type'] != 'PT_LOAD':
            continue
        addr, mem, data = segment['p_vaddr'], segment['p_memsz'], segment.data()
        matches = [r for r in regions if r[0] <= addr and addr + mem <= r[0] + r[1]]
        assert len(matches) == 1
        base, _, buffer, offset = matches[0]
        start = offset + addr - base
        expected[buffer][start:start + len(data)] = data
        loads.append(dict(address=hex(addr), file_bytes=len(data), memory_bytes=mem))
    for path, data in zip((args.code, args.data), expected):
        assert path.read_bytes() == data, path
    args.report.write_text(json.dumps(dict(
        result='pass', firmware_sha256=SHA, load_segments=loads,
        allocation_bytes=[len(b) for b in expected],
        staged_sha256=[hashlib.sha256(b).hexdigest() for b in expected],
        boot_parameters_applied=False, physical_access=False), indent=2) + '\n')
    print('PASS native code/data allocations match all ELF loads, zero-fill and padding')


if __name__ == '__main__':
    main()
