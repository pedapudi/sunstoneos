#!/usr/bin/env python3
"""Report artifacts and bounded host time for one supervised SD0 read.

The report is optional scheduling information. Device access depends on the
selected port lock, live identity and native session limits. Observation timeouts
do not release a live run's port lock or authorize a retry.
"""
import argparse
import importlib.util
import json
import sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
CAPTURE_SECONDS = 59
SETUP_SECONDS = 60
ARTIFACTS = ('prepared.json', 'image.elf', 'launch.bin')


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec); spec.loader.exec_module(value); return value


checker = module('storage_read_queue_checker', ROOT / 'tools/check-a733-storage-read-supervised.py')
base = checker.supervisor.base


def plan(bundle):
    bundle = Path(bundle).resolve()
    prepared = checker.freeze_bundle(bundle)
    request = base.FEL_REQUEST_SECONDS
    regions = [base.transfer_timeout(length) for _, _, length in base.REGIONS]
    uploads = [request + base.transfer_timeout(2 * prepared['_sizes'][name]) for name, _ in base.uploads(prepared)]
    terms = {'version': request, 'backups': sum(regions), 'uploads': sum(uploads), 'watchdog_and_exe': 2 * request,
             'capture': CAPTURE_SECONDS,
             'refusal': base.transfer_timeout(128) + request +
                        sum(base.transfer_timeout(2 * length) for _, _, length in base.REGIONS),
             'setup': SETUP_SECONDS}
    return {'artifacts': [str(bundle / name) for name in ARTIFACTS],
            'timeout_seconds': sum(terms.values()), 'terms': terms,
            'preload_seconds': terms['version'] + terms['backups'] + terms['uploads']}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--bundle', type=Path, required=True)
    parser.add_argument('--format', choices=('json', 'shell'), default='json')
    args = parser.parse_args(argv)
    value = plan(args.bundle)
    if args.format == 'json': print(json.dumps(value, indent=1))
    else: print(value['timeout_seconds'], *(f'--artifact {path}' for path in value['artifacts']))
    return 0


if __name__ == '__main__':
    try: sys.exit(main())
    except (OSError, ValueError, KeyError) as error: sys.exit(f'Stopped: {error}')
