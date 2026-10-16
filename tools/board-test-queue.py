#!/usr/bin/env python3
"""Store board-test proposals and serialize coordinator claims; never run tests."""

import argparse
from contextlib import contextmanager
import hashlib
import hmac
import json
import os
from pathlib import Path
import re
import secrets
import sqlite3
import sys
from datetime import datetime, timezone

DEFAULT = Path.home() / '.cache/sunstoneos/board-tests.sqlite3'
RECOVERY = 'independent-watchdog-rom-return'


def require(condition, message):
    if not condition:
        raise ValueError(message)


def text(value):
    return isinstance(value, str) and bool(value.strip())


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), allow_nan=False)


def digest(value):
    return hashlib.sha256(value.encode()).hexdigest()


def file_hash(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def artifact(value, verify=False):
    require(isinstance(value, dict) and set(value) == {'path', 'sha256'},
            'Artifact requires path and sha256 only.')
    require(text(value['path']) and Path(value['path']).is_absolute(),
            'Artifact path must be absolute.')
    require(isinstance(value['sha256'], str) and
            re.fullmatch('[0-9a-f]{64}', value['sha256']), 'Invalid SHA-256.')
    if verify:
        require(file_hash(value['path']) == value['sha256'],
                f"Artifact content changed: {value['path']}")


def submission(value):
    fields = {'component', 'kind', 'sources', 'artifacts', 'prerequisites',
              'expected_result', 'procedure', 'timeout_seconds',
              'stop_condition', 'evidence_directory'}
    require(isinstance(value, dict) and fields <= value.keys() and
            value.keys() <= fields | {'retry_of', 'retry_reason'},
            'Submission fields do not match the documented schema.')
    for name in ('component', 'expected_result', 'stop_condition'):
        require(text(value[name]), f'{name} must be nonempty text.')
    require(value['kind'] in ('peripheral', 'recovery'), 'Invalid test kind.')
    require(type(value['timeout_seconds']) is int and
            1 <= value['timeout_seconds'] <= 3600,
            'Timeout must be 1..3600 seconds.')
    require(isinstance(value['procedure'], list) and value['procedure'] and
            all(text(step) for step in value['procedure']),
            'Procedure must contain nonempty steps.')
    require(text(value['evidence_directory']) and
            Path(value['evidence_directory']).is_absolute(),
            'Evidence directory must be absolute.')
    prereqs = value['prerequisites']
    require(isinstance(prereqs, list) and prereqs and all(text(p) for p in prereqs)
            and len(set(prereqs)) == len(prereqs),
            'Prerequisites must be distinct nonempty strings.')
    require(value['kind'] != 'peripheral' or RECOVERY in prereqs,
            f'Peripheral tests require {RECOVERY}.')
    require(isinstance(value['sources'], list) and value['sources'],
            'At least one source revision is required.')
    for source in value['sources']:
        require(isinstance(source, dict) and set(source) == {'repository', 'revision'}
                and text(source['repository']) and
                isinstance(source['revision'], str) and
                re.fullmatch('[0-9a-f]{40}', source['revision']),
                'Source requires repository and full 40-digit Git revision.')
    require(isinstance(value['artifacts'], list) and value['artifacts'],
            'At least one hashed artifact is required.')
    for item in value['artifacts']:
        artifact(item)
    if 'retry_of' in value or 'retry_reason' in value:
        require(isinstance(value.get('retry_of'), str) and
                re.fullmatch('[0-9a-f]{64}', value['retry_of']) and
                text(value.get('retry_reason')),
                'Retry requires original ID and a reason based on changed conditions.')
    return canonical(value)


SCHEMA = '''
CREATE TABLE settings(key TEXT PRIMARY KEY, value TEXT NOT NULL);
CREATE TABLE tests(
 id TEXT PRIMARY KEY, submission TEXT NOT NULL, state TEXT NOT NULL
 CHECK(state IN ('pending','ready','claimed','passed','failed','blocked')),
 review TEXT, owner TEXT, created TEXT NOT NULL);
CREATE UNIQUE INDEX single_claim ON tests(state) WHERE state='claimed';
CREATE TRIGGER immutable_submission BEFORE UPDATE OF id,submission,created ON tests
 BEGIN SELECT RAISE(ABORT, 'Submission is immutable'); END;
CREATE TABLE events(sequence INTEGER PRIMARY KEY, test_id TEXT, time TEXT NOT NULL,
 action TEXT NOT NULL, details TEXT NOT NULL);
'''


def event(db, test_id, action, details):
    db.execute('INSERT INTO events(test_id,time,action,details) VALUES(?,?,?,?)',
               (test_id, datetime.now(timezone.utc).isoformat(), action, canonical(details)))


@contextmanager
def database(path):
    require(path.is_file(), 'Queue does not exist; coordinator must initialize it.')
    db = sqlite3.connect(f'{path.resolve().as_uri()}?mode=rw', uri=True, timeout=10)
    db.row_factory = sqlite3.Row
    try:
        db.execute('BEGIN IMMEDIATE')
        yield db
        db.commit()
    except BaseException:
        db.rollback()
        raise
    finally:
        db.close()


def authorize(db, token_file):
    require(token_file is not None, 'Coordinator token file is required.')
    token = token_file.read_text().strip()
    expected = db.execute("SELECT value FROM settings WHERE key='coordinator'").fetchone()[0]
    require(hmac.compare_digest(digest(token), expected), 'Invalid coordinator token.')


def get(db, test_id):
    row = db.execute('SELECT * FROM tests WHERE id=?', (test_id,)).fetchone()
    require(row is not None, 'Unknown submission ID.')
    return row


def verify_review(row):
    spec = json.loads(row['submission'])
    for item in spec['artifacts']:
        artifact(item, verify=True)
    review = json.loads(row['review'])
    for item in review['prerequisites'].values():
        artifact(item, verify=True)


def load(path):
    return json.loads(path.read_text())


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--database', type=Path, default=DEFAULT)
    parser.add_argument('--token-file', type=Path)
    commands = parser.add_subparsers(dest='command', required=True)
    commands.add_parser('init')
    submit = commands.add_parser('submit')
    submit.add_argument('specification', type=Path)
    commands.add_parser('list')
    show = commands.add_parser('show')
    show.add_argument('id')
    review = commands.add_parser('review')
    review.add_argument('id')
    review.add_argument('evidence', type=Path,
                        help='JSON mapping prerequisite names to path/sha256 records')
    review.add_argument('--note', required=True)
    claim = commands.add_parser('claim')
    claim.add_argument('id')
    claim.add_argument('--owner', required=True)
    finish = commands.add_parser('finish')
    finish.add_argument('id')
    finish.add_argument('--result', choices=('passed', 'failed', 'blocked'), required=True)
    finish.add_argument('--evidence', type=Path, required=True)
    finish.add_argument('--note', required=True)
    finish.add_argument('--confirm-stopped', action='store_true')
    block = commands.add_parser('block')
    block.add_argument('id')
    block.add_argument('--evidence', type=Path, required=True)
    block.add_argument('--note', required=True)
    resume = commands.add_parser('resume')
    resume.add_argument('--evidence', type=Path, required=True)
    resume.add_argument('--note', required=True)
    args = parser.parse_args(argv)
    path = args.database.expanduser().resolve()
    if args.command == 'init':
        require(args.token_file is not None, 'Choose an exclusive coordinator token file.')
        path.parent.mkdir(parents=True, exist_ok=True)
        # Exclusive creation prevents concurrent initialization from replacing ownership.
        descriptor = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
        os.close(descriptor)
        try:
            token = secrets.token_hex(32)
            descriptor = os.open(args.token_file, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
            with os.fdopen(descriptor, 'w') as stream:
                stream.write(token + '\n')
            db = sqlite3.connect(path)
            try:
                db.executescript(SCHEMA)
                db.execute('INSERT INTO settings VALUES(?,?)', ('coordinator', digest(token)))
                db.execute('INSERT INTO settings VALUES(?,?)', ('halted', ''))
                event(db, None, 'initialized', {})
                db.commit()
            finally:
                db.close()
        except BaseException:
            path.unlink()
            raise
        return {'database': str(path), 'token_file': str(args.token_file)}
    with database(path) as db:
        if args.command == 'submit':
            value = load(args.specification)
            body = submission(value)
            test_id = digest(body)
            if 'retry_of' in value:
                previous = get(db, value['retry_of'])
                require(previous['state'] in ('failed', 'blocked'),
                        'Retry requires a failed or blocked original submission.')
            old = db.execute('SELECT id FROM tests WHERE id=?', (test_id,)).fetchone()
            if old is None:
                db.execute('INSERT INTO tests VALUES(?,?,?,NULL,NULL,?)',
                           (test_id, body, 'pending', datetime.now(timezone.utc).isoformat()))
                event(db, test_id, 'submitted', {})
            return {'id': test_id, 'state': get(db, test_id)['state']}
        if args.command == 'list':
            return {'halted': db.execute("SELECT value FROM settings WHERE key='halted'").fetchone()[0],
                    'tests': [{key: row[key] for key in ('id', 'state', 'owner', 'created')} |
                              {'component': json.loads(row['submission'])['component'],
                               'prerequisites': json.loads(row['submission'])['prerequisites']}
                              for row in db.execute('SELECT id,submission,state,owner,created FROM tests ORDER BY created,id')]}
        if args.command == 'show':
            row = dict(get(db, args.id))
            row['submission'] = json.loads(row['submission'])
            row['review'] = json.loads(row['review']) if row['review'] else None
            row['events'] = [dict(event_row) for event_row in db.execute(
                'SELECT * FROM events WHERE test_id=? ORDER BY sequence', (args.id,))]
            return row
        authorize(db, args.token_file)
        if args.command == 'resume':
            require(text(args.note), 'Recovery note is required.')
            require(not db.execute("SELECT 1 FROM tests WHERE state='claimed'").fetchone(),
                    'An active claim prevents resuming the queue.')
            require(db.execute("SELECT value FROM settings WHERE key='halted'").fetchone()[0],
                    'Queue is not halted.')
            evidence = {'path': str(args.evidence.resolve()), 'sha256': file_hash(args.evidence)}
            event(db, None, 'resumed', {'note': args.note, 'evidence': evidence})
            db.execute("UPDATE settings SET value='' WHERE key='halted'")
            return {'halted': ''}
        row = get(db, args.id)
        if args.command == 'block':
            require(row['state'] in ('pending', 'ready'), 'Only unclaimed tests can be blocked.')
            require(text(args.note), 'Blocker note is required.')
            evidence = {'path': str(args.evidence.resolve()), 'sha256': file_hash(args.evidence)}
            db.execute("UPDATE tests SET state='blocked' WHERE id=?", (args.id,))
            event(db, args.id, 'blocked', {'note': args.note, 'evidence': evidence})
        elif args.command == 'review':
            require(row['state'] in ('pending', 'ready'), 'Only pending or ready tests can be reviewed.')
            require(text(args.note), 'Review note is required.')
            prereqs = load(args.evidence)
            require(isinstance(prereqs, dict) and
                    set(prereqs) == set(json.loads(row['submission'])['prerequisites']),
                    'Evidence must cover every prerequisite, with no extra names.')
            for item in prereqs.values():
                artifact(item, verify=True)
            for item in json.loads(row['submission'])['artifacts']:
                artifact(item, verify=True)
            review = {'note': args.note, 'prerequisites': prereqs}
            db.execute("UPDATE tests SET state='ready',review=? WHERE id=?", (canonical(review), args.id))
            event(db, args.id, 'reviewed', review)
        elif args.command == 'claim':
            require(text(args.owner), 'Owner must be named.')
            require(row['state'] == 'ready', 'Only reviewed ready tests can be claimed.')
            require(not db.execute("SELECT value FROM settings WHERE key='halted'").fetchone()[0],
                    'Queue is halted after failure; coordinator recovery review is required.')
            require(not db.execute("SELECT 1 FROM tests WHERE state='claimed'").fetchone(),
                    'Another test has an active claim.')
            verify_review(row)
            db.execute("UPDATE tests SET state='claimed',owner=? WHERE id=?", (args.owner, args.id))
            event(db, args.id, 'claimed', {'owner': args.owner})
        elif args.command == 'finish':
            require(row['state'] == 'claimed', 'Only an active claim can be finished.')
            require(args.confirm_stopped, 'Confirm all associated board processes stopped before releasing claim.')
            require(text(args.note), 'Result note is required.')
            evidence = {'path': str(args.evidence.resolve()), 'sha256': file_hash(args.evidence)}
            db.execute('UPDATE tests SET state=? WHERE id=?', (args.result, args.id))
            event(db, args.id, 'finished', {'result': args.result, 'note': args.note, 'evidence': evidence})
            if args.result == 'failed':
                db.execute("UPDATE settings SET value=? WHERE key='halted'", (args.id,))
        return {'id': args.id, 'state': get(db, args.id)['state']}


if __name__ == '__main__':
    try:
        print(json.dumps(main(), indent=2))
    except (ValueError, OSError, sqlite3.Error) as error:
        print(f'board-test-queue: {error}', file=sys.stderr)
        sys.exit(1)
