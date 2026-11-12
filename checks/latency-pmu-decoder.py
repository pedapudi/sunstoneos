#!/usr/bin/env python3
"""Check PMU event-bank identity, incomplete coverage and clock observations."""
import importlib.util
import json
from pathlib import Path
import struct
import sys

import harness

ROOT = Path(__file__).resolve().parents[1]
GATE_RUNS = [['{output}']]


def main():
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    spec = importlib.util.spec_from_file_location('latency_pmu_host', ROOT / 'apps/latency/host.py')
    host = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(host)
    cases = []
    ceid0 = sum(1 << event for event in (0x03, 0x01, 0x17))
    ceid1 = sum(1 << (event - 32) for event in (0x2a, 0x24, 0x34, 0x35))

    def payload():
        data = bytearray(host.SCHED_PMU_BYTES)
        struct.pack_into('<8Q', data, 0, host.SCHED_PMU_MAGIC, 1, len(data), 8, 5, 2, 6, 6)
        for core in range(8):
            base = 64 + core * 1456
            midr = 0x414fd050 if core < 6 else 0x414fd0b0
            struct.pack_into('<12Q', data, base, 0, midr, 6, ceid0, ceid1, 63, 63, 3, 1, 1, 0, 0)
            bank = core & 1
            cycles = (1794 if core < 6 else 2002) * 10
            struct.pack_into('<17Q', data, base + 96 + bank * 136,
                             1, 0, 0, 1000, 1240, cycles, 8, 9, 10, 11, 12, 13,
                             63, 0, 4 + core, 7, core + 1)
        return data

    def word(data, offset, value):
        struct.pack_into('<Q', data, offset, value)

    def decode(data):
        return host.decode_scheduler_pmu(data, 24000000)

    data = payload()
    (out / 'pmu-synthetic.bin').write_bytes(data)
    result = decode(data)
    rows = [row for core in result['cores'] for row in core['rows'] if row['count']]
    assert len(rows) == 8 and all(row['top_clock_consistent'] for row in rows)
    assert rows[0]['events']['l3_refill'] == 11 and 'stall_backend' not in rows[0]['events']
    assert rows[1]['events']['stall_backend'] == 11 and 'l3_refill' not in rows[1]['events']
    assert all(row['nested_rejection_count'] is None for row in rows)
    assert sum(not row['observed'] for core in result['cores'] for row in core['rows']) == 72
    cases.append('separate banks retain their own events and expose unobserved samples')

    def refused(name, mutate, reason):
        data = payload()
        mutate(data)
        harness.refused(lambda: decode(data), reason, ValueError)
        cases.append(name)

    refused('reserved header flags', lambda d: word(d, 56, 7), 'header')
    refused('inconsistent capability mask', lambda d: word(d, 64 + 40, 31), 'support mask')
    refused('unobserved bank with counters', lambda d: word(d, 64 + 96 + 136 + 40, 1), 'empty bank')
    refused('invalid self identity', lambda d: word(d, 64 + 96 + 15 * 8, 6), 'identity')
    refused('wrong timer core', lambda d: (
        word(d, 64 + 1456 + 96 + 136, 0),
        d.__setitem__(slice(64 + 1456 + 96 + 136, 64 + 1456 + 96 + 272), bytes(136)),
        struct.pack_into('<17Q', d, 64 + 1456 + 96 + 6 * 136,
                         1, 0, 0, 1000, 1240, 17940, 8, 9, 10, 11, 12, 13, 63, 0, 0xffffffff, 6, 2)),
        'timer core')
    refused('completed count mismatch', lambda d: (
        word(d, 64 + 64, 2), word(d, 64 + 72, 2)), 'completed count')
    refused('unsupported event cannot acquire a count', lambda d: (
        word(d, 64 + 32, ceid1 & ~(1 << 10)), word(d, 64 + 40, 55),
        word(d, 64 + 96 + 12 * 8, 55)), 'unsupported event')

    data = payload()
    word(data, 64 + 96 + 2 * 8, 11)
    word(data, 64 + 96 + 13 * 8, 1)
    row = decode(data)['cores'][0]['rows'][0]
    assert row['status'] == 'overflow' and row['events']['l1d_refill'] is None
    assert row['raw_events'][0] == 8 and row['effective_mhz'] is None
    cases.append('overflow remains explicit and invalidates the event interpretation')

    data = payload()
    word(data, 64 + 96 + 5 * 8, 4940)
    row = decode(data)['cores'][0]['rows'][0]
    assert row['effective_mhz'] == 494 and not row['top_clock_consistent']
    cases.append('probe-rate execution cannot pass the top-clock observation')

    data = payload()
    word(data, 64 + 64, 2)
    word(data, 64 + 72, 2)
    struct.pack_into('<17Q', data, 64 + 96 + 8 * 136,
                     1, 0, 0, 1000, 1000, 50, 0, 0, 0, 0, 0, 0,
                     63, 0, 0xffffffff, 6, 2)
    row = decode(data)['cores'][0]['rows'][8]
    assert row['below_one_counter_tick'] and row['effective_mhz'] is None
    cases.append('positive-cycle blank can fall below one architectural counter tick')

    data = payload()
    word(data, 56, 14)
    result = decode(data)
    assert not result['snapshot_complete'] and not result['cores']
    cases.append('incomplete snapshot publishes no qualified rows')
    result = {'passed': True, 'cases': cases, 'scope': 'binary decoder semantics; no physical PMU evidence'}
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(f'latency PMU decoder: {len(cases)} cases passed')


if __name__ == '__main__':
    main()
