#!/usr/bin/env python3
"""Check the latency decoder and runner expectation against native producer DWARF."""
import argparse
from pathlib import Path
import runpy
import struct
from elftools.elf.elffile import ELFFile
from harness import Image, refused

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--object', type=Path, required=True)
    parser.add_argument('--dma', action='store_true')
    args = parser.parse_args()
    image = Image(args.object, {'latency_result', 'ss_latency_core', 'ss_latency_metric', 'latency_traffic', 'ss_latency_distribution', 'ss_latency_phase_core', 'latency_wake_sample'})
    with args.object.open('rb') as stream:
        dwarf = ELFFile(stream).get_dwarf_info()
        sizes = {}
        for cu in dwarf.iter_CUs():
            for die in cu.iter_DIEs():
                name = die.attributes.get('DW_AT_name')
                if die.tag == 'DW_TAG_structure_type' and name and 'DW_AT_byte_size' in die.attributes:
                    sizes[name.value.decode()] = die.attributes['DW_AT_byte_size'].value
    layouts = image.layouts
    record, metric = layouts['latency_result'], layouts['ss_latency_metric']
    payload = bytearray(sizes['latency_result'])
    for name, value in [('magic', 0x53534c4154454e31), ('frequency', 24000000), ('elapsed_ticks', 200)]:
        struct.pack_into('<Q', payload, record[name], value)
    struct.pack_into('<I', payload, record['core_mask'], 255)
    struct.pack_into('<I', payload, record['timer_irq_core_mask'], 1)
    if args.dma:
        struct.pack_into('<I', payload, record['dma_jobs'], 2)
    for c in range(8):
        traffic = record['traffic'] + c * sizes['latency_traffic']
        for name, value in [('sweeps', 2), ('bytes_read', 2097152), ('bytes_written', 2097152)]:
            struct.pack_into('<Q', payload, traffic + layouts['latency_traffic'][name], value)
        for r in range(9 if args.dma and c == 0 else 8):
            if r == 0 and c:
                continue
            base = record['core'] + c * sizes['ss_latency_core'] + r * sizes['ss_latency_metric']
            count = 64 if r == 6 else 128 if r == 7 else 2 if r == 8 else 1
            for name, value in [('count', count), ('maximum', 1), ('start', 300 if r == 7 else 100), ('site', 0x41000100)]:
                struct.pack_into('<Q', payload, base + metric[name], value)
    for phase in range(3):
        for c in range(8):
            for r in range(9 if args.dma and c == 0 else 8):
                if r == 0 and c:
                    continue
                if r in (6, 7):
                    count = 64 if phase == 1 and r == 6 else 128 if phase == 2 and r == 7 else 0
                else:
                    count = (2 if r == 8 else 1) if phase == 0 else 0
                if not count:
                    continue
                base = record['phases'] + (phase * 8 + c) * sizes['ss_latency_phase_core'] + r * sizes['ss_latency_distribution']
                for name, value in [('count', count), ('p50_upper', 1), ('p99_upper', 1),
                                    ('maximum', 1), ('start', 300 if phase == 2 else 100 if phase == 1 else 10), ('site', 0x41000100)]:
                    struct.pack_into('<Q', payload, base + layouts['ss_latency_distribution'][name], value)
    for c in range(8):
        struct.pack_into('<2Q', payload, record['acquisitions'] + c * 16, 2, 2)
        struct.pack_into('<2Q', payload, record['phase_acquisitions'] + c * 16, 2, 2)
    for p, (begin, end) in enumerate(((0, 50), (90, 200), (200, 400))):
        struct.pack_into('<2Q', payload, record['phase_bounds'] + p * 16, begin, end)
    for c in range(8):
        struct.pack_into('<Q', payload, record['wake_site'] + c * 8, 0x41000100)
        for index in range(192):
            ready = 100 if index < 64 else 300
            struct.pack_into('<2Q2I', payload, record['wakes'] + (c * 192 + index) * sizes['latency_wake_sample'],
                             ready, ready + 1, 6 if index < 64 else 7, 1 if index < 64 else 2)
    decode = runpy.run_path(str(ROOT / 'apps/latency/host.py'))['decode']
    expected = runpy.run_path(str(ROOT / 'apps/latency/expect.py'))['expectation'](args.dma)['words']
    assert len(decode(payload, args.dma)['cores']) == 8
    words = struct.unpack('<' + 'I' * (len(payload) // 4), payload)
    assert len(words) == len(expected)
    for actual, rule in zip(words, expected):
        assert (rule['min'] <= actual <= rule['max']) if isinstance(rule, dict) else actual == rule
    broken = bytearray(payload)
    struct.pack_into('<Q', broken, record['core'] + 9 * sizes['ss_latency_metric'], 1)
    refused(lambda: decode(broken, args.dma), 'cannot report accelerator or DMA samples')
    if args.dma:
        broken = bytearray(payload)
        struct.pack_into('<Q', broken, record['core'] + 8 * sizes['ss_latency_metric'], 1)
        refused(lambda: decode(broken, True), 'DMA completion coverage is incomplete')
        refused(lambda: decode(payload), 'header differs')
    else:
        refused(lambda: decode(payload, True), 'header differs')
    refused(lambda: decode(payload[:-1], args.dma), '57728 bytes')
    broken = bytearray(payload)
    phase_count = record['phases'] + sizes['ss_latency_phase_core'] * 8 + 6 * sizes['ss_latency_distribution']
    struct.pack_into('<Q', broken, phase_count, 63)
    refused(lambda: decode(broken, args.dma), 'phase wake samples are incomplete')
    broken = bytearray(payload)
    struct.pack_into('<Q', broken, record['acquisitions'], 0)
    refused(lambda: decode(broken, args.dma), 'acquisition counts differ')
    for offset, value, message in ((record['wakes'] + 20, 2, 'workload flags'),
                                   (record['wakes'] + 8, 201, 'crosses a phase'),
                                   (phase_count + 8, 0, 'raw wake quantiles')):
        broken = bytearray(payload)
        struct.pack_into('<Q' if offset != record['wakes'] + 20 else '<I', broken, offset, value)
        refused(lambda: decode(broken, args.dma), message)
    print('PASS native producer layout, complete runner expectation and decoder coverage refusal')


if __name__ == '__main__':
    main()
