#!/usr/bin/env python3
"""Check the host reader of the radio result: descriptor, length from the head, long records, refusals."""
import pathlib
import struct
import sys

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root/'tools'))
import radio_result
import radio_result_channel as channel

RAM = (0x40000000, 0x80000000)


def record(aps):
    head = bytearray(radio_result.HEAD_BYTES)
    head[:8] = radio_result.MAGIC
    total = radio_result.HEAD_BYTES + radio_result.AP_BYTES * aps
    struct.pack_into('<4I', head, 8, 2, total, radio_result.HEAD_BYTES, radio_result.AP_BYTES)
    struct.pack_into('<4I', head, 24, 7, 0, 0, 0)          # scanned
    struct.pack_into('<I', head, 216, aps)
    struct.pack_into('<2I', head, 220, aps, 0)
    body = b''
    for i in range(aps):
        entry = bytearray(radio_result.AP_BYTES)
        entry[0:6] = bytes([2, 0, 0, 0, i >> 8, i & 255])
        entry[6] = 2
        entry[7] = (-40 - i % 50) & 255
        struct.pack_into('<H2B', entry, 8, 2412 + 5 * (i % 11), 1 + i % 11, 0)
        entry[12:14] = b'n0'
        body += bytes(entry)
    return bytes(head) + body


def memory_for(data, address=0x41234560):
    reads = []

    def read(a, size, name):
        reads.append((a, size, name))
        assert address <= a and a + size <= address + len(data)
        return data[a - address:a - address + size]
    return read, reads


for aps in (0, 3, 2000):
    data = record(aps)
    descriptor = channel.MAGIC + struct.pack('<2Q', 0x41234560, len(data))
    read, reads = memory_for(data)
    decoded, raw = channel.read_record(read, descriptor, RAM)
    assert raw == data and decoded['scan']['listed'] == aps and decoded['stage'] == 'scanned'
    # The head first, then the whole record in one read: nothing is truncated.
    assert reads == [(0x41234560, radio_result.HEAD_BYTES, 'radio-head'), (0x41234560, len(data), 'radio-result')]

data = record(3)
good = channel.MAGIC + struct.pack('<2Q', 0x41234560, len(data))
read, _ = memory_for(data)
for name, descriptor in [
        ('no descriptor', bytes(24)),
        ('short', good[:20]),
        ('outside RAM', channel.MAGIC + struct.pack('<2Q', 0x10000, len(data))),
        ('misaligned', channel.MAGIC + struct.pack('<2Q', 0x41234561, len(data))),
        ('length disagrees with the head', channel.MAGIC + struct.pack('<2Q', 0x41234560, len(data) + 44)),
        ('shorter than a head', channel.MAGIC + struct.pack('<2Q', 0x41234560, 16))]:
    try:
        channel.read_record(read, descriptor, RAM)
    except ValueError:
        continue
    raise SystemExit('accepted: ' + name)
print('PASS radio result channel: descriptor, length from the head, records of 0, 3 and 2000 access points read whole, six refusals')
