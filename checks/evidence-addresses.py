#!/usr/bin/env python3
"""Check that evidence records refuse device addresses, one access point's frequency and RSSI and, in radio records,
IPv4 addresses, and that a record added from a queue entry names each measurement and evidence path once."""
import argparse
import importlib.util
import json
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
assert not refused(record(note='image ' + 'a0' * 32 + ', revision 6bf24ca03, fe80::1'))

# A radio record holds no IPv4 address in any field: a leased address, gateway or DNS server locates the network.
for text in ('10.0.0.2', 'leased 192.168.1.20.', 'gateway 172.16.0.1, DNS 8.8.8.8', '0.0.0.0', 'via 010.000.000.002',
             'peer=10.0.0.2;'):
    assert refused(record(note=text)), text
assert refused(record(conditions={'gateway': '192.168.1.1'}))
assert refused(record(measurements=[{'name': 'lease', 'value': 3600, 'unit': 's'}], conclusion='address 10.1.2.3'))
assert refused(record(evidence=['~/runs/10.0.0.2.json']))
# Three-part versions, five-part runs, octets above 255 and other subsystems pass.
for text in ('wpa_supplicant 2.12.0', 'FreeRTOS 11.1.0', 'section 4.1', 'OID 1.3.6.1.4', '256.1.1.1', '1.2.3.400',
             'v10.0.2', 'RFC 1918 private range'):
    assert not refused(record(note=text)), text
assert not refused(record(subsystem='usb', note='host 10.0.0.2'))

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
    try:
        evidence.append_record(root, record(id='address-case-4', note='leased 192.168.1.20'))
    except SystemExit as error:
        assert 'IPv4 address' in str(error)
    else:
        raise AssertionError('The writer accepted an IPv4 address in a radio record.')
    path.write_text(before + '{"id": "address-case-5", "subsystem": "radio", "note": "gateway 192.168.1.1"}\n')
    assert any('IPv4 address' in problem for problem in evidence.validate_records(root))

# add fills run_total from result.json: a --measure of the same name is refused, and the run directory given again with
# --evidence is listed once (nine stage 7 records of 2026-11-15 carried both twice).
with tempfile.TemporaryDirectory(prefix='sunstone-evidence-add-') as temporary:
    root, queue, entry = Path(temporary) / 'root', Path(temporary) / 'queue', '1794741550815053099-4bf18ca8'
    run = queue / 'entries' / entry / 'run'
    run.mkdir(parents=True)
    (queue / 'entries' / entry / 'entry.json').write_text(json.dumps(
        {'kind': 'cpu-return', 'port': '3-1', 'hypothesis': 'host case'}))
    (run / 'result.json').write_text(json.dumps(
        {'time': 1794741550.8, 'port': '3-1', 'outcome': 'returned', 'normal_return': True, 'image_sha256': 'a0' * 32,
         'git_revision': 'b1f0165f7aca8c4002b790356a0042c1db5e3392', 'durations': {'total': 24.249}}))
    def add(*measures, evidence_paths=()):
        evidence.cmd_add(argparse.Namespace(
            root=str(root), queue=str(queue), entry_id=entry, subsystem='scheduler', hypothesis=None, verdict=None,
            note=None, prediction=None, conclusion=None, design=None, date=None, measure=list(measures), condition=[],
            evidence=list(evidence_paths), replace=True))
        return json.loads((root / 'docs/evidence/scheduler.jsonl').read_text().splitlines()[-1])
    try:
        add('run_total=24.249s', 'cpu1_worst_us=15.2us')
    except SystemExit as error:
        assert 'run_total' in str(error), error
    else:
        raise AssertionError('add accepted a second run_total.')
    recorded = str(run).replace(str(Path.home()), '~')
    added = add('cpu1_worst_us=15.2us', evidence_paths=[recorded, recorded])
    assert [m['name'] for m in added['measurements']] == ['run_total', 'cpu1_worst_us'], added['measurements']
    assert added['evidence'] == [recorded], added['evidence']

# The committed records hold no address and no identifying pair.
committed = [problem for problem in evidence.validate_records(ROOT)
             if 'address' in problem or 'BSSID' in problem or 'RSSI' in problem]
assert not committed, committed

print('PASS: evidence records refuse device addresses and one access point\'s frequency and RSSI; add names each '
      'measurement and evidence path once.')
