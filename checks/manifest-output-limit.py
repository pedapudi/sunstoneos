#!/usr/bin/env python3
"""Check that the manifest tool refuses an output_limit smaller than the failure record.

A failure return publishes a struct ss_failure (include/sunstone_session.h) in the application's output, so a
manifest whose output_limit is smaller cannot carry it. The tool takes the record size from the static assertion in
that header. The check also reads every in-tree manifest and requires the limit to be at least that size."""
import argparse
import importlib.util
import json
from pathlib import Path
import sys
import tempfile

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]


def main():
    argparse.ArgumentParser(description=__doc__).parse_args()
    spec = importlib.util.spec_from_file_location('manifest_tool', ROOT / 'tools/sunstone-manifest.py')
    tool = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(tool)
    record = tool.FAILURE_RECORD_BYTES
    assert record == 48, 'the failure record is %d bytes' % record
    base = {'name': 'limit', 'devices': {'required': [], 'optional': []},
            'cpu': {'cluster': 'a55', 'workers': 0, 'priority': 2, 'stack_bytes': 4096},
            'regions': [], 'deadlines': {'return_reserve_us': 1000000, 'progress_window_us': 1000000}}

    def load(limit):
        raw = dict(base)
        if limit is not None:
            raw['output_limit'] = limit
        with tempfile.NamedTemporaryFile('w', suffix='.json') as handle:
            json.dump(raw, handle)
            handle.flush()
            return tool.load(handle.name)

    for limit in (None, 0, 8, 16, record - 1):
        try:
            load(limit)
        except SystemExit as error:
            assert str(record) in str(error) and 'failure record' in str(error), 'the refusal does not name the record size: %s' % error
            continue
        raise SystemExit('output_limit %s was accepted' % limit)
    assert load(record)['output_limit'] == record and load(record + 16)['output_limit'] == record + 16
    checked = 0
    for path in sorted([*ROOT.glob('apps/*/*manifest*.json'), *ROOT.glob('checks/**/*manifest*.json')]):
        try:
            raw = json.loads(path.read_text())
        except ValueError:
            continue
        if isinstance(raw, dict) and 'deadlines' in raw:
            assert raw.get('output_limit', 0) >= record, '%s: output_limit %s is smaller than the %d-byte failure record' % (
                path.relative_to(ROOT), raw.get('output_limit'), record)
            checked += 1
    print('manifest-output-limit: passed (%d manifests)' % checked)


if __name__ == '__main__':
    main()
