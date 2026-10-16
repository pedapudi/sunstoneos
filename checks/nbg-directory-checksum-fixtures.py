#!/usr/bin/env python3
"""Make synthetic gap and zero-CRC cases from the pinned numerical model.

Python zlib supplies the independent CRC reference. These files exercise checksum
coverage only; they are not compiler-generated or admitted executable models.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib


def normalized(data):
    value = bytearray(data[632:])
    for at in (860, 868, 876):
        value[at - 632:at - 628] = bytes(4)
    return value


def solve_suffix(data):
    baseline = zlib.crc32(data)
    pivots = {}
    for bit in range(32):
        sample = bytearray(data); sample[-4 + bit // 8] ^= 1 << (bit % 8)
        value, mask = zlib.crc32(sample) ^ baseline, 1 << bit
        while value:
            pivot = value.bit_length() - 1
            if pivot not in pivots:
                pivots[pivot] = value, mask
                break
            previous, combination = pivots[pivot]
            value ^= previous; mask ^= combination
    value, suffix = baseline, 0
    while value:
        previous, combination = pivots[value.bit_length() - 1]
        value ^= previous; suffix ^= combination
    return struct.pack('<I', suffix)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('model', type=Path)
    ap.add_argument('output', type=Path)
    args = ap.parse_args()
    original = args.model.read_bytes()
    assert hashlib.sha256(original).hexdigest() == 'db42d815100417f4b8913896462ed031beafae35fb4ee1c28569ac950a9e8fca'
    assert zlib.crc32(normalized(original)) == 0xe5e19256
    gap = bytearray(original[:1600] + bytes(64) + original[1600:])
    struct.pack_into('<I', gap, 16, len(gap))
    for index in (11, 12):
        at = 60 + index * 44 + 4
        offset, = struct.unpack_from('<I', gap, at)
        struct.pack_into('<I', gap, at, offset + 64)
    struct.pack_into('<I', gap, 12, zlib.crc32(normalized(gap)))
    zero = bytearray(original + bytes(4))
    struct.pack_into('<I', zero, 16, len(zero))
    struct.pack_into('<I', zero, 12, 0)
    zero[-4:] = solve_suffix(normalized(zero))
    assert zlib.crc32(normalized(zero)) == 0
    args.output.mkdir(parents=True, exist_ok=False)
    rows = []
    for filename, data in [('gap.nb', gap), ('zero.nb', zero)]:
        (args.output/filename).write_bytes(data)
        rows.append(dict(file=filename, bytes=len(data),
                         sha256=hashlib.sha256(data).hexdigest(),
                         primary_crc=hex(zlib.crc32(normalized(data)))))
    (args.output/'reference.json').write_text(json.dumps(dict(result='pass', fixtures=rows,
                                                            compiler_executed=False), indent=2)+'\n')
    print(json.dumps(rows, indent=2))


if __name__ == '__main__':
    main()
