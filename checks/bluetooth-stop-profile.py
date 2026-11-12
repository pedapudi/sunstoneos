#!/usr/bin/env python3
"""Check Bluetooth reset authority admission and profile layout compatibility."""
import argparse
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]
GATE_RUNS = [['--output', '{output}', '--firmware', '{cache}/sources/radio/firmware/aic8800d80']]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def require(ok, message):
    if not ok:
        raise ValueError(message)


def check(output, firmware):
    output.mkdir(parents=True, exist_ok=True)
    prep = load('stop_profile_prepare', ROOT / 'apps/bluetooth/prepare.py')
    observer = load('stop_profile_observe', ROOT / 'apps/bluetooth/observe.py')
    assets = load('stop_profile_assets', ROOT / 'apps/os/radio-assets.py')
    cache, evidence = {}, {}
    passed = []

    def frozen(name, data):
        path = str(output.resolve() / name)
        cache[path] = data
        return {'path': path, 'sha256': hashlib.sha256(data).hexdigest()}

    def document(name, value):
        return frozen(name, json.dumps(value, sort_keys=True).encode())

    for key in ('wiring', 'sid', 'transport', 'scheduling', 'stop'):
        evidence[key] = document(key + '.json', {
            'scope': 'Host parser fixture only; no physical reset authority', 'role': key})
    models = {'models': [{'bytes': size, 'sha256': hashlib.sha256(bytes([i])).hexdigest()}
                          for i, size in enumerate((64, 128))]}
    profile = bytearray(888)
    struct.pack_into('<QII5Q', profile, 0, 0x3150465442535353, 4, 888, 91, 7, 13, 17, 19)
    for key, offset in [('wiring', 56), ('sid', 88), ('transport', 120), ('scheduling', 672), ('stop', 848)]:
        profile[offset:offset + 32] = bytes.fromhex(evidence[key]['sha256'])
    struct.pack_into('<2Q2I', profile, 152, 67, 71, 63, 4)
    struct.pack_into('<5Q4I6I', profile, 176, 73, 0, 0, 0, 91, 0, 2, 3, 2, 1, 2, 3, 400000000, 200000000, 24000000)
    struct.pack_into('<Q', profile, 328, 19)
    struct.pack_into('<Q', profile, 352, 79)
    struct.pack_into('<8I', profile, 376, 3, 1, 8, 2, 0, 1, 24000000, 1500000)
    struct.pack_into('<2I4B', profile, 496, 3 << 16, 0, 3, 0, 0, 1)
    payloads = []
    for i, (name, address) in enumerate(zip(
            ('fw_adid_8800d80_u02.bin', 'fw_patch_8800d80_u02.bin',
             'fw_patch_table_8800d80_u02.bin', 'fmacfw_8800d80_u02.bin'),
            (0x43010000, 0x43011000, 0x43018000, 0x43020000))):
        path = (firmware / name).resolve()
        data = path.read_bytes()
        cache[str(path)] = data
        payloads.append({'path': str(path), 'sha256': hashlib.sha256(data).hexdigest(), 'address': address})
        struct.pack_into('<Q', profile, 512 + i * 8, address)
        struct.pack_into('<Q', profile, 544 + i * 8, len(data))
    struct.pack_into('<6Q', profile, 576, 83, 10000000, 1000000, 500000, 100000, 1000000)
    struct.pack_into('<2I6s', profile, 624, 2, 0, b'peer01')
    struct.pack_into('<2IQ', profile, 656, 2, 1, 97)
    struct.pack_into('<2QI4H2IBBH', profile, 704, 1000, 500000, 120, 128, 160, 18, 17, 100000, 2000000, 0, 0, 0)
    struct.pack_into('<2Q', profile, 752, *(model['bytes'] for model in models['models']))
    profile[768:832] = b''.join(bytes.fromhex(model['sha256']) for model in models['models'])
    struct.pack_into('<2Q', profile, 832, 101, 91)
    struct.pack_into('<4BI', profile, 880, 1, 0, 1, 1, 0)

    session = bytearray(592)
    struct.pack_into('<6Q', session, 0, 0x4153373342535332, 2, 592, 31, 37, 41)
    for offset in (80, 216, 248, 280):
        session[offset:offset + 32] = bytes([1]) * 32
    struct.pack_into('<4Q', session, 184, 91, 2, 3, 4)
    struct.pack_into('<Q', session, 312, 7)
    struct.pack_into('<I', session, 400, 2)
    struct.pack_into('<2Q', session, 456, 103, 107)
    session[488:512] = profile[232:256]
    struct.pack_into('<2I', session, 512, 3300000, 3300000)
    struct.pack_into('<I', session, 532, 1)

    def bound(data, raw=session):
        result = bytearray(raw)
        result[320:352] = hashlib.sha256(data).digest()
        return result

    def profile_valid(data):
        return prep.profile_check(data, bound(data), model_pin=models)

    def refused(name, operation):
        try:
            operation()
        except (SystemExit, ValueError, KeyError, TypeError, FileNotFoundError):
            passed.append(name)
            return
        raise AssertionError(name + ': invalid input accepted')

    for size, version in ((624, 1), (752, 2), (832, 3), (888, 4)):
        compatible = bytearray(profile[:size])
        struct.pack_into('<II', compatible, 8, version, size)
        if version == 2:
            struct.pack_into('<I', compatible, 624, 1)
        assert len(profile_valid(compatible)) == 4
        passed.append('profile-version-' + str(version))

    for name, offset, format_, value in (
            ('missing-reset-authority', 832, '<Q', 0),
            ('other-reset-topology', 840, '<Q', 92),
            ('non-gpio-reset', 880, '<B', 2),
            ('invalid-reset-pull', 881, '<B', 3),
            ('invalid-reset-drive', 882, '<B', 4),
            ('invalid-inherited-level', 883, '<B', 2),
            ('reserved-reset-field', 884, '<I', 1),
            ('unknown-inherited-pm2', 168, '<I', 0),
            ('partly-known-pm-latch', 168, '<I', 4),
            ('inherited-level-mismatch', 172, '<I', 0)):
        invalid = bytearray(profile)
        struct.pack_into(format_, invalid, offset, value)
        refused(name, lambda invalid=invalid: profile_valid(invalid))
    invalid = bytearray(profile)
    invalid[848:880] = bytes(32)
    refused('missing-reset-wiring', lambda: profile_valid(invalid))
    invalid = bytearray(profile)
    struct.pack_into('<2Q', invalid, 16, 92, 7)
    struct.pack_into('<Q', invalid, 840, 92)
    refused('other-session-topology', lambda: profile_valid(invalid))
    refused('profile-tail-not-authenticated', lambda: prep.profile_check(profile, bound(profile[:832]), model_pin=models))
    refused('missing-model-identities', lambda: prep.profile_check(profile, bound(profile)))
    invalid = bytearray(profile)
    invalid[768] ^= 1
    refused('changed-model-identity', lambda: profile_valid(invalid))

    record = {'payloads': payloads, 'recognition_provider': models, 'bluetooth_stop': evidence['stop']}
    calls = []

    def admission(record_=record, cache_=cache):
        return prep.bluetooth_admission(record_, profile, bound(profile), {}, 'fixture', cache_, True,
                                        application_admission=lambda *args: calls.append(args))

    assert len(admission()) == 4 and len(calls) == 1
    passed.append('standalone-reset-evidence')
    lazy_dir = output / 'frozen-input'
    lazy_dir.mkdir(exist_ok=True)
    lazy_name = 'reset-proof.bin'
    (lazy_dir / lazy_name).write_bytes(cache[evidence['stop']['path']])
    lazy = prep.board.input_cache(lazy_dir, {
        'entry_evidence': {evidence['stop']['path']: {'name': lazy_name, 'sha256': evidence['stop']['sha256']}},
        'files': {lazy_name: evidence['stop']['sha256']}})
    assert len(admission(cache_=lazy)) == 4
    passed.append('standalone-lazy-frozen-reset-evidence')
    legacy = bytearray(profile[:832])
    struct.pack_into('<II', legacy, 8, 3, 832)
    legacy_record = {key: value for key, value in record.items() if key != 'bluetooth_stop'}
    assert len(prep.bluetooth_admission(legacy_record, legacy, bound(legacy), {}, 'fixture', cache, True,
                                        application_admission=lambda *args: None)) == 4
    passed.append('standalone-legacy-without-reset-authority')
    refused('standalone-missing-reset-reference', lambda: admission({k: v for k, v in record.items() if k != 'bluetooth_stop'}))
    refused('standalone-unfrozen-reset-evidence', lambda: admission(cache_={k: v for k, v in cache.items() if k != evidence['stop']['path']}))
    changed = dict(cache)
    changed[evidence['stop']['path']] = b'changed'
    refused('standalone-changed-reset-evidence', lambda: admission(cache_=changed))

    # Observe pending records using the original wire formats. These are parser
    # fixtures, not evidence that a controller completes a physical operation.
    for size, version in ((832, 3), (888, 4)):
        observed = bytearray(profile[:size])
        struct.pack_into('<II', observed, 8, version, size)
        raw = bound(observed)
        for decoder, magic, length, args in (
                (observer.decode_pcm, 0x4d435442, 160, (0, 0, 1)),
                (observer.decode_recognition, 0x52544242, 208, (0, 1)),
                (observer.decode_repeated, 0x53525442, 272, ())):
            frame = bytearray(length)
            struct.pack_into('<4I2Q', frame, 0, magic, 1, 0, 0, 31, 37)
            frame[32:64] = raw[320:352]
            status, data = decoder(frame, raw, observed, *args)
            assert status['state'] in ('pending', 0) and data == b''
            passed.append(decoder.__name__ + '-version-' + str(version))

    # Run the real OS asset validator through the native firmware catalogue.
    # All topology statements below are synthetic host inputs kept in memory.
    os_raw = bytearray(808)
    os_raw[:592] = session
    struct.pack_into('<I', os_raw, 400, 0)
    struct.pack_into('<4Q', os_raw, 456, 103, 107, 109, 113)
    struct.pack_into('<Q', os_raw, 616, 127)
    context = {'identity': 'host-fixture', 'port': 'host-only'}
    proof = {'schema': 'a733-radio-sdio-id-v1', **context}
    for group, keys in assets.FACTS.items():
        fact = {'schema': 'a733-radio-sdio-id-v1-' + group, **context,
                **{key: True for key in keys}, 'scope': 'Host parser fixture; no physical authority'}
        if group == 'topology':
            fact['rails'] = {'vbat': 'dcdc1', 'pg': 'bldo5', 'pm': 'cldo1', 'vddio': 'bldo5'}
        if group == 'pins':
            for key, offset in [('expected', 256), ('desired', 280)]:
                fact[key] = [list(profile[offset + 3*i:offset + 3*i + 3]) for i in range(8)]
            fact['pm_latch'] = 4
        if group == 'startup':
            fact.update(fabric=[1, 2, 3], fabric_hz=[400000000, 200000000, 24000000],
                        peri0_400mhz_branch_verified=True)
        proof[group] = document(group + '.json', fact)
    asset_doc = {'schema': assets.SCHEMA, **context, 'epoch': 31, 'generation': 37, 'lease': 41,
                 'headset_profile': frozen('profile.bin', profile), 'platform': document('platform.json', proof),
                 'payloads': payloads, **{'bluetooth_' + key: ref for key, ref in evidence.items()}}

    def os_admission(doc=asset_doc, data=profile, cache_=cache):
        item = {'radio_assets': document('assets.json', doc), **context}
        cache_[item['radio_assets']['path']] = cache[item['radio_assets']['path']]
        return assets.validate(item, bound(data, os_raw), {'radio': False, 'headset': True, 'msbc': False},
                               cache_, prep.board, prep, require, frozen=True, model_pin=models)

    assert len(os_admission()) == 5
    passed.append('os-reset-evidence')
    missing = copy.deepcopy(asset_doc)
    missing.pop('bluetooth_stop')
    refused('os-missing-reset-evidence', lambda: os_admission(missing))
    changed = copy.deepcopy(asset_doc)
    changed['bluetooth_stop'] = evidence['wiring']
    refused('os-other-reset-evidence', lambda: os_admission(changed))
    missing_cache = {key: value for key, value in cache.items() if key != evidence['stop']['path']}
    refused('os-unfrozen-reset-evidence', lambda: os_admission(cache_=missing_cache))
    legacy_doc = copy.deepcopy(asset_doc)
    legacy_doc['headset_profile'] = frozen('legacy-profile.bin', legacy)
    legacy_doc.pop('bluetooth_stop')
    assert len(os_admission(legacy_doc, legacy)) == 5
    passed.append('os-legacy-without-reset-authority')
    result = {'passed': True, 'cases': passed, 'count': len(passed),
              'scope': 'Host admission and copied-result parsing; no physical Bluetooth reset qualification'}
    (output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'passed': True, 'cases': len(passed)}))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--firmware', type=Path, required=True)
    args = parser.parse_args()
    check(args.output, args.firmware)
