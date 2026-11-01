#!/usr/bin/env python3
"""Check optional SD0 timeout reporting without a queue database or software seal."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec); spec.loader.exec_module(value); return value


tool = module('storage_read_queue_plan', ROOT / 'tools/storage-read-queue-plan.py')
checker = tool.checker


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ('bundle', 'output'): parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--software', type=Path, help=argparse.SUPPRESS)
    parser.add_argument('--session', type=Path)
    args = parser.parse_args(argv)
    args.output.mkdir(parents=True, exist_ok=False)
    bundle = args.bundle.resolve(); prepared = checker.freeze_bundle(bundle)
    value = tool.plan(bundle)
    terms = value['terms']
    assert value['timeout_seconds'] == sum(terms.values()) and terms['capture'] == 59
    result = {'passed': True, 'physical_access': False, 'bundle': str(bundle), 'plan': value,
              'queue_records_required': False}
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
    print(f"PASS SD0 optional artifacts and host timeout {value['timeout_seconds']} s")
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
