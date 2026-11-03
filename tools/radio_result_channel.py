#!/usr/bin/env python3
"""Read the radio service's result from a returned board (include/os_radio_result.h).

The descriptor at the image symbol os_radio_result_descriptor names the record. The record is read whole: the
runner's FEL read takes any size (its timeout grows with the size), so no limit truncates a long scan list."""
import struct

import radio_result

MAGIC = b'SOSRASD1'
DESCRIPTOR_BYTES = 24


def read_record(read, descriptor, ram):
    """read(address, size, name) returns bytes. Returns the decoded record and the raw bytes."""
    if len(descriptor) != DESCRIPTOR_BYTES or descriptor[:8] != MAGIC:
        raise ValueError('radio result descriptor absent: the service published nothing')
    address, length = struct.unpack_from('<2Q', descriptor, 8)
    if address % 8 or length < radio_result.HEAD_BYTES or not (ram[0] <= address and address + length <= ram[1]):
        raise ValueError('radio result descriptor names memory outside image RAM')
    head = read(address, radio_result.HEAD_BYTES, 'radio-head')
    if radio_result.record_length(head) != length:
        raise ValueError('radio result length differs between its descriptor and its head')
    data = read(address, length, 'radio-result')
    if len(data) != length:
        raise ValueError('radio result read came back short')
    return radio_result.decode(data), data
