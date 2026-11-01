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
    image = Image(args.object, {'latency_result', 'ss_latency_core', 'ss_latency_metric', 'latency_traffic'})
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
            for name, value in [('count', count), ('maximum', 1), ('start', 100), ('site', 0x41000100)]:
                struct.pack_into('<Q', payload, base + metric[name], value)
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
    refused(lambda: decode(payload[:-1], args.dma), '5440 bytes')
    print('PASS native producer layout, complete runner expectation and decoder coverage refusal')


if __name__ == '__main__':
    main()
