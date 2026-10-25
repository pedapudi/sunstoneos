#!/usr/bin/env python3
"""Record the pinned convolution's directory layout and checksum evidence.

This is a research check for one hash-pinned model, not a general NBG validator.
It reads the model file and the SDK inspector's debug information and computes
every checksum with Python zlib. No vendor code executes.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import struct
import zlib

from elftools.elf.elffile import ELFFile


MODEL_SHA = 'db42d815100417f4b8913896462ed031beafae35fb4ee1c28569ac950a9e8fca'
INSPECTOR_SHA = '5ef6bb4f1590e0d1b9603506574bee09321d6159e32a6b3aaef66148085b2639'


def pinned(path, expected):
    data = path.read_bytes()
    if hashlib.sha256(data).hexdigest() != expected:
        raise ValueError(f'Unexpected input identity: {path}')
    return data


def sdk_types(data):
    """Retain distinct definitions: the SDK reuses a patch type name across CUs."""
    wanted = {'_nbg_format_header', '_nbg_sub_header', '_nbg_section_header',
              '_nbg_section_network_info', '_nbg_network_attribute_t',
              '_nbg_desc_input_output', '_nbg_section_patch_table',
              '_nbg_section_patch_table_40bit', '_nbg_section_checksum',
              '_nbg_section_operation', '_nbg_section_entry_32bit',
              '_nbg_section_entry_64bit'}
    result = []
    dwarf = ELFFile(io.BytesIO(data)).get_dwarf_info()
    for cu in dwarf.iter_CUs():
        for die in cu.iter_DIEs():
            name = die.attributes.get('DW_AT_name')
            name = name.value.decode() if name else ''
            if die.tag != 'DW_TAG_structure_type' or name not in wanted:
                continue
            if 'DW_AT_byte_size' not in die.attributes:
                continue
            members = []
            for member in die.iter_children():
                if member.tag != 'DW_TAG_member':
                    continue
                item = {}
                for key in ('name', 'data_member_location', 'bit_size', 'bit_offset'):
                    attribute = member.attributes.get('DW_AT_' + key)
                    if attribute:
                        value = attribute.value
                        item[key] = value.decode() if isinstance(value, bytes) else value
                members.append(item)
            row = dict(name=name, bytes=die.attributes['DW_AT_byte_size'].value,
                       members=members)
            if row not in result:
                result.append(row)
    assert {row['name'] for row in result} == wanted
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('model', type=Path)
    ap.add_argument('inspector', type=Path)
    ap.add_argument('output', type=Path)
    args = ap.parse_args()
    data = pinned(args.model, MODEL_SHA)
    types = sdk_types(pinned(args.inspector, INSPECTOR_SHA))
    header = struct.unpack_from('<12I', data)
    assert header == (0x4e4d5056, 0x20000, 0x1000003b, 0xe5e19256,
                      1928, 0, 48, 12, 1, 0, 0, 0)
    directory, stride, count = struct.unpack_from('<3I', data, header[6])
    assert (directory, stride, count) == (60, 44, 13)
    sections = []
    previous_end = directory + stride * count
    for index in range(count):
        words = struct.unpack_from('<11I', data, directory + index * stride)
        kind, low, high, size_low, size_high, addr_low, addr_high, align, entry, r1, r2 = words
        offset, size = low | high << 32, size_low | size_high << 32
        assert offset == previous_end and 0 < size <= len(data) - offset
        assert entry and size % entry == 0
        assert not (addr_low or addr_high or r1 or r2)
        assert not align or offset % align == 0
        sections.append(dict(type=kind, offset=offset, bytes=size,
                             alignment=align, entry_bytes=entry, count=size // entry))
        previous_end = offset + size
    assert previous_end == len(data)
    by_type = {row['type']: row for row in sections}
    assert len(by_type) == count
    normalized = bytearray(data)
    checksum_section = by_type[40]
    checksum_rows = []
    for at in range(checksum_section['offset'],
                    checksum_section['offset'] + checksum_section['bytes'], 8):
        kind, stored = struct.unpack_from('<2I', data, at)
        section = by_type[kind]
        payload = data[section['offset']:section['offset'] + section['bytes']]
        computed = zlib.crc32(payload)
        assert computed == stored
        checksum_rows.append(dict(type=kind, stored=hex(stored), computed=hex(computed)))
        normalized[at + 4:at + 8] = bytes(4)
    start = sections[0]['offset']
    computed = zlib.crc32(normalized[start:])
    assert computed == header[3]
    cumulative = 0
    for section in sections:
        chunk = normalized[section['offset']:section['offset'] + section['bytes']]
        cumulative = zlib.crc32(chunk, cumulative)
    assert cumulative == computed
    records = {}
    for kind in (4, 10, 23, 27):
        section = by_type[kind]
        width = section['entry_bytes']
        records[str(kind)] = [list(struct.unpack_from('<' + 'I' * (width // 4), data, at))
                             for at in range(section['offset'],
                                             section['offset'] + section['bytes'], width)]
    tensors = []
    for kind in (1, 2):
        at = by_type[kind]['offset']
        fields = struct.unpack_from('<12I', data, at)
        tensors.append(dict(type=kind, dimensions=fields[0], sizes=fields[1:7],
                            data_format=fields[7], data_type=fields[8],
                            quant_format=fields[9], scale_bits=hex(fields[10]),
                            scale=struct.unpack_from('<f', data, at + 40)[0],
                            zero_point=fields[11],
                            name=data[at + 48:at + 112].split(b'\0')[0].decode()))
    # These comparisons demonstrate coverage limits; they do not admit mutations.
    header_mutation = bytearray(normalized)
    header_mutation[8] ^= 1
    assert zlib.crc32(header_mutation[start:]) == computed
    payload_mutation = bytearray(normalized)
    payload_mutation[start] ^= 1
    assert zlib.crc32(payload_mutation[start:]) != computed
    report = dict(result='pass', model_sha256=MODEL_SHA, inspector_sha256=INSPECTOR_SHA,
                  header_words=header,
                  sections=sections, sdk_types=types, tensors=tensors, records=records,
                  section_checksums=checksum_rows, header_checksum=dict(
                      stored=hex(header[3]), computed=hex(computed), payload_start=start,
                      normalization='zero checksum values in section 40; retain type IDs',
                      excludes='format header, subheader, section directory',
                      evidence_scope='one contiguous, unpadded, hash-pinned fixture'),
                  loader_executed=False, physical_access=False)
    args.output.mkdir(parents=True, exist_ok=False)
    (args.output / 'comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'PASS: {count} sections; three section CRCs; payload CRC {computed:#010x}; '
          f'incremental CRC across {count} sections.')


if __name__ == '__main__':
    main()
