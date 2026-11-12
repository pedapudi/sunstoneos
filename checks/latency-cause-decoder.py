#!/usr/bin/env python3
"""Reject incomplete cause measurements and preserve raw benchmark identities."""
import importlib.util
import json
from pathlib import Path
import struct
import sys

import harness

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['apps/latency/host.py', 'kernel/scheduler/latency_cause.h',
                'apps/latency/cause-ping.h', 'apps/latency/workload.h']
GATE_RUNS = [['{output}']]


def main():
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    spec = importlib.util.spec_from_file_location('latency_cause_host', ROOT / 'apps/latency/host.py')
    host = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(host)
    cases = []

    def payload():
        w = [host.CAUSE_MAGIC, 1, host.CAUSE_BYTES, 8, 2, 15, 64, 13]
        for core in range(8):
            mhz = 1794 if core < 6 else 2002
            w.extend([0, 0x414fd050 if core < 6 else 0x414fd0b0, core << 8,
                      0x3041, 4, 0, 1 << 31, 0, 1000, 1, 1, 0x40, 255, 500,
                      1000, 1000, 1000 + mhz * 10000, 241000])
        w.extend([0] * (8 * 8 * 4))
        for count in (64, 128):
            for core in range(8):
                sg = 100 if core else 0
                w.extend([count, count * 1000, 1000, count * sg, sg, count if core else 0,
                          count, count * 100, 100, 0, 0, 0, count * 100, 100])
        w.extend([1, 1000, 1388, 1000, 30000, 255, 255, 0])
        for core in range(8):
            begin = 1200 + core * 2000
            scan = 19000 + core * 500
            w.extend([begin, begin + 1000, 100 if core else 0, int(bool(core)),
                      scan, scan + 100, 0, 0, 1, 0, (1 << 64) - 1, 0,
                      begin + 700, begin + 800])
        w.extend([1, 1, 0, 384, 384, 0, 0, 64, 8, 10, 15])
        w.extend([64] * 15)
        for case in range(15):
            w.extend(range(20 + case, 84 + case))
        assert len(w) * 8 == host.CAUSE_BYTES
        return bytearray(struct.pack('<' + 'Q' * len(w), *w))

    def put(data, word, value):
        struct.pack_into('<Q', data, word * 8, value)

    def decode(data):
        return host.decode_latency_cause(data, 24000000)

    original = payload()
    result = decode(original)
    assert result['benchmark']['cases']['sgi_isb_dsb_sy']['raw_cycles'] == list(range(33, 97))
    assert result['tick']['targets'][1]['release_residual_cycles'] == 800
    assert result['tick']['top_clock_consistent']
    assert 'wall_cycle_ratio_mhz' in result['cores'][0] and 'effective_mhz' not in result['cores'][0]
    cases.append('separate raw cases, correlated components and active-clock scope')

    def refused(name, word, value, reason):
        data = payload()
        put(data, word, value)
        harness.refused(lambda: decode(data), reason, ValueError)
        cases.append(name)

    refused('incomplete snapshot', 7, 15, 'header')
    refused('unfrozen writer', 8 + 10, 0, 'configuration')
    refused('divided cycle counter', 8 + 3, 0x3049, 'configuration')
    refused('broad EL0 PMU access', 8 + 4, 5, 'configuration')
    refused('zero clock interval', 8 + 17, 1000, 'configuration')
    tick = 8 + 18 * 8 + 4 * 64 + 14 * 16
    refused('missing release identity', tick + 5, 127, 'complete correlated')
    refused('overlapping SGI and restore accounting', tick + 8 + 2, 999, 'exceed')
    bench = tick + 8 + 14 * 8
    refused('unacknowledged benchmark SGI', bench + 4, 383, 'incomplete')
    refused('incomplete observer control', bench + 7, 63, 'incomplete')
    refused('missing barrier samples', bench + 11, 63, 'incomplete')
    harness.refused(lambda: decode(original[:-8]), 'size', ValueError)
    cases.append('truncated cause report')

    ping = bytearray(host.CAUSE_PING_BYTES)
    struct.pack_into('<8Q', ping, 0, host.CAUSE_PING_MAGIC, 1, len(ping), 256, 24000000,
                     1000, 2000, 1)
    struct.pack_into('<8Q', ping, 64, 0, 0, 1, 256, 1100, 1200, 10, 10)
    struct.pack_into('<256I', ping, 128, *([36] * 256))
    for index, target in enumerate((1, 6)):
        base = 1152 + index * 1120
        struct.pack_into('<12Q', ping, base, 0, target, 0, int(target == 6), 1, 256,
                         1250, 1500, 10, 11, 5, 6)
        struct.pack_into('<256I', ping, base + 96, *([512 + index * 256] * 256))
    decoded = host.decode_latency_cause_ping(ping, 24000000, 1794)
    assert decoded['pairs'][0]['estimated_transfer_median_cycles'] == 256
    assert decoded['pairs'][0]['half_difference_of_medians_cycles'] == 238
    cases.append('one-core roundtrip clock and explicit reference estimate')
    for name, offset, value, reason in [
            ('ping timeout status', 56, 4, 'header'),
            ('incomplete pair', 1152 + 40, 255, 'coverage'),
            ('wrong target identity', 1152 + 8, 6, 'identity'),
            ('reversed IRQ coverage', 1152 + 72, 1, 'coverage')]:
        bad = bytearray(ping)
        struct.pack_into('<Q', bad, offset, value)
        harness.refused(lambda: host.decode_latency_cause_ping(bad, 24000000, 1794), reason, ValueError)
        cases.append(name)

    prefix = bytes(113536)
    body = prefix + original + ping
    body += bytes((-len(body)) % 64)
    footer = (host.CAUSE_FOOTER_MAGIC, 1, len(body) + 64, len(prefix), len(original),
              len(prefix) + len(original), len(ping), 0)
    combined = body + struct.pack('<8Q', *footer)
    assert host.peel_latency_cause(combined) == (prefix, original, ping)
    cases.append('versioned footer preserves the legacy traced report')
    for name, index, value in [('wrong offset', 5, len(prefix)), ('reserved footer', 7, 1),
                                ('wrong result size', 2, 123456)]:
        bad = bytearray(combined)
        struct.pack_into('<Q', bad, len(bad) - 64 + index * 8, value)
        harness.refused(lambda: host.peel_latency_cause(bad), 'footer', ValueError)
        cases.append(name)
    (out / 'cause-synthetic.bin').write_bytes(original)
    (out / 'ping-synthetic.bin').write_bytes(ping)
    (out / 'result.json').write_text(json.dumps({'passed': True, 'cases': cases,
        'scope': 'binary decoding and measurement completeness; no physical timing evidence'}, indent=2) + '\n')
    print(f'PASS latency cause decoder: {len(cases)} cases')


if __name__ == '__main__':
    main()
