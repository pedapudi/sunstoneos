#!/usr/bin/env python3
"""Check that evidence records refuse device addresses and one access point's frequency and RSSI."""
import importlib.util
from pathlib import Path
import tempfile

import harness

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('evidence_addresses', ROOT / 'tools/evidence.py')
evidence = importlib.util.module_from_spec(spec)
spec.loader.exec_module(evidence)


def record(**fields):
    base = {'id': 'address-case', 'subsystem': 'radio', 'date': '2026-11-14', 'hypothesis': 'host case',
            'outcome': 'not-run', 'verdict': 'inconclusive', 'measurements': [], 'conditions': {},
            'evidence': [], 'note': '', 'narrative': 'host case'}
    base.update(fields)
    return base


def refused(item):
    return evidence.address_problems(item, 'case')


# A device address is refused in every common spelling and in any field, key or list item.
for text in ('02:11:22:33:44:55', '02-11-22-33-44-55', 'A4:5E:60:C1:22:9F', '0211.2233.4455', 'joined 02:11:22:33:44:55.'):
    assert refused(record(note=text)), text
assert refused(record(conditions={'peer': '02:11:22:33:44:55'}))
assert refused(record(evidence=['~/runs/02:11:22:33:44:55.json']))
assert refused(record(conditions={'bssid': 'redacted'}))
assert refused(record(measurements=[{'name': 'bssid_count', 'value': 1, 'unit': ''}]))

# The protocol constants that share the format name no device and pass.
for constant in evidence.PROTOCOL_ADDRESSES:
    assert not refused(record(note='destination ' + constant)), constant
    assert not refused(record(note='destination ' + constant.upper().replace(':', '-'))), constant

# Hashes, revisions and IPv6 addresses do not have the address format.
assert not refused(record(note='image ' + 'a0' * 32 + ', revision 6bf24ca03, fe80::1, 10.0.0.2'))

# One channel frequency with one RSSI identifies an access point; a predicted range does not.
assert refused(record(conclusion='The matched access point answered on 2437 MHz at -48 dBm.'))
assert refused(record(note='strongest: -51 dBm, 5180 MHz'))
assert not refused(record(prediction='Results on 2412 to 2462 MHz with RSSI between -95 and -20 dBm.'))
assert not refused(record(note='The scan listed 8 of 12 reported results, all on 2.4 GHz.'))

# The same rule over measurements and conditions.
rssi = {'name': 'rssi_matched', 'value': -48, 'unit': 'dBm'}
assert refused(record(measurements=[rssi, {'name': 'frequency', 'value': 2437, 'unit': 'MHz'}]))
assert refused(record(measurements=[rssi], conditions={'channel': '2437 MHz'}))
assert not refused(record(measurements=[rssi]))
assert not refused(record(measurements=[rssi], conditions={'cpu_l': '1794 MHz', 'dram': '2040 MHz'}))

# The writer refuses before it writes, and validation reports a committed record with an address.
with tempfile.TemporaryDirectory(prefix='sunstone-evidence-addresses-') as temporary:
    root = Path(temporary)
    path = evidence.append_record(root, record())
    before = path.read_text()
    try:
        evidence.append_record(root, record(id='address-case-2', note='peer 02:11:22:33:44:55'))
    except SystemExit as error:
        assert 'device address' in str(error)
    else:
        raise AssertionError('The writer accepted a device address.')
    assert path.read_text() == before
    path.write_text(before + '{"id": "address-case-3", "subsystem": "radio", "note": "peer 02:11:22:33:44:55"}\n')
    assert any('device address' in problem for problem in evidence.validate_records(root))

# The committed records hold no address and no identifying pair.
committed = [problem for problem in evidence.validate_records(ROOT)
             if 'address' in problem or 'BSSID' in problem or 'RSSI' in problem]
assert not committed, committed

print('PASS: evidence records refuse device addresses and one access point\'s frequency and RSSI.')
