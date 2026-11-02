#!/usr/bin/env python3
"""Decode and judge the receipt of the NPU graph application (receipt.h); this module never opens a board."""
import argparse
import json
from pathlib import Path
import struct

MAGIC = 0x5048415247554e50
BYTES = 1728
STATUS = ('match', 'mismatch', 'refused', 'failed', 'timeout', 'not run', 'fault')
HEADER = 16
MARKS = ('captured', 'progress done', 'settle', 'finished', 'reply posted', 'cancel done', 'reinit', 'ready',
         'client waits', 'client woke', 'client read', 'recover failed')


def marks_named(value):
    return [name for bit, name in enumerate(MARKS) if value >> bit & 1]


def require(condition, message):
    if not condition:
        raise ValueError(message)


def decode(raw, names=None, sizes=None, faults=(), queue=False, sequence=False):
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
        if row['status'] == 'fault':
            # The fault record: the address, the MMU status, the canary CRC, the service's marks and the wait window.
            row['fault'] = {'mmu_address': first, 'mmu_status': observed, 'canary_crc': expected,
                            'marks': row['differing_bytes'], 'marks_named': marks_named(row['differing_bytes']),
                            'window_ms': row['max_error_16bit']}
        graphs.append(row)
    result = {'count': count, 'completed': completed, 'matched': matched, 'sdk_closed': closed,
              'elapsed_us': (stop - start) / 24, 'graphs': graphs}
    require(completed <= count and matched <= completed, 'The receipt counts disagree.')
    if sequence:
        result['sequence'] = sequence_of(result, raw)
        result['clock'] = clock_of(words[9:16])
    if queue:
        result['queue'] = queue_rows(result)
        result['clock'] = clock_of(words[9:16])
    result['passed'] = (matched == count - len(faults) and completed == matched and closed == 1 and
                        all(g['status'] == ('fault' if g['index'] in faults else 'match') for g in graphs))
    return result


ANY = {'min': 0, 'max': 4294967295}
# CRC-32 of the canary pattern (apps/os/npu.c): word i is i * 0x9e3779b1 xor 0xc0ffee11, 1 MiB.
CANARY_CRC = 0x4696ca8e


def expectation(sizes, faults=None):
    """The queue expectation as u32 words: every word exact, except the times (start, stop, per-graph ticks).

    faults maps the index of a graph that must fail with an NPU MMU exception to its expected faulting address: that
    record holds status 6, the address, any MMU status, the CRC-32 of the unchanged canary and any marks and window."""
    faults = faults or {}
    good = len(sizes) - len(faults)
    words64 = [MAGIC, 1, BYTES, len(sizes), good, good, 1, None, None] + [0] * 7
    for i, (inputs, outputs) in enumerate(sizes):
        if i in faults:
            words64 += [i, inputs, outputs, 6, faults[i], None, CANARY_CRC, None]
        else:
            words64 += [i, inputs, outputs, 0, 0, 0, 0, None]
    words64 += [0] * (BYTES // 8 - len(words64))
    words = []
    for value in words64:
        words += [ANY, ANY] if value is None else [value & 0xffffffff, value >> 32]
    for i in faults:  # the marks and the window are measured, not predicted
        words[1152 // 4 + i] = ANY
        words[1216 // 4 + i] = ANY
    return {'schema': 'sunstone-board-expect-u32-v1', 'words': words}


def clock_of(r):
    """The CPU clock record that the queue image read before and after the sweep (apps/npu-graphs/main.c clock_words):
    applied points of the little (Cortex-A55) and big clusters in MHz, their rails in mV, the DSU point, the transitions
    made and the core of the submitting task."""
    def one(a, b):
        return {'little_mhz': a & 0xffff, 'big_mhz': a >> 16 & 0xffff, 'core': a >> 32 & 0xff, 'transitions': a >> 40,
                'little_mv': b & 0xffff, 'big_mv': b >> 16 & 0xffff, 'dsu_mhz': b >> 32 & 0xffff}
    return {'before': one(r[0], r[1]), 'after': one(r[3], r[4])}


def sequence_of(result, raw):
    """The sequence study (NPU_GRAPHS_SEQUENCE): graphs per round, rounds, the time of a whole round and which outputs matched.

    Record 0 holds the graphs per round (input_bytes), the rounds (output_bytes), the ticks of all rounds (first_difference),
    the submit refusals (observed), the longest round (expected) and the shortest (ticks). Record 1 holds the bit mask of the
    graphs whose output equals the simulator's (input_bytes), the graph count (output_bytes) and the sum of wrong bytes
    (first_difference). differing_bytes[i] and then max_error[i] hold the wrong bytes of graph i."""
    a, b = result['graphs'][0], result['graphs'][1]
    n, rounds = a['input_bytes'], a['output_bytes']
    wrong = [struct.unpack_from('<I', raw, 1152 + 4 * i)[0] for i in range(min(n, 16))]
    wrong += [struct.unpack_from('<I', raw, 1216 + 4 * i)[0] for i in range(n - 16)] if n > 16 else []
    return {'graphs': n, 'rounds': rounds, 'round_us_mean': a['first_difference'] / 24 / rounds,
            'round_us_min': a['us'], 'round_us_max': a['expected'] / 24, 'refusals': a['observed'],
            'matched_mask': b['input_bytes'], 'all_match': b['input_bytes'] == (1 << n) - 1, 'wrong_bytes': wrong}


def sequence_expectation(n, rounds):
    """The sequence receipt as u32 words: counts and the outputs exact, the times and refusals measured."""
    words64 = [MAGIC, 1, BYTES, 2, 2, 2, 1, None, None] + [None] * 6 + [0]
    words64 += [0, n, rounds, 0, None, None, None, None]
    words64 += [1, (1 << n) - 1, n, 0, 0, 0, 0, None]
    words64 += [0] * (BYTES // 8 - len(words64))
    words = []
    for value in words64:
        words += [ANY, ANY] if value is None else [value & 0xffffffff, value >> 32]
    return {'schema': 'sunstone-board-expect-u32-v1', 'words': words}


def queue_rows(result):
    """The backpressure study (NPU_GRAPHS_QUEUE): record i holds the in-flight limit k in input_bytes, the jobs in
    output_bytes, the counter ticks of all jobs in first_difference and the submit refusals in observed. The rows give
    jobs per second, microseconds per job and refusals for each k; the receipt's differing_bytes is the wrong output
    bytes after the last job."""
    rows = []
    for g in result['graphs']:
        if g['input_bytes'] == 1 and g['output_bytes'] == 1 and g is result['graphs'][-1]:
            # The link row: one link chosen at open, one pair of jobs; the output of the second handle is the reference
            # only if the first handle's output reached its input.
            rows.append({'link': True, 'pair_us': g['first_difference'] / 24, 'wrong_output_bytes': g['differing_bytes'],
                         'status': g['status']})
            continue
        total_us = g['first_difference'] / 24
        rows.append({'k': g['input_bytes'], 'jobs': g['output_bytes'], 'refusals': g['observed'], 'total_us': total_us,
                     'us_per_job': total_us / g['output_bytes'], 'jobs_per_s': g['output_bytes'] / (total_us / 1e6),
                     'wrong_output_bytes': g['differing_bytes'], 'status': g['status']})
    return rows


def queue_expectation(depths, jobs, link=True):
    """The queue-mode receipt as u32 words: the counts and the output check are exact; the times and the refusals,
    which the study measures, are not."""
    n = len(depths) + (1 if link else 0)
    words64 = [MAGIC, 1, BYTES, n, n, n, 1, None, None] + [None] * 6 + [0]   # the clock words are measured
    for i, k in enumerate(depths):
        words64 += [i, k, jobs, 0, None, None, 0, None]
    if link:  # one link, one pair; the pair's time is measured, its output must equal the reference
        words64 += [len(depths), 1, 1, 0, None, 0, 0, None]
    words64 += [0] * (BYTES // 8 - len(words64))
    words = []
    for value in words64:
        words += [ANY, ANY] if value is None else [value & 0xffffffff, value >> 32]
    return {'schema': 'sunstone-board-expect-u32-v1', 'words': words}


def freeze_decoder(destination, names, sizes, faults=(), queue=False, sequence=False):
    """Bind a standalone decode(payload) to the model list and tensor sizes the image was built from."""
    source = Path(__file__).read_text()
    source += '\nNAMES = %r\nSIZES = %r\nFAULTS = %r\nQUEUE = %r\nSEQUENCE = %r\n_bound_decode = decode\n' % (names, sizes, tuple(faults), queue, sequence)
    source += 'def decode(payload):\n    return _bound_decode(payload, NAMES, SIZES, FAULTS, QUEUE, SEQUENCE)\n'
    Path(destination).write_text(source)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('payload', type=Path)
    args = parser.parse_args()
    print(json.dumps(decode(args.payload.read_bytes()), sort_keys=True, indent=1))


if __name__ == '__main__':
    main()


# The chain study (apps/npu-graphs/chain.h, NPU_GRAPHS_CHAIN): its result is not the graph receipt.
CHAIN_MAGIC = 0x5453455248435053
CHAIN_MAX_GRAPHS, CHAIN_MAX_RECORDS, CHAIN_MAX_STAT_RECORDS, CHAIN_MAX_ROWS = 24, 64, 16, 128
CHAIN_HEAD = struct.Struct('<8Q3Q3Q')
CHAIN_BYTES = 30832


def chain_decode(raw):
    """Decode the result of the chain application: times in microseconds, the statistics of every graph against its float
    reference, and the greedy token of every frame of every record."""
    require(len(raw) == CHAIN_BYTES, 'The chain result must hold exactly %d bytes.' % CHAIN_BYTES)
    magic, version, graphs, records, reference_records, rows, start, stop = struct.unpack_from('<8Q', raw, 0)
    require((magic, version) == (CHAIN_MAGIC, 1), 'The chain result header differs.')
    before, after = struct.unpack_from('<3Q', raw, 64), struct.unpack_from('<3Q', raw, 88)
    offset = 112
    series = {}
    for name in ('record', 'wait', 'rescale', 'decode'):
        series[name] = [t / 24 for t in struct.unpack_from('<%dQ' % CHAIN_MAX_RECORDS, raw, offset)[:records]]
        offset += 8 * CHAIN_MAX_RECORDS
    stats = []
    for r in range(reference_records):
        row = []
        for g in range(graphs):
            dot, aa, bb, worst = struct.unpack_from('<4d', raw, offset + 32 * (r * CHAIN_MAX_GRAPHS + g))
            row.append({'cosine': dot / ((aa * bb) ** 0.5) if aa > 0 and bb > 0 else None, 'max_abs': worst,
                        'rms_ref': (bb) ** 0.5})
        stats.append(row)
    offset += 32 * CHAIN_MAX_STAT_RECORDS * CHAIN_MAX_GRAPHS
    tokens = [list(struct.unpack_from('<%dH' % rows, raw, offset + 2 * CHAIN_MAX_ROWS * r)) for r in range(records)]
    return {'graphs': graphs, 'records': records, 'reference_records': reference_records, 'rows': rows,
            'clock': {'before': clock_of([before[0], before[1], 0, before[0], before[1], 0, 0]),
                      'after': clock_of([after[0], after[1], 0, after[0], after[1], 0, 0])},
            'elapsed_us': (stop - start) / 24, 'us': series, 'stats': stats, 'tokens': tokens}


def chain_expectation():
    """Every word of the chain result is measured except the header's magic and version."""
    words = [{'min': 0, 'max': 4294967295}] * (CHAIN_BYTES // 4)
    words[0:4] = [CHAIN_MAGIC & 0xffffffff, CHAIN_MAGIC >> 32, 1, 0]
    return {'schema': 'sunstone-board-expect-u32-v1', 'words': words}


def freeze_chain_decoder(destination):
    source = Path(__file__).read_text()
    source += '\ndef decode(payload):\n    return chain_decode(payload)\n'
    Path(destination).write_text(source)
