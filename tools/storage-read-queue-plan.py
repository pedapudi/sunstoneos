#!/usr/bin/env python3
"""Print the queue artifacts and host timeout of one supervised SD0 read run.

usage: storage-read-queue-plan.py --bundle PREPARED [--format json|shell]

check-a733-storage-read-supervised.py refuses a claim whose queue submission
does not cover the bundle's prepared.json, image.elf and launch.bin with the
hashes it admitted (check-a733-smp-supervised.py, device_claim). This tool
names those three files after checking the same hashes, so the runbook can
pass them to bundle-run.py as --artifact.

The timeout is the sum of the bounds the checker itself applies, so a run
that stays within every bound is never cut short by the queue:

- the ROM version request;
- the five SRAM backups and, for each upload in preload order, the watchdog
  read and the write with its readback, each bounded by fel_timeout;
- the watchdog read before entry and the exe request;
- the 59-second capture window for a new native instance;
- the refusal path: the report read, the watchdog read and the restoring
  write and readback of the five SRAM regions;
- 60 seconds for the checker's start, claim checks and USB health check
  (the run of 2026-10-25 spent 10 seconds between its version request and
  its first backup).

After the capture window every outcome except a clean refusal holds the
port lock and queue claim until the host timeout, so the timeout also ends
that hold; it never ends a FEL request early. No board, USB or queue access.
"""
import argparse
import hashlib
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
    expected = {'prepared.json': prepared['_prepared_sha256'], 'image.elf': prepared['files']['image.elf'],
                'launch.bin': prepared['files']['launch.bin']}
    for name, value in expected.items():
        base.require(hashlib.sha256((bundle / name).read_bytes()).hexdigest() == value,
                     f'{name} differs from the admitted bundle.')
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
