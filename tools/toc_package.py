#!/usr/bin/env python3
"""Build an Allwinner TOC1 boot package from a configuration and its binary files.

This packs the same container as the vendor `dragonsecboot -pack` command: a
64-byte main header, one 368-byte record per item, each payload at a 1 KiB
boundary, and a total length rounded up to 16 KiB. Encryption, signing,
certificates and the TOC0 container are not implemented.
"""
import argparse
from pathlib import Path
import struct
import sys

MAGIC = 0x89119800
MAIN_END = 0x3b45494d
ITEM_END = 0x3b454949
STAMP = 0x5f0a6c39
ITEM_TYPE_BINFILE = 3
HEADER_BYTES, ITEM_BYTES = 64, 368
MAX_ITEMS = 16
NAME_BYTES = 64
DATA_ALIGN, TOTAL_ALIGN = 1024, 16384
# The vendor tool builds the package in a 10 MiB buffer and never checks the bound.
BUFFER_BYTES = 10 << 20
# The vendor parser reads lines through a 256-byte buffer and collects the
# section's lines in a 1024-byte buffer that holds at most 16 of them.
LINE_BYTES, SECTION_BYTES = 256, 1024
# The vendor tool prints this to its standard output; the wrapper records it as the log.
LOG_END = 'GetPrivateProfileSection read to end\n'  # printed when the section runs to the end of the file
LOG_COUNT = 'content_count={}\n'


def align(value, alignment):
    return (value + alignment - 1) & -alignment


def checksum(data):
    """Modulo 32-bit sum of little-endian words; the checksum field must hold STAMP."""
    usable = len(data) & ~3
    return sum(w[0] for w in struct.iter_unpack('<I', data[:usable])) & 0xffffffff


def pack(items):
    """Return the TOC1 package for (name, payload) pairs, in order."""
    items = list(items)
    if not 0 < len(items) <= MAX_ITEMS:
        raise ValueError('A package holds 1 to %d items' % MAX_ITEMS)
    offset = align(HEADER_BYTES + len(items) * ITEM_BYTES, DATA_ALIGN)
    records, placed = [], []
    for name, payload in items:
        encoded = name.encode() if isinstance(name, str) else bytes(name)
        if not encoded or len(encoded) >= NAME_BYTES or b'\0' in encoded:
            raise ValueError('Item name must be 1 to %d bytes without NUL' % (NAME_BYTES - 1))
        records.append(struct.pack('<64s6I69II', encoded, offset, len(payload), 0,
                                   ITEM_TYPE_BINFILE, 0, 0, *([0] * 69), ITEM_END))
        placed.append((offset, payload))
        offset = align(offset + len(payload), DATA_ALIGN)
    total = align(offset, TOTAL_ALIGN)
    if total > BUFFER_BYTES:
        raise ValueError('Package exceeds the 10 MiB the vendor tool supports')
    image = bytearray(total)
    image[HEADER_BYTES:HEADER_BYTES + len(records) * ITEM_BYTES] = b''.join(records)
    for start, payload in placed:
        image[start:start + len(payload)] = payload
    struct.pack_into('<16s12I', image, 0, b'sunxi-package', MAGIC, STAMP, 0, 0,
                     len(items), total, 0, 0, 0, 0, 0, MAIN_END)
    struct.pack_into('<I', image, 20, checksum(image))
    return bytes(image)


def unpack(data):
    """Decode a TOC1 package, checking every format field; return [(name, payload)]."""
    if len(data) < HEADER_BYTES or len(data) % TOTAL_ALIGN:
        raise ValueError('Package length is not a multiple of 16 KiB')
    head = struct.unpack_from('<16s12I', data)
    name, magic, total_sum, serial, status, count, valid, _, _, r0, r1, r2, end = head
    if name != b'sunxi-package' + bytes(3) or magic != MAGIC or end != MAIN_END:
        raise ValueError('Unexpected package name, magic or end marker')
    if (serial, status, r0, r1, r2) != (0, 0, 0, 0, 0) or head[7:9] != (0, 0):
        raise ValueError('Package reserved fields are not zero')
    if valid != len(data) or not 0 < count <= MAX_ITEMS:
        raise ValueError('Package length or item count mismatch')
    stamped = bytearray(data)
    struct.pack_into('<I', stamped, 20, STAMP)
    if checksum(stamped) != total_sum:
        raise ValueError('Package additive checksum mismatch')
    first = align(HEADER_BYTES + count * ITEM_BYTES, DATA_ALIGN)
    if any(data[HEADER_BYTES + count * ITEM_BYTES:first]):
        raise ValueError('Package table padding is not zero')
    items, expected = [], first
    for index in range(count):
        record = struct.unpack_from('<64s6I69II', data, HEADER_BYTES + index * ITEM_BYTES)
        raw, start, length, encrypt, kind, run_addr, item_index = record[:7]
        if record[7:76] != (0,) * 69 or record[76] != ITEM_END:
            raise ValueError('Item reserved fields or end marker mismatch')
        if (encrypt, kind, run_addr, item_index) != (0, ITEM_TYPE_BINFILE, 0, 0):
            raise ValueError('Item is encrypted, not a binary file or carries an address')
        if start != expected or start + length > len(data):
            raise ValueError('Item geometry mismatch')
        label = raw.rstrip(b'\0')
        if not label or b'\0' in label or len(raw) - len(label) < 1:
            raise ValueError('Item name malformed')
        items.append((label.decode(), bytes(data[start:start + length])))
        if any(data[start + length:align(start + length, DATA_ALIGN)]):
            raise ValueError('Item padding is not zero')
        expected = align(start + length, DATA_ALIGN)
    if align(expected, TOTAL_ALIGN) != len(data) or any(data[expected:]):
        raise ValueError('Package tail extent or padding mismatch')
    return items


def parse_config(text):
    """Return [(item name, file name)] from a package configuration."""
    return parse_config_log(text)[0]


def parse_config_log(text):
    """Return ([(item name, file name)], reached end of file) from a package configuration.

    Follows the vendor parser: the `[package]` section's lines, minus blank and
    `;`/`#` comment lines, each `key = item, file`; the key is ignored; `;` or `#`
    ends a line's value; item and file names are separated by spaces, tabs or commas.
    """
    section, lines, used, ended = False, [], 0, True
    for line in text.replace(b'\r\n', b'\n').replace(b'\r', b'\n').split(b'\n'):
        if len(line) >= LINE_BYTES - 1:
            raise ValueError('Configuration line exceeds %d bytes' % (LINE_BYTES - 2))
        line = line.lstrip(b' \t')
        if not line or line[:1] in b';#':
            continue
        if line[:1] == b'[':
            if section:
                ended = False
                break
            label = line[1:].lstrip(b' \t')
            for stop in (b']', b' ', b'\t'):
                label = label.split(stop)[0]
            section = label == b'package'
            continue
        if section:
            used += len(line) + 1
            lines.append(line)
    if used >= SECTION_BYTES or len(lines) > MAX_ITEMS:
        raise ValueError('Package section exceeds the vendor parser limits')
    if not lines:
        raise ValueError('No [package] section content')
    entries = []
    for line in lines:
        if b'=' not in line:
            raise ValueError('Package line has no "="')
        value = line.split(b'=', 1)[1].lstrip(b' \t')
        for stop in b';#':
            value = value.split(bytes([stop]))[0]
        names = value.replace(b'\t', b' ').replace(b',', b' ').split()
        if len(names) < 2:
            raise ValueError('Package line needs an item name and a file name')
        entries.append((names[0].decode(), names[1].decode()))
    return entries, ended


def pack_config(config, directory):
    """Pack the files named by a configuration, resolved from `directory`.

    Returns (package, log text); the log matches the vendor tool's standard output.
    """
    entries, ended = parse_config_log(Path(config).read_bytes())
    items = [(name, (Path(directory) / path).read_bytes()) for name, path in entries]
    return pack(items), (LOG_END if ended else '') + LOG_COUNT.format(len(items))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('config', type=Path, help='Package configuration (a [package] section)')
    parser.add_argument('--directory', type=Path, default=Path('.'),
                        help='Directory the configuration\'s file names resolve from')
    parser.add_argument('--output', type=Path, default=Path('boot_package.fex'))
    args = parser.parse_args()
    try:
        package, log = pack_config(args.config, args.directory)
        with args.output.open('xb') as stream:
            stream.write(package)
    except (ValueError, OSError) as error:
        parser.exit(1, str(error) + '\n')
    sys.stdout.write(log)


if __name__ == '__main__':
    main()
