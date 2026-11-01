#!/usr/bin/env python3
"""Prepare a returning CPU diagnostic for a board's current USB instance.

The bundle supplies build/ and template/prepared/. The current host loaders
check the ELF, overlay and reserved memory. The native launcher checks current
ROM, cache and CPU state before execution. The native relock checks the chip
revision, oscillator, speed bin and both CPU rails before a clock write. Preparation
regenerates the native session contract and prints a direct runner command.
No device access occurs here.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import secrets
import shlex
import shutil
from host_evidence import new_directory, unique_directory

ROOT = Path(__file__).resolve().parents[1]
CACHE = Path.home() / '.cache/sunstoneos'


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def read(path):
    return json.loads(path.read_text())


def save(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n')


def require(condition, message):
    if not condition:
        raise ValueError(message)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('bundle', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--survey', type=Path, help='optional earlier survey path, recorded only')
    parser.add_argument('--port', required=True, choices=('3-1', '3-3'))
    parser.add_argument('--root-review', type=Path, help='optional review note path, recorded only')
    parser.add_argument('--continuity', type=Path, action='append', default=[],
                        help='optional earlier run paths, recorded only')
    args = parser.parse_args(argv)
    bundle = args.bundle.resolve()
    runner = module('cpu_runner', ROOT / 'tools/run-native-startup-probe.py')
    supervised = runner.supervised
    manifest = read(bundle / 'build/manifest.json')
    hosts = {'a733-all-core-pre-scheduler-probe-v1': 'all-core-pre-scheduler-host.py',
             'a733-all-core-first-task-probe-v1': 'all-core-first-task-host.py',
             'a733-all-core-task-switch-probe-v1': 'all-core-task-switch-host.py',
             'a733-all-core-task-workload-probe-v1': ('workload-bisect-host.py' if 'workload_bisect' in manifest
                                                       else 'all-core-task-workload-host.py'),
             'a733-primary-cache-probe-v1': 'primary-cache-host.py'}
    require(manifest.get('schema') in hosts, 'Unsupported diagnostic layout')
    host = module('cpu_host', ROOT / 'tools' / hosts[manifest['schema']])
    template = bundle / 'template/prepared'
    supervised.validate_bundle(template)
    host.load(bundle / 'build', template / 'image.elf', manifest)
    output = new_directory(args.output.resolve())
    prepared_dir = output / 'prepared'
    # Copy bytes rather than source modes: preserved inputs can be read-only.
    prepared_dir.mkdir()
    for source in sorted(template.rglob('*')):
        target = prepared_dir / source.relative_to(template)
        if source.is_dir():
            target.mkdir()
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)
    metadata = read(prepared_dir / 'prepared.json')
    admission = {'schema': 'a733-smp-supervised-entry-v1',
                 'image_sha256': hashlib.sha256((prepared_dir / 'image.elf').read_bytes()).hexdigest(),
                 'port': args.port, 'reservation': secrets.randbits(63) or 1}
    save(prepared_dir / 'admission.json', admission)
    segments, address, build_id = supervised.image_parts(prepared_dir / 'image.elf')
    epoch, generation, lease = (secrets.randbits(63) or 1 for _ in range(3))
    admission_hash = supervised.digest(prepared_dir / 'admission.json')
    request, contract = supervised.request_bytes(address, build_id, epoch, generation, lease,
                                                  admission['reservation'], bytes.fromhex(admission_hash))
    (prepared_dir / 'request.bin').write_bytes(request)
    (prepared_dir / 'contract.bin').write_bytes(contract)
    metadata.update(epoch=epoch, generation=generation, lease=lease, reservation=admission['reservation'],
                    admission_sha256=admission_hash)
    metadata['files']['admission.json'] = admission_hash
    metadata['files'] = {name: supervised.digest(prepared_dir / name) for name in metadata['files']}
    save(prepared_dir / 'prepared.json', metadata)
    supervised.validate_bundle(prepared_dir)
    run = output / 'run'
    command = ['env', 'SUNSTONE_USB_PORT=' + args.port, str(CACHE / 'venv/bin/python'), '-B', str(ROOT / 'tools/run-native-startup-probe.py'), 'run',
               '--build', str(bundle / 'build'), '--bundle', str(prepared_dir),
               '--fel', str(CACHE / 'build/sunxi-a733/sunxi-fel'), '--evidence', str(run)]
    save(output / 'command.json', command)
    save(output / 'offline-preparation.json', {
        'bundle_validated': True, 'request_regenerated': True,
        'survey': str(args.survey) if args.survey else None,
        'port': args.port, 'physical_access': False, 'review_note': str(args.root_review) if args.root_review else None,
        'earlier_runs': [str(path) for path in args.continuity]})
    print(shlex.join(command))


if __name__ == '__main__':
    main()
