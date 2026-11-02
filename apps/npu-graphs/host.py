#!/usr/bin/env python3
"""Decode and judge the receipt of the NPU graph application (receipt.h); this module never opens a board."""
import argparse
import json
from pathlib import Path
import struct

MAGIC = 0x5048415247554e50
BYTES = 1728
STATUS = ('match', 'mismatch', 'refused', 'failed', 'timeout', 'not run')
HEADER = 16


def require(condition, message):
    if not condition:
        raise ValueError(message)


def decode(raw, names=None, sizes=None):
    """Decode the receipt. names and sizes (input and output bytes per graph) bind the listing the image was built from."""
    require(len(raw) == BYTES, 'The receipt must hold exactly 1728 bytes.')
    words = struct.unpack('<216Q', raw)
    magic, version, size, count, completed, matched, closed, start, stop = words[:9]
    require((magic, version, size) == (MAGIC, 1, BYTES), 'The receipt header differs (a failure record, or another image).')
    require(0 < count <= 16, 'The graph count is outside 1 to 16.')
    graphs = []
    for i in range(count):
        index, input_bytes, output_bytes, status, first, observed, expected, ticks = words[HEADER + 8 * i:HEADER + 8 * i + 8]
        require(index == i and status < len(STATUS), 'Graph record %d identity or status differs.' % i)
        row = {'index': i, 'name': names[i] if names else None, 'input_bytes': input_bytes, 'output_bytes': output_bytes,
               'status': STATUS[status], 'first_difference': first, 'observed': observed, 'expected': expected,
               'us': ticks / 24}
        if sizes:
            require((input_bytes, output_bytes) == tuple(sizes[i]), 'Graph %d sizes differ from the listing.' % i)
        row['differing_bytes'] = struct.unpack_from('<I', raw, 1152 + 4 * i)[0]
        row['max_error_16bit'] = struct.unpack_from('<I', raw, 1216 + 4 * i)[0]
        graphs.append(row)
    result = {'count': count, 'completed': completed, 'matched': matched, 'sdk_closed': closed,
              'elapsed_us': (stop - start) / 24, 'graphs': graphs}
    require(completed <= count and matched <= completed, 'The receipt counts disagree.')
    result['passed'] = (matched == count and completed == count and closed == 1 and
                        all(g['status'] == 'match' for g in graphs))
    return result


ANY = {'min': 0, 'max': 4294967295}


def expectation(sizes):
    """The queue expectation as u32 words: every word exact, except the times (start, stop, per-graph ticks)."""
    words64 = [MAGIC, 1, BYTES, len(sizes), len(sizes), len(sizes), 1, None, None] + [0] * 7
    for i, (inputs, outputs) in enumerate(sizes):
        words64 += [i, inputs, outputs, 0, 0, 0, 0, None]
    words64 += [0] * (BYTES // 8 - len(words64))
    words = []
    for value in words64:
        words += [ANY, ANY] if value is None else [value & 0xffffffff, value >> 32]
    return {'schema': 'sunstone-board-expect-u32-v1', 'words': words}


def freeze_decoder(destination, names, sizes):
    """Bind a standalone decode(payload) to the model list and tensor sizes the image was built from."""
    source = Path(__file__).read_text()
    source += '\nNAMES = %r\nSIZES = %r\n_bound_decode = decode\n' % (names, sizes)
    source += 'def decode(payload):\n    return _bound_decode(payload, NAMES, SIZES)\n'
    Path(destination).write_text(source)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('payload', type=Path)
    args = parser.parse_args()
    print(json.dumps(decode(args.payload.read_bytes()), sort_keys=True, indent=1))


if __name__ == '__main__':
    main()
