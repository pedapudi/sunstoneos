#!/usr/bin/env python3
"""Check CPU workload preparation against a preserved bundle and survey.

The current tool prepares a writable native session from read-only preserved
inputs without review, continuity, queue or survey records. Cases refuse an
existing output directory, prepare either listed port and reject changed native
payload bytes and invalid native session values. No device access occurs.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / 'tools/top-opp-workload-bind.py'


def bind(bundle, output, port='3-3', extra=()):
    command = [sys.executable, '-B', str(TOOL), '--bundle', str(bundle), '--port', port,
               '--output', str(output), *extra]
    return subprocess.run(command, capture_output=True, text=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', type=Path, required=True)
    parser.add_argument('--session', type=Path, help='preserved session containing c6-tw-survey/session.json')
    parser.add_argument('--survey', type=Path, help='a preserved completed launcher survey')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    def require(condition, message):
        if not condition:
            raise AssertionError(message)
    sys.path.insert(0, str(ROOT / 'tools'))
    from host_evidence import unique_directory
    out = unique_directory(args.output.resolve())
    # The scratch bundle deliberately has no binding.json, software/review
    # records or earlier board-session paths. Preserve read-only input modes.
    bundle = out / 'bundle'
    for name in ('build', 'template/prepared'):
        shutil.copytree(args.bundle / name, bundle / name, copy_function=shutil.copyfile)
    prepare = out / 'arbitrary-output-name'
    result = bind(bundle, prepare)
    require(result.returncode == 0, result.stdout + result.stderr)
    for name in ('command.json', 'offline-preparation.json', 'prepared/prepared.json',
                 'prepared/request.bin', 'prepared/contract.bin'):
        require((prepare / name).is_file(), name)
    require(all(os.access(path, os.W_OK) for path in prepare.rglob('*')), 'Prepared output is not writable')
    metadata = json.loads((prepare / 'offline-preparation.json').read_text())
    require(metadata['review_note'] is None and metadata['earlier_runs'] == [], 'External paperwork was required')
    require(metadata['survey'] is None, 'A previous survey was required')
    command = json.loads((prepare / 'command.json').read_text())
    require(command[:2] == ['env', 'SUNSTONE_USB_PORT=3-3'], 'Command does not identify its target port')
    require(str(ROOT / 'tools/run-native-startup-probe.py') in command, 'Command uses frozen host sources')
    require(all(word not in command for word in ('--claim', '--owner', '--database', '--software')), 'Command requires process records')
    require(str(prepare / 'run') in command, 'Run output is outside the prepared session')
    original = {p.relative_to(prepare): p.read_bytes() if p.is_file() else None for p in prepare.rglob('*')}
    result = bind(bundle, prepare)
    require(result.returncode != 0 and 'FileExistsError' in result.stderr, result.stdout + result.stderr)
    require({p.relative_to(prepare): p.read_bytes() if p.is_file() else None for p in prepare.rglob('*')} == original,
            'Refused preparation changed existing output')
    fresh = out / 'fresh-native-session'
    result = bind(bundle, fresh)
    require(result.returncode == 0, result.stdout + result.stderr)
    require((fresh / 'prepared/request.bin').read_bytes() != (prepare / 'prepared/request.bin').read_bytes(),
            'Fresh session contract was not generated')
    cases = ['no-release-records', 'arbitrary-output-path', 'existing-output-refused-without-changes', 'fresh-native-session']

    # Old notes may be kept without becoming launch prerequisites.
    result = bind(bundle, out / 'metadata-only', extra=('--survey', str(out / 'missing-survey.json'),
                  '--root-review', str(out / 'missing-review.json'), '--continuity', str(out / 'missing-run.json')))
    require(result.returncode == 0, result.stdout + result.stderr)
    cases.append('optional-process-metadata')

    def refused(name, expected, port='3-3'):
        target = out / name
        result = bind(bundle, target, port)
        require(result.returncode != 0 and expected in result.stderr + result.stdout, name + ': ' + result.stderr[-600:])
        require(not target.exists(), name + ' wrote output before refusing')
        cases.append(name)

    result = bind(bundle, out / 'second-port', port='3-1')
    require(result.returncode == 0, result.stdout + result.stderr)
    other = json.loads((out / 'second-port/command.json').read_text())
    require(other[:2] == ['env', 'SUNSTONE_USB_PORT=3-1'], 'Second board target was lost')
    cases.append('both-listed-ports')
    # Transfer integrity remains mandatory even when old review hashes are absent.
    segment = next((bundle / 'template/prepared').glob('segment-*.bin'))
    original_segment = segment.read_bytes()
    data = bytearray(original_segment)
    data[0] ^= 1
    segment.write_bytes(data)
    refused('changed-native-bytes', 'Prepared input changed')
    segment.write_bytes(original_segment)
    path = bundle / 'template/prepared/prepared.json'
    metadata = json.loads(path.read_text()); metadata['reservation'] = 0
    path.write_text(json.dumps(metadata))
    refused('invalid-native-reservation', 'Native reservation')
    (out / 'result.json').write_text(json.dumps({'pass': True, 'physical_access': False,
        'cases': cases, 'tool_sha256': hashlib.sha256(TOOL.read_bytes()).hexdigest()}, indent=2) + '\n')
    print(f'PASS CPU workload binding: {len(cases)} cases; direct ABI checks, both board targets, preserved evidence')


if __name__ == '__main__':
    main()
