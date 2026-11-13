#!/usr/bin/env python3
"""Check legacy GPU retirement proofs and optional owner wake telemetry."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
from elftools.elf.elffile import ELFFile

ROOT = Path(__file__).resolve().parents[1]
GATE_RUNS = [['--image', '{elf:ret-gpu}', '--output', '{output}']]
CHECK_INPUTS = ['apps/os/gpu-return.py', 'apps/os/gpu.c', 'include/pvr_gpu_service.h']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    spec = importlib.util.spec_from_file_location('gpu_receipt', ROOT/'apps/os/gpu-return.py')
    decoder = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(decoder)
    rows = []

    def rejected(name, call):
        try:
            call()
        except ValueError:
            rows.append({'case': name, 'rejected': True})
        else:
            raise AssertionError(('accepted invalid receipt', name))

    proof = [decoder.MAGIC, 1, 64, 123, 456, 7, 63, 960000]
    tail = struct.pack('<4Q', 19, 7, 3, 1000000)
    for version in (1, 2, 3, 4):
        words = proof.copy()
        words[1] = version
        if version == 2:
            words[5:] = [0, 0, 0]
        elif version == 3:
            words[7] = 3 << 32 | 7
        elif version == 4:
            words[7] = 960000 << 32 | 2
        for size in (64, 96):
            layout = {'bytes': size, 'allow_untouched': True, 'allow_failure': True}
            if version == 3:
                layout['bisect'] = {'stage': 3, 'limit': 7}
            raw = struct.pack('<8Q', *words) + (tail if size == 96 else b'')
            result = decoder.decode_captured(raw, layout, 123, 456, failure_return=version == 4)
            assert ('owner_waits' in result) == (size == 96)
            if size == 96:
                assert result['owner_waits']['idle_notification_wakes_per_second'] == 3
            rows.append({'case': f'version-{version}-{size}', 'accepted': True})
            for index in (0, 2, 3, 4, 6):
                changed = bytearray(raw)
                struct.pack_into('<Q', changed, index*8, words[index] ^ 1)
                rejected(f'proof-{version}-{size}-{index}', lambda: decoder.decode_captured(
                    changed, layout, 123, 456, failure_return=version == 4))
    raw = struct.pack('<8Q', *proof)
    layout = {'bytes': 96, 'allow_untouched': False}
    for bad in (raw[:-1], raw, raw + tail[:-1], raw + tail + b'\0'):
        rejected(f'length-{len(bad)}', lambda: decoder.decode_captured(bad, layout, 123, 456))
    for fields in ((2, 7, 3, 1000000), (19, 7, 3, 0xffffffffffffffff)):
        rejected('telemetry-' + str(fields), lambda: decoder.decode_captured(
            raw + struct.pack('<4Q', *fields), layout, 123, 456))
    immediate = decoder.decode_captured(raw + struct.pack('<4Q', 1, 0, 1, 0), layout, 123, 456)
    assert immediate['owner_waits']['idle_notification_wakes_per_second'] is None

    geometry = decoder.geometry(args.image)
    assert geometry['bytes'] == 96
    with args.image.open('rb') as stream:
        elf = ELFFile(stream)
        symbols = elf.get_section_by_name('.symtab')
        entry = next(index for index, symbol in enumerate(symbols.iter_symbols())
                     if symbol.name == 'os_gpu_return_report')
        size_offset = symbols['sh_offset'] + entry * symbols['sh_entsize'] + 16
    for size in (64, 72, 104):
        changed = bytearray(args.image.read_bytes())
        struct.pack_into('<Q', changed, size_offset, size)
        path = args.output/f'geometry-{size}.elf'
        path.write_bytes(changed)
        if size == 64:
            assert decoder.geometry(path) == dict(geometry, bytes=64)
            rows.append({'case': 'legacy-geometry', 'accepted': True})
        else:
            rejected(f'geometry-{size}', lambda: decoder.geometry(path))
    (args.output/'results.json').write_text(json.dumps({
        'physical_access': False, 'image_sha256': hashlib.sha256(args.image.read_bytes()).hexdigest(),
        'cases': rows}, indent=2) + '\n')
    print('PASS GPU receipt:', len(rows), 'proof, telemetry and geometry cases')


if __name__ == '__main__':
    main()
