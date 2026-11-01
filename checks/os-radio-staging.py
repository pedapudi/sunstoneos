#!/usr/bin/env python3
"""Check radio staging without reviewed lifetimes, claims or frozen bundles.

Electrical state and ownership are modeled. Profile parsers and firmware
catalogue checks use production code; no device access occurs.
"""
import argparse
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
from harness import refused

ROOT = Path(__file__).resolve().parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


assets = module('radio_staging_assets', ROOT / 'apps/os/radio-assets.py')
board = module('radio_staging_board', ROOT / 'apps/board/prepare.py')
bluetooth = module('radio_staging_bluetooth', ROOT / 'apps/bluetooth/prepare.py')
sha = lambda data: hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--firmware', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    serial = 0
    def put(data):
        nonlocal serial
        serial += 1
        if not isinstance(data, (bytes, bytearray)):
            data = json.dumps(data).encode()
        path = (args.output / ('model-%d.bin' % serial)).resolve()
        path.write_bytes(data)
        return dict(path=str(path), sha256=sha(data))
    raw = bytearray(808)
    struct.pack_into('<3Q', raw, 24, 11, 12, 13)
    struct.pack_into('<Q', raw, 184, 7)
    struct.pack_into('<Q', raw, 312, 8)
    struct.pack_into('<I', raw, 400, 1)
    struct.pack_into('<2Q2I', raw, 432, 64, 65, 0, 0)
    struct.pack_into('<4Q', raw, 456, 10, 11, 12, 13)
    struct.pack_into('<6I', raw, 488, 1, 2, 3, 200000000, 200000000, 200000000)
    struct.pack_into('<Q', raw, 616, 14)
    r, h = bytearray(1872), bytearray(832)
    struct.pack_into('<QII', r, 0, 0x3152464f52545353, 4, len(r))
    struct.pack_into('<QII', h, 0, 0x3150465442535353, 3, len(h))
    for i, offset in enumerate((16, 24, 32, 40, 48, 56, 64)):
        struct.pack_into('<Q', r, offset, 70 + i)
    platform = bytearray(152)
    struct.pack_into('<Q', platform, 0, 60)
    struct.pack_into('<Q4I', platform, 32, 7, 0, 1, 1, 1)
    platform[56:80] = raw[488:512]
    r[184:336] = h[176:328] = platform
    r[384:408] = raw[488:512]
    h[152:176] = raw[432:456]
    struct.pack_into('<5Q', h, 16, 7, 8, 80, 81, 82)
    struct.pack_into('<Q', r, 336, 61)
    struct.pack_into('<Q', h, 352, 62)
    struct.pack_into('<Q', r, 1224, 63)
    struct.pack_into('<6Q', h, 576, 63, 90000, 1000, 5000000, 4000000, 9)
    struct.pack_into('<Q', h, 328, 82)
    struct.pack_into('<I7I', h, 376, 3, 1, 8, 2, 0, 1, 24000000, 1500000)
    chip = struct.pack('<2I4B', 0xc30000, 0, 3, 0, 1, 1)
    r[496:508] = h[496:508] = chip
    struct.pack_into('<2I', h, 624, 2, 1)
    h[632:638] = bytes([1, 2, 3, 4, 5, 6])
    struct.pack_into('<2IQ', h, 656, 2, 7, 17)
    struct.pack_into('<2Q', h, 704, 1000, 20000)
    struct.pack_into('<I4H2I', h, 720, 24000, 128, 0x1fa0, 0x800, 0x12, 1000000, 5000000)
    models = {'models': [dict(bytes=16, sha256='ab' * 32), dict(bytes=32, sha256='cd' * 32)]}
    struct.pack_into('<2Q', h, 752, *(m['bytes'] for m in models['models']))
    h[768:832] = b''.join(bytes.fromhex(m['sha256']) for m in models['models'])
    target = dict(identity=[7, 2, 3, 0, 0], port='3-3', epoch=11, generation=12, lease=13)
    record = {k: target[k] for k in ('identity', 'port')}
    doc = dict(schema=assets.SCHEMA, **target, chip_id=[0, 0, 0, 0], sid_authority=8)
    facts = {}
    for group, keys in assets.FACTS.items():
        facts[group] = dict(schema='a733-radio-sdio-id-v1-' + group,
                            identity=target['identity'], port=target['port'], **dict.fromkeys(keys, True))
    facts['topology']['rails'] = dict(vbat='dcdc1', pg='dcdc8', pm='dcdc8', vddio='dcdc8')
    facts['pins'].update(expected=[[0, 0, 0]] * 8, desired=[[0, 0, 0]] * 8)
    facts['startup'].update(fabric=[1, 2, 3], fabric_hz=[200000000] * 3, peri0_400mhz_branch_verified=True)
    for key, offset, profile in [('radio_ce', 120, r), ('radio_entropy', 152, r),
                                 ('bluetooth_wiring', 56, h), ('bluetooth_sid', 88, h),
                                 ('bluetooth_transport', 120, h), ('bluetooth_scheduling', 672, h)]:
        doc[key] = put({'modeled_security_input': key})
        profile[offset:offset + 32] = bytes.fromhex(doc[key]['sha256'])
    doc['radio_sid'] = doc['bluetooth_sid']
    names = ['fw_adid_8800d80_u02.bin', 'fw_patch_8800d80_u02.bin',
             'fw_patch_table_8800d80_u02.bin', 'fmacfw_8800d80_h_u02.bin']
    addresses = [0x43010000, 0x43020000, 0x43030000, 0x43040000]
    doc['payloads'] = [dict(path=str((args.firmware / name).resolve()),
                            sha256=sha((args.firmware / name).read_bytes()), address=address)
                       for name, address in zip(names, addresses)]
    sizes = [(args.firmware / name).stat().st_size for name in names]
    struct.pack_into('<4Q', r, 432, *addresses)
    struct.pack_into('<4Q', r, 464, *sizes)
    struct.pack_into('<4Q', h, 512, *addresses)
    struct.pack_into('<4Q', h, 544, *sizes)
    def check(edit=None, reason=None, mode=None, frozen=False, pin=models, corrupt=False):
        rr, hh, ss, dd, ff = bytearray(r), bytearray(h), bytearray(raw), copy.deepcopy(doc), copy.deepcopy(facts)
        selected = mode or dict(radio=True, headset=True, msbc=False)
        if edit:
            edit(rr, hh, ss, dd, ff)
        proof = dict(schema='a733-radio-sdio-id-v1', identity=target['identity'], port=target['port'])
        proof.update({group: put(fact) for group, fact in ff.items()})
        dd['platform'] = put(proof)
        rr[88:120] = bytes.fromhex(dd['platform']['sha256'])
        if selected['radio']:
            dd['radio_profile'] = put(rr)
            struct.pack_into('<2Q', ss, 352, 0x43000000, len(rr))
            ss[368:400] = bytes.fromhex(dd['radio_profile']['sha256'])
        if selected['headset']:
            dd['headset_profile'] = put(hh)
            ss[320:352] = bytes.fromhex(dd['headset_profile']['sha256'])
        else:
            ss[320:352] = bytes.fromhex(dd['radio_sid']['sha256'])
        entry = {**record, 'radio_assets': put(dd)}
        cache = {}
        if corrupt:
            cache[dd['radio_profile']['path']] = b'changed'
        action = lambda: assets.validate(entry, ss, selected, cache, board, bluetooth, board.require,
                                         frozen=frozen, model_pin=pin)
        if reason:
            refused(action, reason)
        else:
            plan = action()
            assert len(plan) == 4 + selected['radio'] + selected['headset']
        return entry, ss, selected, cache
    cases = []
    def case(name, **kwargs):
        check(**kwargs)
        cases.append(name)
    case('profiles-and-pinned-firmware-without-review-lifetime-claim-or-ddr-record')
    case('legacy-frozen-flag-allows-direct-inputs', frozen=True)
    case('radio-only', mode=dict(radio=True, headset=False, msbc=False))
    case('headset-only', mode=dict(radio=False, headset=True, msbc=False))
    case('wrong-session', edit=lambda r,h,s,d,f:d.update(lease=14), reason='another target or session')
    case('wrong-target', edit=lambda r,h,s,d,f:d.update(port='3-1'), reason='another target or session')
    case('missing-ownership', edit=lambda r,h,s,d,f:struct.pack_into('<Q',r,16,0), reason='authority absent')
    case('unsafe-startup', edit=lambda r,h,s,d,f:f['startup'].update(device_mappings_security_verified=False), reason='Missing radio startup')
    case('wrong-parent', edit=lambda r,h,s,d,f:f['startup'].update(peri0_400mhz_branch_verified=False), reason='PERI0 400 MHz')
    case('wrong-fitted-rails', edit=lambda r,h,s,d,f:f['topology']['rails'].update(pg='bldo5'), reason='Fitted rail routing')
    case('owner-collision', edit=lambda r,h,s,d,f:struct.pack_into('<Q',h,352,14), reason='Headset owners collide')
    case('unsupported-codec', edit=lambda r,h,s,d,f:struct.pack_into('<I',h,624,3), reason='linked codec')
    case('changed-cache-bytes', corrupt=True, reason='asset changed')
    case('missing-firmware-role', edit=lambda r,h,s,d,f:d.update(payloads=[]), reason='Exactly four')
    case('firmware-address', edit=lambda r,h,s,d,f:d['payloads'][0].update(address=0x44000000), reason='role address')
    case('profile-handoff', edit=lambda r,h,s,d,f:struct.pack_into('<Q',r,192,1), reason='handoffs/software deadlines')
    case('model-identity', pin={'models': [dict(bytes=17, sha256='ab'*32), models['models'][1]]}, reason='Application model identities differ')
    def overlap(r,h,s,d,f):
        struct.pack_into('<Q',r,440,addresses[0]);struct.pack_into('<Q',h,520,addresses[0])
        d['payloads'][1]['address'] = addresses[0]
    case('firmware-overlap', edit=overlap, reason='Overlapping profile')
    def bad_pm(r,h,s,d,f):
        struct.pack_into('<I',s,452,1);h[152:176] = s[432:456]
    case('invalid-pm-mask', edit=bad_pm, reason='Invalid normal PM')
    def full_pm(r,h,s,d,f):
        struct.pack_into('<2I',s,448,63,5);h[152:176] = s[432:456]
        f['pins']['pm_latch'] = 5
    case('pm-latch-without-writer-history', edit=full_pm)
    def bad_latch(r,h,s,d,f):
        full_pm(r,h,s,d,f);f['pins']['pm_latch'] = 6
    case('wrong-pm-latch', edit=bad_latch, reason='PM latch differs')
    def corrupt_role(r,h,s,d,f):
        data = bytearray(Path(d['payloads'][0]['path']).read_bytes());data[0] ^= 1
        d['payloads'][0] = {**put(data), 'address': addresses[0]}
    case('catalogue-rejects-self-consistent-wrong-firmware', edit=corrupt_role, reason='native catalogue')
    assert assets.validate({}, raw, dict(radio=False, headset=False), {}, board, bluetooth, board.require) == []
    cases.append('no-radio-compatible')
    (args.output / 'result.json').write_text(json.dumps(dict(passed=True, physical_access=False, cases=cases), indent=2) + '\n')
    print('PASS', len(cases), 'radio staging cases')


if __name__ == '__main__':
    main()
