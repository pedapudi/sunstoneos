#!/usr/bin/env python3
"""Check returning CPU grants without review or historical qualification files."""
import argparse
import copy
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


clock = module('cpu_clock_profile_check', ROOT / 'apps/os/cpu-return-clock-review.py')
host = module('cpu_clock_host_check', ROOT / 'apps/os/host.py')
review = module('cpu_return_metadata_check', ROOT / 'apps/os/cpu-return-review.py')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    profile = dict(epoch=11, generation=12, lease=13)
    record = dict(identity=[7, 2, 3, 0, 0], port='3-3', **profile,
                  inference=dict(version=1, workers=8, owner_core=6, core_mask=255,
                                 owner_priority=3, a55_priority=2, a76_priority=3))
    image_hash = 'ab' * 32
    raw = bytearray(904)
    struct.pack_into('<3Q', raw, 24, *profile.values())
    raw[48:80] = bytes.fromhex('cd' * 32)  # Native build ID differs from the ELF digest.
    struct.pack_into('<I', raw, 400, 2)
    struct.pack_into('<3Q', raw, 592, 0x415337334f534131, 1, 904)
    struct.pack_into('<I', raw, 804, 70000)
    struct.pack_into('<6Q4I32s', raw, 808, 0x4153373343505531, 1, 96, 99, 11, 12,
                     3, 1794, 2002, 32, bytes([1]) * 32)
    grant = dict(owner=99, clusters=3, max_mhz=(1794, 2002), changes=32, approval=bytes([1]) * 32)
    def unused(*args, **kwargs):
        raise AssertionError('Review records must not be read')
    def check(wire=raw, target=record, decoded=grant, selected=profile):
        clock.validate({}, b'', target, image_hash, wire, selected, decoded, unused, host.require, unused)
    cases = []
    metadata = review.validate(record, unused, image_hash, 'ef' * 32, host.require)
    assert metadata['port'] == record['port'] and metadata['image_sha256'] == image_hash
    cases.append('return-preparation-without-final-source-review')
    legacy = {**record, 'normal_return_review': dict(path='/unavailable/review.json', sha256='00' * 32),
              'whole_board_power_removal': True, 'no_resource_reuse_before_power_removal': True}
    assert review.validate(legacy, unused, image_hash, 'ef' * 32, host.require) == metadata
    cases.append('legacy-review-and-retention-fields-do-not-admit-execution')
    check()
    cases.append('legacy-session-without-review-history-claim-or-helper-image')
    typed = host.session_bytes(raw[:592], {host.THERMAL: raw[592:808], host.CPU_GRANT: raw[808:]},
                               None, '00' * 32, 0, host.BIND_SID)
    check(typed)
    cases.append('typed-session-without-review-history-claim-or-helper-image')
    for name, offset, fmt, value, reason in (
        ('session-generation', 32, '<Q', 20, 'session identity'),
        ('grant-session', 840, '<Q', 20, 'limits or owner'),
        ('grant-owner', 832, '<Q', 0, 'limits or owner'),
        ('grant-clusters', 856, '<I', 1, 'limits or owner'),
        ('little-clock-limit', 860, '<I', 2016, 'limits or owner'),
        ('big-clock-limit', 864, '<I', 2106, 'limits or owner'),
        ('too-few-changes', 868, '<I', 3, 'limits or owner'),
        ('too-many-changes', 868, '<I', 65, 'limits or owner'),
        ('critical-temperature', 804, '<I', 90000, 'thermal configuration'),
    ):
        bad = bytearray(raw)
        struct.pack_into(fmt, bad, offset, value)
        refused(lambda: check(bad), reason)
        cases.append('reject-' + name)
    refused(lambda: check(raw[:-1]), 'staged workload')
    cases.append('reject-truncated-session')
    bad = bytearray(raw); bad[872:904] = bytes(32)
    refused(lambda: check(bad), 'limits or owner')
    cases.append('reject-ungranted-native-record')
    bad = bytearray(raw); bad[48:80] = bytes(32)
    refused(lambda: check(bad), 'build identity absent')
    cases.append('reject-missing-native-build-identity')
    refused(lambda: check(decoded={**grant, 'owner': 100}), 'differs from session')
    cases.append('reject-decoded-grant-mismatch')
    bad = copy.deepcopy(record); bad['inference']['workers'] = 7
    refused(lambda: check(target=bad), 'inference profile')
    cases.append('reject-worker-profile')
    refused(lambda: check(target={**record, 'identity': None}), 'target identity')
    cases.append('reject-missing-target')
    (args.output / 'result.json').write_text(json.dumps(dict(passed=True, physical_access=False, cases=cases), indent=2) + '\n')
    print('PASS', len(cases), 'CPU profile cases; no review records read')


if __name__ == '__main__':
    main()
