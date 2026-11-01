#!/usr/bin/env python3
"""Check the latency decoder and runner expectation against native producer DWARF."""
import argparse
from pathlib import Path
import runpy
import struct
from elftools.elf.elffile import ELFFile
from harness import Image, refused

ROOT = Path(__file__).resolve().parents[1]
LAYOUTS = {'latency_result', 'ss_latency_core', 'ss_latency_metric', 'latency_traffic',
           'ss_latency_phase_core', 'ss_latency_distribution', 'latency_wake_sample',
           'ss_latency_site'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--object', type=Path, required=True)
    parser.add_argument('--dma', action='store_true')
    args = parser.parse_args()
    image = Image(args.object, LAYOUTS)
    with args.object.open('rb') as stream:
        dwarf = ELFFile(stream).get_dwarf_info()
        sizes = {}
        for cu in dwarf.iter_CUs():
            for die in cu.iter_DIEs():
                name = die.attributes.get('DW_AT_name')
                if die.tag == 'DW_TAG_structure_type' and name and 'DW_AT_byte_size' in die.attributes:
                    sizes[name.value.decode()] = die.attributes['DW_AT_byte_size'].value
    layouts = image.layouts
    record = layouts['latency_result']
    payload = bytearray(sizes['latency_result'])
    decoder = runpy.run_path(str(ROOT / 'apps/latency/host.py'))
    decode, upper = decoder['decode'], decoder['histogram_upper']

    def put(base, typename, **fields):
        for name, value in fields.items():
            struct.pack_into('<Q', payload, base + layouts[typename][name], value)

    def metric_base(core, metric):
        return record['core'] + core * sizes['ss_latency_core'] + metric * sizes['ss_latency_metric']

    def distribution_base(phase, core, metric):
        return (record['phases'] + (phase * 8 + core) * sizes['ss_latency_phase_core'] +
                metric * sizes['ss_latency_distribution'])

    def wake_base(core, index):
        return record['wakes'] + (core * 192 + index) * sizes['latency_wake_sample']

    def site_base(phase, core, kind, slot):
        return record['sites'] + (((phase * 8 + core) * 2 + kind) * 8 + slot) * sizes['ss_latency_site']

    put(0, 'latency_result', magic=0x53534c4154454e31, frequency=24000000, elapsed_ticks=200)
    struct.pack_into('<I', payload, record['core_mask'], 255)
    struct.pack_into('<I', payload, record['timer_irq_core_mask'], 1)
    if args.dma:
        struct.pack_into('<I', payload, record['dma_jobs'], 6)
    bounds = ((0, 1000), (1100, 5000), (5100, 12000))
    for p, pair in enumerate(bounds):
        struct.pack_into('<2Q', payload, record['phase_bounds'] + p * 16, *pair)
    for c in range(8):
        put(record['traffic'] + c * sizes['latency_traffic'], 'latency_traffic',
            sweeps=2, bytes_read=2097152, bytes_written=2097152)
        site = 0x141000100 + c * 4
        struct.pack_into('<Q', payload, record['wake_site'] + c * 8, site)
        for lock in range(2):
            struct.pack_into('<Q', payload, record['acquisitions'] + (c * 2 + lock) * 8, 32)
            for p in range(3):
                struct.pack_into('<Q', payload,
                    record['phase_acquisitions'] + ((p * 8 + c) * 2 + lock) * 8, 8)
        for m in range(9 if args.dma and c == 0 else 8):
            if m == 0 and c:
                continue
            if m in (6, 7):
                p, first, count = (1, 0, 64) if m == 6 else (2, 64, 128)
                durations = [1 + i % 17 for i in range(count)]
                start = bounds[p][0] + 100 + durations.index(max(durations)) * 20
                for i, duration in enumerate(durations):
                    ready = bounds[p][0] + 100 + i * 20
                    base = wake_base(c, first + i)
                    put(base, 'latency_wake_sample', ready=ready, running=ready + duration)
                    struct.pack_into('<I', payload, base + layouts['latency_wake_sample']['metric'], m)
                    struct.pack_into('<I', payload, base + layouts['latency_wake_sample']['workload_flags'], p)
                ordered = sorted(durations)
                put(distribution_base(p, c, m), 'ss_latency_distribution', count=count,
                    p50_upper=upper(ordered[(count + 1) // 2 - 1]),
                    p99_upper=upper(ordered[count - count // 100 - 1]),
                    maximum=max(durations), start=start, site=site)
                put(metric_base(c, m), 'ss_latency_metric', count=count,
                    maximum=max(durations), start=start, site=site)
            else:
                count = 8 if m in (1, 5) else 2 if m == 8 else 1
                maximum = 20 if m == 1 else 16 if m == 5 else 4
                put(metric_base(c, m), 'ss_latency_metric', count=count * 3 + (m in (1, 5)),
                    maximum=maximum, start=995 if m == 1 else 1050 if m == 5 else 100, site=site)
                for p, (begin, _) in enumerate(bounds):
                    phase_max = 8 if m == 1 else 4
                    put(distribution_base(p, c, m), 'ss_latency_distribution', count=count,
                        p50_upper=upper(phase_max), p99_upper=upper(phase_max),
                        maximum=phase_max, start=begin + 100, site=site)
        for p in range(4):
            for kind in range(2):
                durations = list(range(8 if kind == 0 else 4, 0, -1))
                if p == 3:
                    durations = [20 if kind == 0 else 16] + durations[:7]
                for slot, duration in enumerate(durations):
                    start = bounds[p][0] + 100 + slot * 20 if p < 3 else 995 if kind == 0 else 1050
                    put(site_base(p, c, kind, slot), 'ss_latency_site', ticks=duration,
                        start=start, enter_site=site, exit_site=site + 0x100)
    expected = runpy.run_path(str(ROOT / 'apps/latency/expect.py'))['expectation'](args.dma)['words']
    decoded = decode(payload, args.dma)
    assert len(decoded['cores']) == 8
    assert decoded['cores'][7]['sites']['busy']['irq_masked'][7] == {
        'ticks': 1, 'start': 5340, 'enter_site': 0x14100011c, 'exit_site': 0x14100021c}
    assert len(decoded['cores'][0]['sites']['idle']['task_lock_hold']) == 4
    assert decoded['cores'][0]['sites']['aggregate']['irq_masked'][0]['start'] == 995
    # Aggregate intervals retain timestamps outside the named phase windows.
    wide = bytearray(payload)
    struct.pack_into('<Q', wide, site_base(3, 0, 0, 0) + layouts['ss_latency_site']['start'], 1 << 40)
    assert decode(wide, args.dma)['cores'][0]['sites']['aggregate']['irq_masked'][0]['start'] == 1 << 40
    words = struct.unpack('<' + 'I' * (len(payload) // 4), payload)
    assert len(words) == len(expected)
    for index, (actual, rule) in enumerate(zip(words, expected)):
        assert ((rule['min'] <= actual <= rule['max']) if isinstance(rule, dict) else actual == rule), (index, actual, rule)

    def corrupt(offset, value, reason, fmt='<Q'):
        broken = bytearray(payload)
        struct.pack_into(fmt, broken, offset, value)
        refused(lambda: decode(broken, args.dma), reason)

    corrupt(metric_base(0, 9), 1, 'cannot report accelerator or DMA samples')
    corrupt(record['phase_bounds'] + 16, 999, 'phase bounds overlap')
    corrupt(distribution_base(1, 0, 1) + layouts['ss_latency_distribution']['count'], 100,
            'phase count exceeds aggregate')
    corrupt(distribution_base(1, 0, 1) + layouts['ss_latency_distribution']['start'], 4999,
            'phase maximum crosses a phase boundary')
    corrupt(distribution_base(1, 0, 6) + layouts['ss_latency_distribution']['p50_upper'], 1,
            'raw wake quantiles differ')
    corrupt(wake_base(0, 0) + layouts['latency_wake_sample']['workload_flags'], 2,
            'raw wake workload flags or site differ', '<I')
    corrupt(wake_base(0, 0) + layouts['latency_wake_sample']['running'], 5001,
            'raw wake crosses a phase boundary')
    corrupt(metric_base(0, 6) + layouts['ss_latency_metric']['site'], 1,
            'raw wake aggregate maximum provenance differs')
    corrupt(distribution_base(1, 0, 6) + layouts['ss_latency_distribution']['site'], 1,
            'raw wake maximum provenance differs')
    site = site_base(1, 0, 0, 0)
    corrupt(site + layouts['ss_latency_site']['ticks'], 21, 'attribution duration exceeds aggregate')
    corrupt(site + layouts['ss_latency_site']['enter_site'], 0, 'lacks enter or exit site')
    corrupt(site + layouts['ss_latency_site']['exit_site'], 0, 'lacks enter or exit site')
    corrupt(site + layouts['ss_latency_site']['start'], 4999, 'attribution interval crosses a phase boundary')
    corrupt(site_base(1, 0, 0, 1), 9, 'attribution slots are not packed in descending order')
    corrupt(site_base(1, 0, 1, 4) + layouts['ss_latency_site']['start'], 1, 'inactive attribution slot has context')
    broken = bytearray(payload)
    start = site_base(1, 0, 0, 0)
    broken[start:start + sizes['ss_latency_site']] = bytes(sizes['ss_latency_site'])
    refused(lambda: decode(broken, args.dma), 'attribution slots are not packed in descending order')
    if args.dma:
        corrupt(metric_base(0, 8), 1, 'DMA completion coverage is incomplete')
        refused(lambda: decode(payload), 'header differs')
    else:
        refused(lambda: decode(payload, True), 'header differs')
    refused(lambda: decode(payload[:-1], args.dma), f'{len(payload)} bytes')
    print('PASS native DWARF layout, complete runner expectation, phase quantiles, raw wakes and attribution refusals')


if __name__ == '__main__':
    main()
