#!/usr/bin/env python3
"""Record each device capture tool's claim, software and reference refusals; no device access.

For every device capture tool listed below, the check builds a prepared
bundle, review references, software evidence and a queue database, then calls
the tool's claimed, validate_software and reference_bytes with a valid input
and with one tampered field at a time. outcomes.json records whether each case
passed or the refusal it produced. Run it with --root on two checkouts and
compare the two outcomes.json files to show that a change keeps every
admission decision.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import sqlite3
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness

ROOT = Path(__file__).resolve().parents[1]
FLAGS = ('admission_cases', 'launcher_cases', 'host_capture', 'host_lifecycle')
# Device: (software-evidence schema, result flags).
DEVICES = {
    'bluetooth': ('a733-bluetooth-supervised-software-v1', FLAGS),
    'camera': ('a733-camera-supervised-software-v1', FLAGS),
    'crypto': ('a733-crypto-supervised-software-v1', FLAGS),
    'displayport': ('a733-displayport-supervised-software-v1', FLAGS),
    'g2d': ('a733-g2d-supervised-software-v1', FLAGS),
    'gpu': ('a733-gpu-supervised-software-v1', FLAGS),
    'h264-encode': ('a733-h264-encode-supervised-software-v1', FLAGS),
    'h264': ('a733-h264-supervised-software-v1', FLAGS),
    'hdmi-audio': ('a733-hdmi-audio-supervised-software-v1',
                   ('admission_cases', 'launcher_cases', 'host_capture', 'native_composition')),
    'hevc': ('a733-hevc-supervised-software-v1', FLAGS),
    'i2s': ('a733-i2s-supervised-software-v1', FLAGS),
    'isp0': ('a733-isp0-supervised-software-v1', FLAGS),
    'jpeg-encode': ('a733-jpeg-encode-supervised-software-v1', FLAGS),
    'jpeg': ('a733-jpeg-supervised-software-v1', FLAGS),
    'mimus': ('a733-mimus-supervised-software-v1', FLAGS),
    'pwm': ('a733-pwm-supervised-software-v1',
            ('session_binding', 'waveform_irq_stop', 'reusable_commands', 'revocation_retention', 'host_capture')),
    'rtc': ('a733-rtc-supervised-software-v1', ('admission_cases', 'launcher_cases', 'host_capture')),
    'storage-read': ('a733-storage-read-supervised-software-v2',
                     ('session_binding', 'read_only_trace', 'revocation_retention', 'host_capture')),
}


def load(root, device):
    path = root / f'tools/check-a733-{device}-supervised.py'
    spec = importlib.util.spec_from_file_location(f'claims_{device.replace("-", "_")}', path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


def reference(path):
    return {'path': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}


def outcome(action, out):
    try:
        action()
    except Exception as error:
        return f'{type(error).__name__}: {error}'.replace(str(out), '<out>')
    return 'accepted'


def device_cases(tool, device, out):
    schema, flags = DEVICES[device]
    work = out / device
    bundle = work / 'bundle'
    bundle.mkdir(parents=True)
    for name in ('admission.json', 'image.elf', 'launch.bin', 'prepared.json'):
        (bundle / name).write_bytes(f'{device} {name}'.encode())
    sources = work / 'sources'
    sources.mkdir()
    for name in ('tool.py', 'model.py'):
        (sources / name).write_text(f'# {device} {name}\n')
    other = work / 'other'
    other.mkdir()
    (other / 'admission.json').write_bytes((bundle / 'admission.json').read_bytes())
    (other / 'image.elf').write_bytes(b'another image')
    (work / 'result.json').write_text('{}\n')
    digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    prepared = {'files': {name: digest(bundle / name) for name in ('admission.json', 'image.elf', 'launch.bin')},
                '_prepared_sha256': digest(bundle / 'prepared.json'),
                '_source_hashes': {name: digest(sources / name) for name in ('tool.py', 'model.py')}}

    def software(**changes):
        value = {'schema': schema, 'physical_access': False, **{flag: True for flag in flags},
                 'image': reference(bundle / 'image.elf'), 'launcher': reference(bundle / 'launch.bin'),
                 'sources': {name: reference(sources / name) for name in ('tool.py', 'model.py')},
                 'evidence': [reference(work / 'result.json')]}
        value.update(changes)
        return value

    stale = reference(work / 'result.json')
    stale['sha256'] = '0' * 64
    software_name = next(iter(tool.PREREQUISITES - {'supervised-ram-recovery'}))
    cases = {
        'reference-valid': lambda: tool.reference_bytes(reference(bundle / 'image.elf')),
        'reference-extra-field': lambda: tool.reference_bytes({**reference(bundle / 'image.elf'), 'note': 1}),
        'reference-relative-path': lambda: tool.reference_bytes({'path': 'image.elf', 'sha256': prepared['files']['image.elf']}),
        'reference-changed': lambda: tool.reference_bytes(stale),
        'software-valid': lambda: tool.validate_software(software(), prepared),
        'software-schema': lambda: tool.validate_software(software(schema=schema + '-other'), prepared),
        'software-physical': lambda: tool.validate_software(software(physical_access=True), prepared),
        'software-flag': lambda: tool.validate_software(software(**{flags[0]: False}), prepared),
        'software-image': lambda: tool.validate_software(software(image=reference(other / 'image.elf')), prepared),
        'software-sources-incomplete': lambda: tool.validate_software(
            software(sources={'tool.py': reference(sources / 'tool.py')}), prepared),
        'software-source-other': lambda: tool.validate_software(
            software(sources={'tool.py': reference(sources / 'tool.py'), 'model.py': reference(sources / 'tool.py')}), prepared),
        'software-no-evidence': lambda: tool.validate_software(software(evidence=[]), prepared),
        'software-stale-evidence': lambda: tool.validate_software(software(evidence=[stale]), prepared),
    }
    (work / 'software.json').write_text(json.dumps(software()))
    (work / 'software-bad.json').write_text(json.dumps(software(schema='other')))
    artifacts = [reference(bundle / name) for name in ('prepared.json', 'image.elf', 'launch.bin')]

    def queue(name, halted='', state='claimed', owner='owner', prerequisites=None, entry=None,
              software_file='software.json', submitted=None):
        refs = {'supervised-ram-recovery': reference(entry or bundle / 'admission.json'),
                software_name: reference(work / software_file)}
        names = sorted(prerequisites or tool.PREREQUISITES)
        path = work / (name + '.db')
        db = sqlite3.connect(path)
        db.execute('CREATE TABLE settings(key TEXT PRIMARY KEY, value TEXT NOT NULL)')
        db.execute('CREATE TABLE tests(id TEXT PRIMARY KEY, state TEXT, owner TEXT, submission TEXT, review TEXT)')
        db.execute('INSERT INTO settings VALUES(?,?)', ('halted', halted))
        db.execute('INSERT INTO tests VALUES(?,?,?,?,?)', ('fixture', state, owner,
                   json.dumps({'prerequisites': names, 'artifacts': artifacts if submitted is None else submitted}),
                   json.dumps({'prerequisites': refs})))
        db.commit()
        db.close()
        return path

    claims = {
        'claim-valid': queue('valid'),
        'claim-halted': queue('halted', halted='coordinator stop'),
        'claim-state': queue('state', state='queued'),
        'claim-owner': queue('owner', owner='other'),
        'claim-prerequisites': queue('prerequisites', prerequisites=set(tool.PREREQUISITES) | {'extra'}),
        'claim-entry-path': queue('entry', entry=other / 'admission.json'),
        'claim-software': queue('software', software_file='software-bad.json'),
        'claim-artifacts': queue('artifacts', submitted=artifacts[:2]),
    }
    for name, database in claims.items():
        cases[name] = lambda database=database: tool.claimed(database, 'fixture', 'owner', bundle, prepared)
    cases['claim-missing-row'] = lambda: tool.claimed(claims['claim-valid'], 'absent', 'owner', bundle, prepared)
    result = {name: outcome(action, out) for name, action in cases.items()}
    result['software-sources'] = sorted((name, Path(path).name) for name, path in tool.software_sources().items())
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--root', type=Path, default=ROOT, help='checkout whose tools/ are exercised')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    outcomes = {device: device_cases(load(args.root.resolve(), device), device, out) for device in DEVICES}
    for device, result in outcomes.items():
        valid = ('reference-valid', 'software-valid', 'claim-valid')
        for name in valid:
            assert result[name] == 'accepted', (device, name, result[name])
        for name in set(result) - set(valid) - {'software-sources'}:
            assert result[name].startswith('ValueError: '), (device, name, result[name])
    harness.save(out / 'outcomes.json', outcomes)
    print(f'PASS {len(outcomes)} device tools, {sum(len(v) for v in outcomes.values())} recorded outcomes')


if __name__ == '__main__':
    main()
