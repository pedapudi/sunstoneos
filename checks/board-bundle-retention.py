#!/usr/bin/env python3
"""Check a frozen bundle wrapper's timeout without queue or device access."""
import argparse
import hashlib
import json
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile
from types import SimpleNamespace
from unittest.mock import patch


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--wrapper', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    waits, queue = [], []

    class Child:
        pid = 733

        def __init__(self, command, **options):
            assert command == ['MODEL-never-execute']
            assert 'timeout' not in options
            options['stdout'].write('MODEL retained runner\n')
            options['stdout'].flush()

        def wait(self, timeout=None):
            waits.append(timeout)
            if len(waits) == 1:
                raise subprocess.TimeoutExpired('MODEL', timeout)
            # Represent a later coordinator recovery ending the retained child.
            return 1

        def kill(self):
            raise AssertionError('Observation timeout killed the retained runner')

    def command(argv, **unused):
        if argv[0] == 'git':
            output = 'MODEL-revision\n'
        else:
            operation = argv[6]
            queue.append(operation)
            assert operation in ('submit', 'review', 'claim')
            output = '{"id": "MODEL-test"}' if operation == 'submit' else '{}'
        return SimpleNamespace(returncode=0, stdout=output, stderr='')

    with tempfile.TemporaryDirectory(prefix='sunstone-retention-') as directory:
        root = Path(directory)
        identity = root/'.cache/sunstoneos/evidence/rom-identity-20261023/port-3-1/session.json'
        identity.parent.mkdir(parents=True)
        identity.write_text('{}')
        recovery = root/'MODEL-recovery.json'
        recovery.write_text('{}')
        argv = [str(args.wrapper), '3-1', str(root/'runtime'), '300', 'MODEL', 'MODEL',
                '--prerequisite', 'supervised-ram-recovery='+str(recovery),
                '--', 'MODEL-never-execute']
        with patch.object(sys, 'argv', argv), patch.object(Path, 'home', return_value=root), \
             patch.object(subprocess, 'run', command), patch.object(subprocess, 'Popen', Child):
            try:
                runpy.run_path(str(args.wrapper), run_name='__main__')
            except SystemExit as error:
                assert error.code == 1
            else:
                raise AssertionError('Retained failure reported success')
        record = json.loads((root/'runtime-setup/runner.json').read_text())
        assert waits == [300, None]
        assert record['runner_pid'] == 733 and record['observation_expired'] is True
        assert queue == ['submit', 'review', 'claim']
    args.output.write_text(json.dumps({
        'physical_access': False, 'queue_access': False, 'pass': True,
        'wrapper_sha256': hashlib.sha256(args.wrapper.read_bytes()).hexdigest(),
        'check_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        'wait_seconds': waits, 'claim_finished': False,
        'limits': 'Actual wrapper; modeled child and queue commands. No physical lock qualification.'
    }, indent=2)+'\n')


if __name__ == '__main__':
    main()
