#!/usr/bin/env python3
"""Host side of heap-test: the expected result of each image mode and a decoder.

The image (apps/heap-test/main.c) reports a 128-byte result that holds no time
and no address that varies between boots, so the host computes it exactly.
Mode 0 uses the heap through malloc; mode 3 shares it with an attached worker.
Modes 1 and 2 end the session with a permission fault and have no result.

    host.py expected MODE OUTPUT   write the expected result bytes
    host.py decode FILE            print a result as JSON
"""
import json
import struct
import sys

import numpy

RESULT = struct.Struct('<16Q')
FIELDS = ('tag', 'mode', 'base', 'bytes', 'in_range', 'nonzero_words', 'fold_written', 'fold_read', 'out_of_memory',
          'reused', 'worker_ran', 'worker_core', 'same_tag', 'main_tag_set', 'notify_count', 'worker_readback')
TAG_OK = 0x48454150544f4b
PATTERN = 0x9e3779b97f4a7c15
BASE, BYTES = 0x80000000, 1 << 30
TAKE = 512 << 20


def fold(words):
    """The XOR of PATTERN * (i + 1) modulo 2^64 for i below words, in blocks that fit in memory."""
    total = numpy.uint64(0)
    step = 1 << 22
    with numpy.errstate(over='ignore'):
        for start in range(0, words, step):
            count = min(step, words - start)
            index = numpy.arange(start + 1, start + count + 1, dtype=numpy.uint64)
            total ^= numpy.bitwise_xor.reduce(index * numpy.uint64(PATTERN))
    return int(total)


def expected(mode):
    if mode == 0:
        value = fold(TAKE // 8)
        values = (TAG_OK, 0, BASE, BYTES, 1, 0, value, value, 1, 1)
    elif mode == 3:
        value = fold(4096)
        values = (TAG_OK, 3, BASE, BYTES, 0, 0, value, value, 0, 0, 1, 3, 1, 1, 1, value)
    else:
        raise ValueError('mode %d ends in a fault and has no result' % mode)
    return RESULT.pack(*values + (0,) * (16 - len(values))) + bytes(128 - RESULT.size)


def decode(payload):
    if len(payload) != 128 or any(payload[RESULT.size:]):
        raise ValueError('heap-test result is 128 bytes with a zero tail')
    return dict(zip(FIELDS, RESULT.unpack_from(payload)))


if __name__ == '__main__':
    if len(sys.argv) == 4 and sys.argv[1] == 'expected':
        open(sys.argv[3], 'wb').write(expected(int(sys.argv[2])))
    elif len(sys.argv) == 3 and sys.argv[1] == 'decode':
        print(json.dumps({k: hex(v) for k, v in decode(open(sys.argv[2], 'rb').read()).items()}, indent=2))
    else:
        raise SystemExit(__doc__)
