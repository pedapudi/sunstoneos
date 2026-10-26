#!/usr/bin/env python3
"""Validate the frozen exact-sum image and its returning-kernel capture."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import sys

EXPECTED = ('COMPLETE GPU exact sums: 2 jobs, tags and sums match, context cleaned up, firmware stopped, OFF and '
            'provider parked')


def validate_result(result, log):
    if tuple(result.get(k) for k in ('magic', 'stage', 'status')) != (0x534b4552, 5, 0):
        raise ValueError('exact-sum terminal status')
    lines = log.splitlines()
    if any(line.startswith('FAIL ') for line in lines) or lines.count(EXPECTED) != 1:
        raise ValueError('exact-sum completion report')

    def fields(prefix, expected):
        found = [line[len(prefix):] for line in lines if line.startswith(prefix)]
        if len(found) != 1:
            raise ValueError('missing or duplicate ' + prefix.strip())
        values = {}
        for token in found[0].split():
            match = re.fullmatch(r'([a-z0-9_]+)=(0x[0-9a-f]+|[0-9]+)', token)
            if not match or match[1] in values:
                raise ValueError('malformed ' + prefix.strip())
            values[match[1]] = int(match[2], 16 if match[2].startswith('0x') else 10)
        if any(values.get(k) != v for k, v in expected.items()):
            raise ValueError('incorrect ' + prefix.strip())
        return values

    sums = fields('GPU exact sums ', {'jobs': 2, 'sums': 1122, 'bad_tags': 0, 'bad_sums': 0, 'bad_guards': 0,
                                    'minimum': 0xffffffff7f010200, 'maximum': 0x80fefe00})
    fields('GPU FWJ job ', {'complete': 1, 'outcome': 1, 'stage': 4, 'phase': 7, 'error': 0,
                           'client': 2, 'client_error': 0, 'timeline': 2, 'jobs': 2})
    fields('GPU FWJ events ', {'control': 4, 'control_error': 0, 'n': 0, 'resets': 0, 'unexpected': 0})
    fields('GPU FWJ cleanup ', {'status': 1, 'events': 0})
    fields('GPU FWS stop ', {'result': 0, 'phase': 10, 'error': 0, 'unacknowledged': 0})
    fields('GPU FWS power ', {'ack_idle': 1, 'ack_off': 1, 'state_off': 0, 'isolated': 1})
    return {'accepted': True, 'jobs': sums['jobs'], 'sums': sums['sums']}


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--mode', default='kernel')
    parser.add_argument('--expect', default=EXPECTED)
    args, _ = parser.parse_known_args(argv)
    bundle = Path(__file__).resolve().parents[2]
    record = json.loads((bundle / 'build.json').read_text())
    if record.get('variant') != 'fw-mimus' or record.get('expect') != EXPECTED or \
            hashlib.sha256(args.image.read_bytes()).hexdigest() != record['binary_sha256']:
        raise ValueError('exact-sum image identity')
    if args.mode != 'kernel' or args.expect != EXPECTED or any(
            value.split('=')[0] in ('--controlled', '--ram-input', '--ram-input-present') for value in argv):
        raise ValueError('exact-sum host mode')
    path = Path(__file__).with_name('returning-kernel.py')
    spec = importlib.util.spec_from_file_location('exact_sum_returning_kernel', path)
    runner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runner)
    return runner.main(list(argv) + ['--expect', EXPECTED], validate_result=validate_result)


if __name__ == '__main__':
    main()
