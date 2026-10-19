#!/usr/bin/env python3
"""Capture one supervised board session under an existing coordinator claim."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import sqlite3
import struct
import sys
import time

ROOT = Path(__file__).resolve().parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


supervisor = module('board_capture_supervisor', ROOT / 'tools/check-a733-smp-supervised.py')
preparation = module('board_capture_preparation', ROOT / 'apps/board/prepare.py')
decoder = module('board_capture_decoder', ROOT / 'apps/board/observe.py')
require, digest, artifact = supervisor.require, supervisor.digest, supervisor.artifact
PREREQUISITES = {'supervised-ram-recovery', 'board-supervised-software'}


def bundle_bytes(bundle, prepared, name):
    require(Path(name).name == name, 'Invalid bundle filename.')
    data = (bundle / name).read_bytes()
    require(hashlib.sha256(data).hexdigest() == prepared['files'].get(name),
            'Prepared input changed before use: ' + name)
    return data


def capture(exchange, session, profile, evidence, record, save, deadline,
            now=time.monotonic, pause=time.sleep):
    """Only complete pending replies permit another query; never retry a transfer."""
    workload = preparation.session_check(session)
    for sequence in range(1, 241):
        require(now() < deadline, 'Board capture deadline expired.')
        timeout = min(500, max(1, int((deadline - now()) * 1000)))
        frame = exchange(b'\0', timeout)
        path = evidence / f'board-{sequence:03}.bin'
        supervisor.durable_frame(path, frame)
        entry = {'path': path.name, 'sha256': hashlib.sha256(frame).hexdigest()}
        record.setdefault('board_frames', []).append(entry)
        save()
        result = decoder.decode(frame, session, profile)
        sector = result.pop('sector')
        entry.update(result)
        require(now() < deadline, 'Board response arrived after capture deadline.')
        if result['usb_state'] == 2 or result['radio_state'] == 3:
            record['board_result'] = 'observed-failure'
            save()
            return result
        host_done = result['usb_state'] == (1 if workload < 2 or workload in (4,6) else 3)
        radio_done = result['radio_state'] == (2 if workload & 1 else 0)
        if host_done and radio_done:
            if sector is not None:
                sector_path = evidence / 'sector.bin'
                supervisor.durable_frame(sector_path, sector)
                record['copied_sector'] = {'path': sector_path.name, 'sha256': digest(sector_path)}
            if result.get('kind') in ('hid-input','storage-and-hid'):
                hid_path=evidence/'hid-input.json'
                supervisor.durable_frame(hid_path,(json.dumps(result,sort_keys=True)+'\n').encode())
                record['copied_hid_input']={'path':hid_path.name,'sha256':digest(hid_path)}
            record['board_result'] = 'observed-success'
            record['durable_result'] = entry.copy()
            record['workload'] = workload
            try:
                save()
            except BaseException:
                # A later failure-record save must not publish this incomplete
                # success transition. The exact received frame stays preserved.
                record['board_result'] = 'unobserved'
                record.pop('durable_result', None)
                raise
            return result
        save()
        pause(min(.25, max(0, deadline - now())))
    raise supervisor.CaptureWindowExpired('Board exhausted its bounded pending-query budget.')


def capture_device(device, same, deadline, session, profile, evidence, record, save):
    def exchange(packet, timeout):
        same()
        require(device.write(0x02, packet, timeout=timeout) == 1, 'Incomplete board request.')
        same()
        remaining = int((deadline - time.monotonic()) * 1000)
        require(remaining > 0, 'Board capture deadline expired after write.')
        return bytes(device.read(0x81, 1160, timeout=min(timeout, remaining)))
    return capture(exchange, session, profile, evidence, record, save, deadline)


def source_files():
    return [('board-host-tool.py', Path(__file__)),
            ('board-preparation.py', Path(preparation.__file__)),
            ('board-decoder.py', Path(decoder.__file__))]


def validate_software(value, bundle, prepared):
    require(value.get('schema') == 'a733-board-supervised-software-v1' and
            value.get('physical_access') is False and
            all(value.get(key) is True for key in
                ('admission_cases', 'launcher_cases', 'host_capture', 'host_lifecycle')),
            'Board software evidence is incomplete.')
    for key, name in [('image', 'image.elf'), ('launcher', 'launch.bin')]:
        artifact(value[key])
        require(value[key]['sha256'] == prepared['files'][name],
                'Software evidence covers different executable bytes.')
    paths = dict(source_files())
    paths.update({'supervised-host-tool.py': Path(supervisor.__file__),
                  'base-host-tool.py': Path(supervisor.base.__file__),
                  'observer-host-tool.py': Path(supervisor.observer.__file__)})
    require(set(value.get('sources', {})) == set(paths), 'Software source identities are incomplete.')
    for name, path in paths.items():
        require(digest(artifact(value['sources'][name])) == digest(path),
                'Software evidence covers another implementation: ' + name)
    require(value.get('evidence'), 'Software result evidence is missing.')
    for item in value['evidence']:
        artifact(item)


def claimed(database, test_id, owner, bundle, prepared):
    expected = {**prepared['files'], 'prepared.json': prepared['_prepared_sha256']}
    for name, identity in expected.items():
        require(digest(bundle / name) == identity, 'Frozen board bundle changed: ' + name)
    db = sqlite3.connect(database.resolve().as_uri() + '?mode=ro', uri=True)
    db.row_factory = sqlite3.Row
    try:
        require(not db.execute("SELECT value FROM settings WHERE key='halted'").fetchone()[0],
                'Board queue is halted.')
        row = db.execute('SELECT * FROM tests WHERE id=?', (test_id,)).fetchone()
        require(row is not None and row['state'] == 'claimed' and row['owner'] == owner,
                'Matching coordinator claim is required.')
        submission, review = json.loads(row['submission']), json.loads(row['review'])
        require(set(submission['prerequisites']) == PREREQUISITES and
                set(review['prerequisites']) == PREREQUISITES, 'Board prerequisites differ.')
        refs = {name: artifact(value) for name, value in review['prerequisites'].items()}
        require(refs['supervised-ram-recovery'] == (bundle / 'admission.json').resolve() and
                review['prerequisites']['supervised-ram-recovery']['sha256'] == expected['admission.json'],
                'Reviewed board entry differs.')
        validate_software(json.loads(refs['board-supervised-software'].read_text()), bundle, prepared)
        require({((bundle / name).resolve(), expected[name]) for name in ('prepared.json', 'image.elf', 'launch.bin')} <=
                {(artifact(value), value['sha256']) for value in submission['artifacts']},
                'Queue does not cover this prepared image.')
    finally:
        db.close()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('bundle', 'fel', 'evidence', 'database'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--claim', required=True)
    parser.add_argument('--owner', required=True)
    parser.add_argument('--supervised-manual-reconnect', action='store_true', required=True)
    args = parser.parse_args(argv)
    bundle = args.bundle.resolve()
    prepared = preparation.validate_bundle(bundle)
    admission = json.loads(bundle_bytes(bundle, prepared, 'admission.json'))
    session = bundle_bytes(bundle, prepared, 'session.bin')
    template_hash = hashlib.sha256(bundle_bytes(bundle, prepared, 'session-template.bin')).hexdigest()
    profile = bundle_bytes(bundle, prepared, prepared['profile']) if prepared['profile'] else None
    entry_evidence = {path: bundle_bytes(bundle, prepared, item['name'])
                      for path, item in prepared['entry_evidence'].items()}

    def validate_entry(identity, port):
        preparation.validate_admission(admission, prepared['image_sha256'], template_hash,
                                       entry_evidence, True, prepared.get('execution_profile','board-supervised-v2'))
        require(admission['identity'] == identity and admission['port'] == port,
                'Board entry covers another USB instance.')

    return supervisor.run_session(args, bundle, prepared,
        lambda: claimed(args.database, args.claim, args.owner, bundle, prepared), validate_entry,
        lambda device, same, deadline, record, save: capture_device(
            device, same, deadline, session, profile, args.evidence, record, save),
        product='sunstoneos native board', serial='sunstoneos-a733-board',
        bcd_device=0x0100, capture_seconds=59, result_key='board_result',
        source_files=source_files(), description='Supervised native board; manual full power removal after capture')


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, sqlite3.Error, struct.error) as error:
        sys.exit(f'Stopped: {error}')
