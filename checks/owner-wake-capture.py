#!/usr/bin/env python3
"""Check optional owner-counter geometry and post-return decoding without USB."""
import argparse
import importlib.util
import json
from pathlib import Path
import struct
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['apps/os/cpu-return-thermal.py', 'apps/os/cpu-return-host.py']

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--image', type=Path)
    parser.add_argument('--no-window-image', type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    spec = importlib.util.spec_from_file_location('owner_capture', ROOT / 'apps/os/cpu-return-thermal.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    assert module.owner_geometry({}) == {}
    symbols = {symbol: (0x41300000 + i * 128, size)
               for i, (_, symbol, size, _) in enumerate(module.OWNER_RECORDS)}
    layout = {'owner_wakes': module.owner_geometry(symbols)}
    assert len(module.captures(layout)) == 1
    copied = {}
    for name, record in layout['owner_wakes'].items():
        row = (100, 1000100, 40, 0, 40, 0)
        copied['owner-wakes-' + name] = struct.pack('<6Q', *row) * (record['bytes'] // 48)
    owners = module.decode_owners(layout, copied)
    assert owners['thermal']['wakes_per_second'] == 40
    refused = 0
    for address, size in ((0x41100000, 48), (0x41300001, 48), (0x41300000, 40), (0x42000000 - 8, 48)):
        try:
            module.owner_geometry({'os_thermal_wake_record': (address, size)})
        except ValueError:
            refused += 1
    assert refused == 4
    copied['owner-wakes-thermal'] = b''
    try:
        module.decode_owners(layout, copied)
    except ValueError:
        pass
    else:
        raise AssertionError('Short counter accepted')
    native = None
    if args.image:
        native = module.geometry(args.image)
        assert 'thermal' in native['owner_wakes']
    if args.no_window_image:
        spec = importlib.util.spec_from_file_location('owner_return_host', ROOT / 'apps/os/cpu-return-host.py')
        host = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(host)
        original = host.linked_symbols
        def symbols_with_counter(image):
            values = original(image)
            values['os_thermal_wake_record'] = (0x41300000, 48)
            return values
        with patch.object(host, 'linked_symbols', symbols_with_counter), patch.object(
                host.thermal_capture, 'geometry', side_effect=AssertionError('Counter alone created a thermal window')):
            assert 'thermal' not in host.geometry(args.no_window_image)
    (args.output / 'result.json').write_text(json.dumps({
        'passed': True, 'physical_access': False, 'geometry_refusals': refused,
        'decoded_owners': list(owners), 'native_geometry': native}, indent=2) + '\n')
    print('PASS optional owner wake capture: geometry, rates, absent producers and short-data refusal')

if __name__ == '__main__':
    main()
