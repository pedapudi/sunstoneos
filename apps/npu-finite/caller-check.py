#!/usr/bin/env python3
"""Reproduce and check caller symbolization from a captured finite NPU run.

Only retained host files are read. No device, queue or FEL operation occurs.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', type=Path, required=True, help='Development run directory containing bundle/ and run/')
    parser.add_argument('--evidence', type=Path, required=True)
    args = parser.parse_args()
    args.evidence.mkdir(parents=True, exist_ok=False)
    spec = importlib.util.spec_from_file_location('npu_caller_crumbs', ROOT / 'tools/a733-crumbs.py')
    crumbs = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(crumbs)
    bundle = args.run / 'bundle'
    image = bundle / 'image.elf'
    metadata = json.loads((bundle / 'dev-bundle.json').read_bytes())
    report = json.loads((args.run / 'run/watchdog-report.json').read_bytes())
    words = {name: int(value, 16) for name, value in report['breadcrumbs']['words'].items()}
    sha = hashlib.sha256(image.read_bytes()).hexdigest()
    assert sha == metadata['image_sha256']
    # The shared host originally supplied None for this NPU bundle layout.
    old_image = bundle / 'board/image.elf'
    old = crumbs.decode(words, metadata['epoch'], old_image if old_image.exists() else None)
    direct = crumbs.decode(words, metadata['epoch'], image)
    assert old['board_failure']['caller_source'] is None
    source = direct['board_failure']['caller_source']
    assert source and direct['board_failure']['caller'] == '0x41003880'
    reproduction = {'physical_access': False, 'image_sha256': sha,
                    'old_selected_image': None, 'old_caller_source': None,
                    'direct_caller_source': source, 'recorded_caller': '0x41003880',
                    'symbolized_address': '0x4100387c', 'reason': 'shared host selected board/image.elf; NPU ELF is image.elf'}
    (args.evidence / 'reproduction.json').write_text(json.dumps(reproduction, indent=2) + '\n')
    selected = crumbs.bundle_image(bundle, sha)
    assert selected == image
    decoded = crumbs.decode(words, metadata['epoch'], selected)
    assert decoded['board_failure']['caller_source'] == source
    assert crumbs.bundle_image(bundle, '0' * 64) is None
    assert crumbs.bundle_image(args.evidence / 'missing', sha) is None
    # Preserve CPU precedence. A present mismatching CPU image must not
    # silently select a second image in the bundle root.
    cpu = args.evidence / 'cpu-layout'
    (cpu / 'board').mkdir(parents=True)
    shutil.copy2(image, cpu / 'board/image.elf')
    shutil.copy2(image, cpu / 'image.elf')
    assert crumbs.bundle_image(cpu, sha) == cpu / 'board/image.elf'
    (cpu / 'board/image.elf').write_bytes(b'another image')
    assert crumbs.bundle_image(cpu, sha) is None
    # Exercise the actual shared transport's return decoder, using the
    # existing mocked-FEL fixture. Selection/hash behavior is checked above;
    # this case proves the transport passes that selection to decode().
    transport_spec = importlib.util.spec_from_file_location('npu_caller_transport_check',
                                                           ROOT / 'checks/returning-watchdog.py')
    transport_check = importlib.util.module_from_spec(transport_spec)
    transport_spec.loader.exec_module(transport_check)
    transport_root = args.evidence / 'transport'
    transport_root.mkdir()
    decoded_images = []
    decode = transport_check.transport.crumbs.decode
    def checked_decode(words, epoch, elf=None):
        assert elf == image, 'shared transport omitted the selected NPU ELF'
        decoded_images.append(str(elf))
        return decode(words, epoch, elf)
    with patch.object(transport_check.transport.crumbs, 'bundle_image', return_value=image) as select, \
            patch.object(transport_check.transport.crumbs, 'decode', side_effect=checked_decode):
        state, record, returned, error, transport_evidence = transport_check.transport_case('fired', transport_root)
    assert select.call_count == 1 and decoded_images == [str(image)]
    assert isinstance(error, transport_check.transport.WatchdogReset) and record['watchdog_reset']
    assert state['launched'] and not state['quarantined'] and returned is None
    result = {**reproduction, 'passed': True, 'caller_source': source,
              'cases': ['exact-NPU-image', 'return-address-minus-four', 'wrong-hash-refused',
                        'missing-image', 'CPU-layout-precedence', 'mismatching-CPU-image-no-fallback',
                        'shared-transport-passes-selected-NPU-ELF'],
              'transport_decoded_images': decoded_images}
    (args.evidence / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
