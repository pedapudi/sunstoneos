#!/usr/bin/env python3
"""Freeze a manifest-bound typed session and application inputs without device access.

This produces offline inputs. The caller must separately establish board,
service and recovery authority before using a transport launcher.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('os_host_records', ROOT / 'apps/os/host.py')
host = importlib.util.module_from_spec(spec)
spec.loader.exec_module(host)
require = host.require
SCHEMA = 'sunstone-session-inputs-v1'


def prepare(image, prefix, services, inputs, output, *, devices, flags=0):
    image = Path(image).read_bytes()
    info = host.image_manifest(image)
    prefix = bytes(prefix)
    values = [bytes(data) for data in inputs]
    facts, uploads = host.input_facts(info['manifest'], values)
    session = host.session_bytes(prefix, services, info['manifest'], info['sha256'], devices, flags, values)
    output = Path(output)
    output.mkdir(parents=True, exist_ok=False)
    files = {'image.elf': image, 'session.bin': session}
    for item, data in zip(uploads, values):
        item['name'] = 'input-%d.bin' % item.pop('index')
        files[item['name']] = data
    for name, data in files.items():
        (output / name).write_bytes(data)
    record = {'schema': SCHEMA, 'physical_access': False, 'admission_required': True,
              'manifest_sha256': info['sha256'], 'devices': devices, 'flags': flags,
              'epoch': struct.unpack_from('<Q', prefix, 24)[0],
              'generation': struct.unpack_from('<Q', prefix, 32)[0], 'uploads': uploads,
              'files': {name: hashlib.sha256(data).hexdigest() for name, data in files.items()}}
    (output / 'prepared.json').write_text(json.dumps(record, indent=2) + '\n')
    return validate_bundle(output)


def validate_bundle(output):
    output = Path(output)
    record = json.loads((output / 'prepared.json').read_text())
    require(record.get('schema') == SCHEMA and record.get('physical_access') is False and
            record.get('admission_required') is True, 'Unsupported session input bundle.')
    require({'image.elf', 'session.bin'} <= set(record['files']), 'Missing session input files.')
    files = {}
    for name, digest in record['files'].items():
        require(Path(name).name == name, 'Invalid bundle filename.')
        data = (output / name).read_bytes()
        require(hashlib.sha256(data).hexdigest() == digest, 'Session input changed.')
        files[name] = data
    info = host.image_manifest(files['image.elf'])
    require(info['sha256'] == record['manifest_sha256'], 'Application manifest changed.')
    decoded = host.session_decode(files['session.bin'], info['manifest'], info['sha256'])
    require((decoded['devices'], decoded['flags']) == (record['devices'], record['flags']), 'Device grant changed.')
    expected = []
    for index, region in enumerate(host.regions(info['manifest'], (1,))):
        name = 'input-%d.bin' % index
        require(name in files, 'Missing frozen input.')
        expected.append({'name': name, 'address': region['address'], 'bytes': len(files[name]),
                         'sha256': hashlib.sha256(files[name]).hexdigest()})
    require(record['uploads'] == expected and set(files) == {'image.elf', 'session.bin'} | {x['name'] for x in expected},
            'Upload plan differs from manifest.')
    facts, _ = host.input_facts(info['manifest'], [files[x['name']] for x in expected])
    require(decoded['extents'].get(host.INPUTS, b'') == facts, 'Session inputs differ from frozen uploads.')
    require(struct.unpack_from('<2Q', files['session.bin'], 24) == (record['epoch'], record['generation']),
            'Session identity changed.')
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--prefix', type=Path, required=True)
    parser.add_argument('--input', type=Path, action='append', default=[])
    parser.add_argument('--service', action='append', default=[], metavar='TYPE=FILE')
    parser.add_argument('--devices', type=lambda x: int(x, 0), required=True)
    parser.add_argument('--bind-sid', action='store_true')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    services = {}
    for item in args.service:
        kind, path = item.split('=', 1)
        kind = int(kind, 0)
        require(kind not in services, 'Duplicate service argument.')
        services[kind] = Path(path).read_bytes()
    print(json.dumps(prepare(args.image, args.prefix.read_bytes(), services,
          [p.read_bytes() for p in args.input], args.output, devices=args.devices,
          flags=host.BIND_SID if args.bind_sid else 0), indent=2))


if __name__ == '__main__':
    main()
