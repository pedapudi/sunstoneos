#!/usr/bin/env python3
"""Build a passive radio scan session from its reviewed producer and reject missing device facts.

The image's native EL3 admission checks the resulting session. The existing
radio-scan-profile check covers the profile producer against native layouts and
firmware initialization. This check covers builder selection, provenance and
upload binding; it performs no device access.
"""
import argparse
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys

import harness

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
GATE_RUNS = [['--radio-elf', '{elf:ret-radio-scan}', '--output', '{output}']]
CHECK_INPUTS = ['tools/session.py', 'tools/session-facts.json', 'tools/radio_scan_prepare.py',
                'docs/designs/radio-vendor-power-sequence.md', 'apps/os/session.c', 'apps/board/admission.c']


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--radio-elf', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    session = load('session_device_profiles', ROOT / 'tools/session.py')
    names = load('session_devices', ROOT / 'tools/sunstone-manifest.py').DEVICES
    passed = []

    def refusal(label, function, *words):
        try:
            function()
        except session.SessionError as error:
            assert all(word in str(error) for word in words), str(error)
            passed.append(label)
            return
        raise AssertionError('accepted ' + label)

    out = args.output / 'radio'
    record = session.build(args.radio_elf, out, clock_grant=True, device_profile='radio-scan')
    raw = (out / 'template/session.bin').read_bytes()
    report = session.inspect(raw, args.radio_elf)
    assert not report['problems'] and report['devices'] == names['radio']
    assert report['prefix']['workload'] == 3 and record['device_profile'] == 'radio-scan'
    assert record['profile'] == 'control' and record['services'] == ['thermal', 'cpu_grant']
    assert 'RADIO' in record['quiesce_actors'] and 'HEADSET' not in record['quiesce_actors']
    verdict = session.admit(args.radio_elf, raw)
    assert verdict['admitted'], verdict
    passed.append('scan session accepted by image EL3 admission')

    scan = session.load('session_scan_producer', ROOT / 'tools/radio_scan_prepare.py')
    uploads = record['uploads']
    assert len(uploads) == 5
    for item in uploads:
        data = (out / item['file']).read_bytes()
        assert len(data) == item['bytes'] and hashlib.sha256(data).hexdigest() == item['sha256']
    profile = (out / uploads[0]['file']).read_bytes()
    assert uploads[0]['address'] == scan.PROFILE_ADDRESS
    assert struct.unpack_from('<QQ', raw, 352) == (uploads[0]['address'], len(profile))
    assert raw[368:400] == hashlib.sha256(profile).digest()
    assert profile[scan.STAGE_LIMIT_OFFSET] == profile[scan.ENTROPY_UNQUALIFIED_OFFSET] == 0
    for index, (_, size, digest) in enumerate(scan.FIRMWARE):
        item = uploads[index + 1]
        assert (item['bytes'], item['sha256']) == (size, digest)
        assert item['address'] == struct.unpack_from('<Q', profile, scan.PROFILE['firmware_address'] + 8 * index)[0]
        assert size == struct.unpack_from('<Q', profile, scan.PROFILE['firmware_bytes'] + 8 * index)[0]
    expected = bytearray(raw[:592])
    expected_profile, expected_uploads = scan.apply(expected, bytes.fromhex(record['device_profile_sources'][session.RADIO_SCAN_SOURCES[1]]), stage_limit=0)
    assert expected == raw[:592] and expected_profile == profile
    assert [(item['address'], (out / item['file']).read_bytes()) for item in uploads] == [(address, data) for address, _, data in expected_uploads]
    passed.append('profile and four firmware uploads match existing runner producer and session digest')

    refusal('required radio has no implicit scan grant', lambda: session.describe(args.radio_elf), 'radio')
    data = session.facts()
    mutations = [('missing profile facts', lambda d: d['device_profiles'].pop('radio-scan'), 'no reviewed facts'),
                 ('wrong board profile', lambda d: d['device_profiles']['radio-scan'].update(board_profile='npu'), 'board profile control'),
                 ('missing source hash', lambda d: d['device_profiles']['radio-scan']['sources'].pop(session.RADIO_SCAN_SOURCES[0]), 'source hashes'),
                 ('changed producer', lambda d: d['device_profiles']['radio-scan']['sources'].update({session.RADIO_SCAN_SOURCES[0]: '00' * 32}), 'source hash differs'),
                 ('changed reference', lambda d: d['device_profiles']['radio-scan']['sources'].update({session.RADIO_SCAN_SOURCES[1]: '00' * 32}), 'source hash differs'),
                 ('missing thermal fact', lambda d: d['profiles']['control']['thermal'].pop('apb1'), 'apb1')]
    for number, (label, mutate, word) in enumerate(mutations):
        variant = copy.deepcopy(data)
        mutate(variant)
        path = args.output / ('facts-%d.json' % number)
        path.write_text(json.dumps(variant))
        refusal(label, lambda path=path: session.build(args.radio_elf, args.output / 'refused', fact_file=path,
                                                      clock_grant=True, device_profile='radio-scan'), word)

    declared = lambda required=0, optional=0: {'devices_required': required, 'devices_optional': optional}
    for profile_name, (device, symbols) in session.DEVICE_PROFILES.items():
        linked = dict.fromkeys(symbols, (0, 0))
        bit = names[device]
        assert session.devices_for(declared(bit), linked, names, profile_name) == (bit, {})
        assert session.devices_for(declared(0, bit), linked, names, profile_name) == (bit, {})
        assert session.devices_for(declared(0, bit), linked, names)[0] == 0
        for symbol in symbols:
            absent = {key: value for key, value in linked.items() if key != symbol}
            refusal(profile_name + ' missing ' + symbol,
                    lambda absent=absent: session.devices_for(declared(bit), absent, names, profile_name), 'service', symbol)
            granted, left_out = session.devices_for(declared(0, bit), absent, names, profile_name)
            assert granted == 0 and device in left_out
        refusal(profile_name + ' undeclared device', lambda: session.devices_for(declared(), linked, names, profile_name), 'declared')
        unsupported = names['usb-storage'] | names['audio-i2s'] | names['audio-headset']
        granted, left_out = session.devices_for(declared(bit, unsupported), linked, names, profile_name)
        assert granted == bit and set(left_out) == {'usb-storage', 'audio-i2s', 'audio-headset'}
        passed.append(profile_name + ' required and optional service selection')
    refusal('USB profile has no fabricated board facts',
            lambda: session.device_profile_facts({'device_profile': 'usb-host'}, data), 'no reviewed facts', 'usb-host')
    refusal('unsupported device profile', lambda: session.devices_for(declared(), {}, names, 'association'), 'unsupported')
    (args.output / 'result.json').write_text(json.dumps({'passed': True, 'physical_access': False, 'cases': passed,
                                                       'image_sha256': record['image_sha256']}, indent=2) + '\n')
    print('PASS %d session device-profile cases; native EL3 admission accepted' % len(passed))


if __name__ == '__main__':
    main()
