#!/usr/bin/env python3
"""Check board-test scheduling with concurrent host processes and no USB access."""
import hashlib
import importlib.util
import json
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile

TOOL = Path(__file__).resolve().parents[1] / 'tools/board-test-queue.py'
BASE = Path(__file__).resolve().parents[1] / 'tools/check-a733-smp.py'


def port_refused(base, db, port):
    try:
        base.require_queue_port(db, port)
    except ValueError:
        return True
    return False


def main():
    with tempfile.TemporaryDirectory(prefix='sunstoneos-queue-') as directory:
        root = Path(directory)
        db, token = root / 'queue.sqlite3', root / 'coordinator'
        base = [sys.executable, str(TOOL), '--database', str(db)]
        admin = base + ['--token-file', str(token)]

        def run(args, success=True):
            result = subprocess.run(args, capture_output=True, text=True)
            assert (result.returncode == 0) == success, (args, result.stdout, result.stderr)
            return json.loads(result.stdout) if success else result.stderr

        def record(path):
            return {'path': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}

        run(admin + ['init'], False)
        run(admin + ['init', '--port', '../3-3'], False)
        assert not db.exists() and not token.exists()
        run(admin + ['init', '--port', '3-3'])
        original = token.read_bytes()
        run(admin + ['init', '--port', '3-3'], False)
        assert token.read_bytes() == original
        assert token.stat().st_mode & 0o777 == 0o600
        assert run(base + ['list'])['port'] == '3-3'
        # Supervised runners compare the queue's port with the locked board port.
        spec = importlib.util.spec_from_file_location('queue_check_base', BASE)
        smp = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(smp)
        assert port_refused(smp, db, '3-1') and not port_refused(smp, db, '3-3')
        run(admin + ['bind-port', '--port', '3-1'], False)
        # A queue created before binding records its port once, by the coordinator.
        legacy, legacy_token = root / 'legacy.sqlite3', root / 'legacy-coordinator'
        legacy_admin = [sys.executable, str(TOOL), '--database', str(legacy),
                        '--token-file', str(legacy_token)]
        run(legacy_admin + ['init', '--port', '3-1'])
        with sqlite3.connect(legacy) as connection:
            connection.execute("DELETE FROM settings WHERE key='port'")
        assert port_refused(smp, legacy, '3-1')
        run([sys.executable, str(TOOL), '--database', str(legacy), 'bind-port', '--port', '3-1'], False)
        run(legacy_admin + ['bind-port', '--port', '3-1'])
        run(legacy_admin + ['bind-port', '--port', '3-3'], False)
        assert not port_refused(smp, legacy, '3-1') and port_refused(smp, legacy, '3-3')
        binary = root / 'image.bin'
        binary.write_bytes(b'bounded fake test artifact')
        proof = root / 'proof.json'
        proof.write_text('{"host_fixture": true}')
        spec = {'component': 'GPU host fixture', 'kind': 'peripheral',
                'sources': [{'repository': '/example/sunstoneos', 'revision': 'a' * 40}],
                'artifacts': [record(binary)],
                'prerequisites': ['independent-watchdog-rom-return', 'host-check'],
                'expected_result': 'Known firmware result and ROM return.',
                'procedure': ['Review identity.', 'Execute one bounded operation.'],
                'timeout_seconds': 15, 'stop_condition': 'First transport failure.',
                'evidence_directory': str(root / 'evidence')}
        spec_path = root / 'spec.json'
        spec_path.write_text(json.dumps(spec))
        first = run(base + ['submit', str(spec_path)])['id']
        assert run(base + ['submit', str(spec_path)])['id'] == first
        spec['component'] = 'NPU host fixture'
        spec_path.write_text(json.dumps(spec))
        second = run(base + ['submit', str(spec_path)])['id']
        spec['prerequisites'] = ['host-check']
        spec_path.write_text(json.dumps(spec))
        run(base + ['submit', str(spec_path)], False)
        run(base + ['claim', first, '--owner', 'worker'], False)
        run(admin + ['claim', first, '--owner', 'coordinator'], False)
        review = root / 'review.json'
        review.write_text(json.dumps({'host-check': record(proof)}))
        run(admin + ['review', first, str(review), '--note', 'Fixture'], False)
        review.write_text(json.dumps({name: record(proof) for name in
                                      ('host-check', 'independent-watchdog-rom-return')}))
        for test_id in (first, second):
            run(admin + ['review', test_id, str(review), '--note', 'Host fixture only'])
        # Content mutation invalidates readiness without changing the immutable record.
        binary.write_bytes(b'changed')
        run(admin + ['claim', first, '--owner', 'coordinator'], False)
        binary.write_bytes(b'bounded fake test artifact')
        # Two independent coordinator invocations contend for the one active slot.
        processes = [subprocess.Popen(admin + ['claim', test_id, '--owner', test_id],
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                     for test_id in (first, second)]
        outputs = [process.communicate(timeout=15) for process in processes]
        assert sorted(process.returncode for process in processes) == [0, 1], outputs
        winner_index = next(index for index, process in enumerate(processes)
                            if process.returncode == 0)
        winner, waiting = ((first, second) if winner_index == 0 else (second, first))
        # The short-lived claiming CLI has exited. Its claim must still block others.
        run(admin + ['claim', waiting, '--owner', 'another'], False)
        run(admin + ['finish', winner, '--result', 'passed', '--evidence', str(proof),
                     '--note', 'Without stopped assertion'], False)
        run(admin + ['finish', winner, '--result', 'failed', '--evidence', str(proof),
                     '--note', 'Simulated transport failure', '--confirm-stopped'])
        run(admin + ['claim', waiting, '--owner', 'another'], False)
        assert run(base + ['list'])['halted'] == winner
        run(admin + ['resume', '--evidence', str(proof), '--note', 'Simulated recovery review'])
        run(admin + ['claim', waiting, '--owner', 'coordinator'])
        run(admin + ['finish', waiting, '--result', 'passed', '--evidence', str(proof),
                     '--note', 'Host fixture complete', '--confirm-stopped'])
        run(admin + ['claim', winner, '--owner', 'coordinator'], False)
        spec['prerequisites'].append('independent-watchdog-rom-return')
        spec.update(retry_of=winner, retry_reason='Changed recovery arrangement for host fixture.')
        spec_path.write_text(json.dumps(spec))
        retry = run(base + ['submit', str(spec_path)])['id']
        run(admin + ['block', retry, '--evidence', str(proof), '--note', 'Missing fixture'])
        assert run(base + ['show', retry])['state'] == 'blocked'
        # The database protects immutable submission fields even through direct SQL.
        with sqlite3.connect(db) as connection:
            try:
                connection.execute("UPDATE tests SET submission='{}' WHERE id=?", (first,))
            except sqlite3.IntegrityError:
                pass
            else:
                raise AssertionError('Submission was mutable')
        for test_id in (first, second, retry):
            saved = run(base + ['show', test_id])
            assert saved['events'] and saved['submission']['sources'][0]['revision'] == 'a' * 40
        print('PASS: port binding, submission identities, authorization, prerequisite review, artifact changes,')
        print('concurrent claims, retained ownership, failure halt, recovery review, terminal history.')


if __name__ == '__main__':
    main()
