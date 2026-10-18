#!/usr/bin/env python3
"""Check only new Bluetooth profile/evidence admission and actual copied result bytes."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path);module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module);return module


def check(output):
    output = Path(output);output.mkdir(exist_ok=True)
    prep = load('bluetooth_host_preparation', ROOT / 'apps/bluetooth/prepare.py')
    observer = load('bluetooth_host_observer', ROOT / 'apps/bluetooth/observe.py')
    frame = (output.parent / 'captured-frame.bin').read_bytes()
    raw_profile = (output.parent / 'captured-profile.bin').read_bytes()
    session = bytearray(592);struct.pack_into('<6Q', session, 0, 0x4153373342535332, 2, 592, 31, 37, 41)
    struct.pack_into('<I', session, 400, 2);struct.pack_into('<Q', session, 312, 7)
    session[320:352] = hashlib.sha256(raw_profile).digest()
    decoded = observer.decode(frame, session, raw_profile)
    assert decoded['state'] == 'identified' and decoded['firmware_generation'] == 4 and decoded['uart_generation'] == 9
    profile = bytearray(raw_profile)
    refs = {};cache = {}
    for key, offset in [('bluetooth_wiring', 56), ('bluetooth_sid', 88), ('bluetooth_transport', 120)]:
        data = json.dumps({'scope': 'software fixture; never physical authority', 'role': key}).encode()
        name = str(output.resolve() / (key + '.json'));cache[name] = data
        refs[key] = {'path': name, 'sha256': hashlib.sha256(data).hexdigest()}
        profile[offset:offset + 32] = hashlib.sha256(data).digest()
    session[320:352] = hashlib.sha256(profile).digest()
    manifest = {'target': 'sunstone-a733-bluetooth', 'execution_profile': 'bluetooth-supervised-v1', 'final_elf': 'fixture'}
    record = {**refs, 'radio_ddr': {'scope': 'parent DDR validation remains separately reused'}, 'payloads': [{}, {}, {}, {}]}
    assert len(prep.bluetooth_admission(record, profile, session, manifest, 'fixture', cache, True)) == 4

    def refused(function):
        try:
            function()
        except (SystemExit, ValueError, KeyError, TypeError):
            return
        raise AssertionError('Invalid Bluetooth input accepted.')

    refused(lambda: prep.bluetooth_admission({**record, 'radio_ddr': None}, profile, session, manifest, 'fixture', cache, True))
    refused(lambda: prep.bluetooth_admission(record, profile, session, {**manifest, 'target': 'sunstone-a733-board'}, 'fixture', cache, True))
    missing = dict(cache);missing.pop(refs['bluetooth_transport']['path'])
    refused(lambda: prep.bluetooth_admission(record, profile, session, manifest, 'fixture', missing, True))
    changed = dict(cache);changed[refs['bluetooth_sid']['path']] = b'changed'
    refused(lambda: prep.bluetooth_admission(record, profile, session, manifest, 'fixture', changed, True))
    refused(lambda: observer.decode(frame, session, raw_profile))
    session[320:352] = hashlib.sha256(raw_profile).digest()
    bad = bytearray(frame);struct.pack_into('<I', bad, 80, 31)
    refused(lambda: observer.decode(bad, session, raw_profile))
    bad = bytearray(frame);bad[64:72] = bad[72:80]
    refused(lambda: observer.decode(bad, session, raw_profile))
    (output / 'result.json').write_text(json.dumps({'passed': True,
        'scope': 'Actual C-produced identity frame; new frozen Bluetooth evidence/manifest/DDR admission checks; no parent protocol suite replay.'}, indent=2) + '\n')
