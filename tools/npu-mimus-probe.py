#!/usr/bin/env python3
"""Probe the NN descriptor of a compiled mimus linear layer for a 32-bit output code.

Run in the pinned compiler image or on the host; needs only the Python 3.8
standard library. The input is a 0x20000 network binary graph (NBG) written
by tools/npu-mimus-linear.sh for the INT16 form. Patched graphs are
simulator-only probe artifacts: they never enter the session catalogue and
never reach a board.

  diff BASE OTHER...        prints every 32-bit word of the 192-byte NN and
                            neural-transpose records, and every other section
                            byte, that differs from BASE.
  patch IN OUT --code C     writes output data-type code C into the fully
                            connected record and repairs the checksums.
                            --wide N also doubles that record's output
                            strides and buffer, sets the following transpose
                            record's element word to N, doubles its strides
                            and buffer and declares an INT32 output tensor.
  evaluate STUDY OUTPUT     compares a simulator output file with the exact
                            integer sums of the study, read as INT16 and as
                            INT32 elements.

Field positions come from comparing compiled graphs that differ only in
output type or output step (`diff`). The output data type of a fully
connected record is three bits: bits 4-5 of word 1 and bit 21 of word 15.
"""
import argparse
import json
from pathlib import Path
import struct
import zlib

RECORD = 192
NN_SECTION, OUTPUT_SECTION, CHECKSUM_SECTION = 6, 1, 39


def word(data, at):
    return struct.unpack_from('<I', data, at)[0]


def directory(data):
    """Section index -> (offset, bytes) from the 44-byte directory records."""
    if word(data, 0) != 0x4e4d5056 or word(data, 4) != 0x20000 or word(data, 48) != 60:
        raise ValueError('not a 0x20000 directory graph')
    result = {}
    for i in range(word(data, 56)):
        kind, offset, _, length = struct.unpack_from('<IIII', data, 60 + i * 44)
        result[kind - 1] = (offset, length)
    return result


def repair(data):
    """Recompute listed section CRCs, then the primary CRC over the payloads."""
    data = bytearray(data)
    sections = directory(data)
    sums, length = sections[CHECKSUM_SECTION]
    for at in range(sums, sums + length, 8):
        offset, size = sections[word(data, at) - 1]
        struct.pack_into('<I', data, at + 4, zlib.crc32(bytes(data[offset:offset + size])))
    value = 0
    for i in range(word(data, 56)):
        _, offset, _, size = struct.unpack_from('<IIII', data, 60 + i * 44)
        payload = bytearray(data[offset:offset + size])
        if offset == sums:
            for at in range(4, size, 8):
                payload[at:at + 4] = bytes(4)
        value = zlib.crc32(bytes(payload), value)
    struct.pack_into('<I', data, 12, value)
    return bytes(data)


def records(data):
    offset, length = directory(data)[NN_SECTION]
    return [list(struct.unpack_from('<48I', data, offset + r * RECORD)) for r in range(length // RECORD)]


def put_records(data, rows):
    data = bytearray(data)
    offset, _ = directory(data)[NN_SECTION]
    for r, row in enumerate(rows):
        struct.pack_into('<48I', data, offset + r * RECORD, *row)
    return bytes(data)


def diff(args):
    base = args.base.read_bytes()
    if repair(base) != base:
        raise ValueError('checksum repair does not reproduce the compiled graph')
    result = {}
    for path in args.others:
        other = path.read_bytes()
        words = [dict(record=r, word=w, base=f'{a:08x}', other=f'{b:08x}', xor=f'{a ^ b:08x}')
                 for r, (ra, rb) in enumerate(zip(records(base), records(other)))
                 for w, (a, b) in enumerate(zip(ra, rb)) if a != b]
        sections = {}
        for index, (offset, size) in directory(base).items():
            if index == NN_SECTION or index not in directory(other):
                continue
            o, s = directory(other)[index]
            a, b = base[offset:offset + size], other[o:o + s]
            if a != b:
                sections[index] = dict(bytes=[size, s], first_offsets=[i for i in range(min(size, s))
                                                                        if a[i] != b[i]][:16])
        result[str(path)] = dict(records=words, sections=sections)
    text = json.dumps(result, indent=1)
    if args.report:
        args.report.write_text(text + '\n')
    print(text)


def fully_connected(rows):
    """The record whose kernel depth is nonzero; the transposes have none."""
    found = [r for r, row in enumerate(rows) if (row[0] >> 6) & 0x3fff]
    if len(found) != 1 or found[0] + 1 >= len(rows):
        raise ValueError('expected one fully connected record followed by a transpose')
    return found[0]


def high(value, factor):
    return (value & 0xffff) | (((value >> 16) * factor) & 0xffff) << 16


def patch(args):
    data = args.input.read_bytes()
    rows = records(data)
    fc = fully_connected(rows)
    row = rows[fc]
    row[1] = (row[1] & ~0x30) | (args.code & 3) << 4
    row[15] = (row[15] & ~(1 << 21)) | ((args.code >> 2) & 1) << 21
    if args.wide is not None:
        # Output y stride (word 8 high), x stride (word 17 low) and circular
        # buffer size in 64-byte units (word 18 low 26 bits).
        row[8] = high(row[8], 2)
        row[17] = (row[17] & ~0xffff) | ((row[17] & 0xffff) * 2)
        row[18] = (row[18] & ~0x3ffffff) | ((row[18] & 0x3ffffff) * 2)
        nt = rows[fc + 1]
        nt[5] = args.wide
        nt[8] = high(nt[8], 2)
        nt[10] *= 2
        nt[16] = high((nt[16] & ~0xffff) | ((nt[16] & 0xffff) * 2), 2)
        nt[17] *= 2
        nt[20] = (nt[20] & ~0x3ffffff) | ((nt[20] & 0x3ffffff) * 2)
    data = bytearray(put_records(data, rows))
    if args.wide is not None:
        offset, _ = directory(data)[OUTPUT_SECTION]
        struct.pack_into('<I', data, offset + 4, word(data, offset + 4) * 2)
    data = repair(data)
    args.output.write_bytes(data)
    print(json.dumps(dict(output=str(args.output), code=args.code, wide=args.wide, bytes=len(data),
                          checksum=f'{word(data, 12):08x}')))


def evaluate(args):
    import numpy as np
    sums = np.load(str(args.study / 'sums.npy'))
    raw = args.output.read_bytes()
    result = dict(output=str(args.output), bytes=len(raw), sums=int(sums.size))
    for name, dtype in (('int16', '<i2'), ('int32', '<i4')):
        values = np.frombuffer(raw[:len(raw) // np.dtype(dtype).itemsize * np.dtype(dtype).itemsize], dtype)
        n = min(values.size, sums.size)
        result[name] = dict(elements=int(values.size), equal_to_sums=int(np.sum(values[:n] == sums.ravel()[:n])),
                            distinct_values=int(np.unique(values).size))
    result['exact'] = result['int32']['elements'] == sums.size and result['int32']['equal_to_sums'] == sums.size
    text = json.dumps(result)
    if args.report:
        args.report.write_text(text + '\n')
    print(text)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='command', required=True)
    d = sub.add_parser('diff')
    d.add_argument('base', type=Path)
    d.add_argument('others', type=Path, nargs='+')
    d.add_argument('--report', type=Path)
    p = sub.add_parser('patch')
    p.add_argument('input', type=Path)
    p.add_argument('output', type=Path)
    p.add_argument('--code', type=int, required=True, choices=range(8))
    p.add_argument('--wide', type=int)
    e = sub.add_parser('evaluate')
    e.add_argument('study', type=Path)
    e.add_argument('output', type=Path)
    e.add_argument('--report', type=Path)
    args = ap.parse_args()
    {'diff': diff, 'patch': patch, 'evaluate': evaluate}[args.command](args)


if __name__ == '__main__':
    main()
