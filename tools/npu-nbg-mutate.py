#!/usr/bin/env python3
"""Change bytes of a directory-format network binary and repair its checksums.

The checksums follow drivers/npu/nbg_checksum.c: the checksum table lists section identifiers and the CRC-32 of
each section payload, and the header word at byte 12 holds the CRC-32 of every section payload with the stored
section checksums read as zero. The repaired file is checked with the OS checksum code before it is written.
Use it to test what a field does: mutate one field of a copy of a graph, run the copy in the SDK simulator and
compare the output with the prediction.

  npu-nbg-mutate.py IN OUT --native LIB --word STATE_OPERATION INDEX VALUE ...   (load-state word of an operation)
  npu-nbg-mutate.py IN OUT --native LIB --descriptor OPERATION WORD VALUE ...    (descriptor word of a neural-network record)
  npu-nbg-mutate.py IN OUT --native LIB --bytes SECTION OFFSET HEX ...           (raw bytes of a section payload)

LIB is a shared library of the OS decoder and checksum code (tools/npu-nbg-census.py builds one).
"""
import argparse, ctypes as c, importlib.util, struct, sys, zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('nbg_model', ROOT / 'checks/nbg-model.py')
nm = importlib.util.module_from_spec(spec); spec.loader.exec_module(nm)
SECTION = {'nn': 6, 'load_states': 20, 'dyna_io': 19, 'ppu_instr': 18, 'coeff': 23}


def u32(data, at): return struct.unpack_from('<I', data, at)[0]


def repair(data, native_path):
    data = bytearray(data)
    view = nm.Native(native_path, bytes(data)).view
    sums = view.sections[39]            # NBG_SECTION_CHECKSUM
    for at in range(0, sums.bytes, 8):
        kind = u32(data, sums.offset + at)
        s = view.sections[kind - 1]     # as drivers/npu/nbg_checksum.c reads the table
        struct.pack_into('<I', data, sums.offset + at + 4, zlib.crc32(bytes(data[s.offset:s.offset + s.bytes])))
    zeroed = bytearray(data)
    for i in range(sums.offset + 4, sums.offset + sums.bytes, 8):
        zeroed[i:i + 4] = b'\0\0\0\0'
    payload = b''
    for entry in range(u32(data, 56)):  # directory order, as the OS checksum code walks it
        p = 60 + entry * 44
        offset, length = u32(data, p + 4), u32(data, p + 12)
        payload += bytes(zeroed[offset:offset + length])
    struct.pack_into('<I', data, 12, zlib.crc32(payload))
    return bytes(data)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('input', type=Path); ap.add_argument('output', type=Path)
    ap.add_argument('--native', type=Path, required=True)
    ap.add_argument('--word', nargs=3, action='append', default=[], metavar=('OP', 'INDEX', 'VALUE'))
    ap.add_argument('--descriptor', nargs=3, action='append', default=[], metavar=('OP', 'WORD', 'VALUE'))
    ap.add_argument('--bytes', nargs=3, action='append', default=[], metavar=('SECTION', 'OFFSET', 'HEX'))
    a = ap.parse_args()
    data = bytearray(a.input.read_bytes())
    native = nm.Native(a.native, bytes(data))
    sections = native.view.sections
    for op, index, value in a.word:
        sp = nm.Span()
        assert native.lib.nbg_model_table(c.byref(native.view), 26, int(op), c.byref(sp)) == 1
        at = sp.data - native.view.data + int(index) * 4
        struct.pack_into('<I', data, at, int(value, 0))
    for op, word, value in a.descriptor:
        operation = native.operations()[int(op)]
        struct.pack_into('<I', data, sections[6].offset + operation[1] * 192 + int(word) * 4, int(value, 0))
    for section, offset, hexa in a.bytes:
        s = sections[SECTION.get(section, None) if section in SECTION else int(section)]
        raw = bytes.fromhex(hexa)
        data[s.offset + int(offset):s.offset + int(offset) + len(raw)] = raw
    out = repair(bytes(data), a.native)
    a.output.write_bytes(out)
    check = c.CDLL(str(a.native.resolve())).nbg_checksum_verify
    check.argtypes = [c.c_char_p, c.c_size_t, c.c_void_p]
    result = (c.c_uint32 * 2)()
    verdict = check(out, len(out), result)
    print('checksum', {1: 'match', 2: 'mismatch', 0: 'invalid'}[verdict])
    return 0 if verdict == 1 else 1


if __name__ == '__main__':
    sys.exit(main())
