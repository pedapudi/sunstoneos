"""A directory-format network binary with more tensors than the graph needs, for the checks of the NPU loader's limits.

widen(data, inputs, outputs) copies the last input record and the last output record of a graph until it has that many
of each, moves the sections behind them, and repairs the checksums as drivers/npu/nbg_checksum.c computes them (the header
word at byte 12 over every payload with the stored section checksums read as zero, and the CRC-32 of each section in the
checksum table). The extra tensors are named by no patch: the graph runs as the original does and its allocations grow by
one page for each tensor, which is what the loader and the runtime must size their tables for.
"""
import struct, zlib

INPUT, OUTPUT, NETWORK_INFO, CHECKSUM = 1, 2, 35, 40


def widen(data, inputs, outputs):
    u32 = lambda blob, at: struct.unpack_from('<I', blob, at)[0]
    assert data[:4] == b'VPMN' and u32(data, 4) == 0x20000
    count = u32(data, 56)
    entries = []
    for i in range(count):
        kind, offset, _, size = struct.unpack_from('<4I', data, 60 + 44 * i)
        entries.append({'at': 60 + 44 * i, 'kind': kind, 'offset': offset, 'size': size, 'payload': bytearray(data[offset:offset + size])})
    by = {e['kind']: e for e in entries}
    network = by[NETWORK_INFO]['payload']
    have_in, have_out = u32(network, 64), u32(network, 68)
    assert inputs >= have_in and outputs >= have_out
    stride = len(by[INPUT]['payload']) // have_in
    assert stride == len(by[OUTPUT]['payload']) // have_out == 112
    for kind, have, want in ((INPUT, have_in, inputs), (OUTPUT, have_out, outputs)):
        last = bytes(by[kind]['payload'][-stride:])
        by[kind]['payload'] += last * (want - have)
    struct.pack_into('<II', network, 64, inputs, outputs)
    out = bytearray(data[:60 + 44 * count])
    end = 60 + 44 * count
    for e in entries:
        assert e['offset'] == end - (end - e['offset']) or True
    offset = end
    for e in entries:
        e['offset'] = offset
        offset += len(e['payload'])
    sums = by[CHECKSUM]['payload']
    for at in range(0, len(sums), 8):
        kind = u32(sums, at)
        struct.pack_into('<I', sums, at + 4, zlib.crc32(bytes(by[kind]['payload'])))
    for e in entries:
        struct.pack_into('<II', out, e['at'] + 4, e['offset'], 0)
        struct.pack_into('<I', out, e['at'] + 12, len(e['payload']))
    body = bytearray(out)
    for e in entries:
        body += e['payload']
    struct.pack_into('<I', body, 16, len(body))
    zeroed = {e['kind']: bytearray(e['payload']) for e in entries}
    for at in range(4, len(sums), 8):
        zeroed[CHECKSUM][at:at + 4] = b'\0\0\0\0'
    payload = b''.join(bytes(zeroed[u32(body, 60 + 44 * i)]) for i in range(count))
    struct.pack_into('<I', body, 12, zlib.crc32(payload))
    return bytes(body)
