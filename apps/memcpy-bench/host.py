#!/usr/bin/env python3
"""Host side of memcpy-bench: the decoder of its result (apps/memcpy-bench/main.c)."""
import struct

HEADER = struct.Struct('<IIQ')
RECORD = struct.Struct('<IIIIQQ')
ROUTINES = ('strict memcpy', 'unaligned memcpy', 'strict memset', 'unaligned memset')


def decode(payload):
    """Cases with the throughput of each, in MB/s (10**6 bytes a second), from the 24 MHz counter."""
    cases, _, hz = HEADER.unpack_from(payload, 0)
    if not cases or cases > 96 or len(payload) < HEADER.size + 96 * RECORD.size:
        raise ValueError('memcpy-bench result has %d cases in %d bytes.' % (cases, len(payload)))
    rows = []
    for index in range(cases):
        routine, size, src, dst, iterations, ticks = RECORD.unpack_from(payload, HEADER.size + index * RECORD.size)
        if routine >= len(ROUTINES) or not ticks:
            raise ValueError('memcpy-bench case %d is malformed.' % index)
        seconds = ticks / hz
        rows.append({'routine': ROUTINES[routine], 'size': size, 'src_offset': src, 'dst_offset': dst,
                     'iterations': iterations, 'ticks': ticks,
                     'mb_per_s': round(size * iterations / seconds / 1e6, 1),
                     'ns_per_call': round(seconds / iterations * 1e9, 1)})
    return {'cases': rows, 'counter_hz': hz}
