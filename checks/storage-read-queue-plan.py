#!/usr/bin/env python3
"""Check the queue artifacts and host timeout of a supervised SD0 read run.

usage: storage-read-queue-plan.py --bundle PREPARED --software SOFTWARE_JSON
                                  --output DIR [--session RECORDED_SESSION_JSON]

Builds the queue row that common/bundle-run.py writes for the runbook's sd()
in a scratch database with the board queue's own schema, then calls the
checker's own claim check, check-a733-smp-supervised.py device_claim:

- without --artifact, the submission covers none of prepared.json, image.elf
  and launch.bin, and the claim check must refuse it ("Queue does not cover
  the initially admitted prepared image"), as it refused lifetime A's first
  submission on 2026-10-25;
- with the three --artifact paths from tools/storage-read-queue-plan.py, the
  claim check must accept it.

With --session, the per-request bounds the checker recorded before its exe
request must sum to the plan's preload bound plus the final watchdog read,
so the plan's timeout derives from the bounds the checker applies. No board,
USB or queue access; the scratch database is under --output.
"""
import argparse
import hashlib
import importlib.util
import json
import sqlite3
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec); spec.loader.exec_module(value); return value


tool = module('storage_read_queue_plan', ROOT / 'tools/storage-read-queue-plan.py')
queue = module('storage_read_queue_schema', ROOT / 'tools/board-test-queue.py')
checker = tool.checker


def ref(path):
    path = Path(path).resolve(); return {'path': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}


def claim(database, bundle, prepared, software, artifacts):
    """One claimed row as bundle-run.py records it for sd(); returns the refusal text or None."""
    runner = [checker.__file__, str(bundle)]
    submission = {'artifacts': [ref(a) for a in runner if Path(a).is_file()] + [ref(a) for a in artifacts],
                  'prerequisites': sorted(checker.PREREQUISITES)}
    review = {'prerequisites': {'supervised-ram-recovery': ref(bundle / 'admission.json'),
                                checker.SOFTWARE_PREREQUISITE: ref(software)}}
    db = sqlite3.connect(database)
    db.executescript(queue.SCHEMA)
    db.execute("INSERT INTO settings VALUES('halted','')")
    db.execute("INSERT INTO tests VALUES('model-claim',?,'claimed',?,'coordinator','2026-10-25')",
               (json.dumps(submission), json.dumps(review)))
    db.commit(); db.close()
    try:
        checker.supervisor.device_claim(database, 'model-claim', 'coordinator', bundle, prepared, 'SD0 read',
                                        checker.SOFTWARE_PREREQUISITE, checker.PREREQUISITES,
                                        checker.reference_bytes, checker.validate_software)
    except ValueError as error: return str(error)
    return None


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ('bundle', 'software', 'output'): parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--session', type=Path)
    args = parser.parse_args(argv)
    args.output.mkdir(parents=True, exist_ok=False)
    bundle = args.bundle.resolve(); prepared = checker.freeze_bundle(bundle)
    value = tool.plan(bundle)
    refused = claim(args.output / 'queue-without.sqlite3', bundle, prepared, args.software.resolve(), [])
    assert refused and 'Queue does not cover the initially admitted prepared image' in refused, refused
    accepted = claim(args.output / 'queue-with.sqlite3', bundle, prepared, args.software.resolve(), value['artifacts'])
    assert accepted is None, accepted
    terms = value['terms']
    assert value['timeout_seconds'] == sum(terms.values()) and terms['capture'] == 59
    result = {'passed': True, 'physical_access': False, 'bundle': str(bundle), 'plan': value,
              'without_artifacts': refused, 'with_artifacts': 'accepted'}
    if args.session:
        record = json.loads(args.session.read_text())
        before = []
        for entry in record['transactions']:
            if entry['arguments'][0] == 'exe': break
            before.append(entry['timeout_seconds'])
        assert sum(before) == value['preload_seconds'] + tool.base.FEL_REQUEST_SECONDS, (sum(before), value)
        result['recorded_session'] = {'path': str(args.session.resolve()), 'pre_exe_requests': len(before),
                                      'pre_exe_bound_seconds': sum(before)}
    (args.output / 'result.json').write_text(json.dumps(result, indent=1) + '\n')
    print(f"PASS SD0 queue artifacts and host timeout {value['timeout_seconds']} s")
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
