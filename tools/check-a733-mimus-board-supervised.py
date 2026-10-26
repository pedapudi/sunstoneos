#!/usr/bin/env python3
"""Capture combined Mimus results under one existing supervised coordinator claim."""
import argparse
from contextlib import contextmanager
import hashlib
import importlib.util
import json
from pathlib import Path
import sqlite3
import struct
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


supervisor = module('mimus_board_supervisor', ROOT / 'tools/check-a733-smp-supervised.py')
preparation = module('mimus_board_preparation', ROOT / 'apps/mimus-board/prepare.py')
host = preparation.host
require, digest = supervisor.require, supervisor.digest
PRODUCT, SERIAL = 'sunstoneos native board', 'sunstoneos-a733-board'
PREREQUISITES = {'supervised-ram-recovery', 'mimus-board-supervised-software'}
# All generations share the native 60-second session. The one recorded physical
# recognition took 4.41 times the audio duration, about 16 s for this 3.59 s
# recording, so about three fit; four leaves margin for a faster image.
MAX_GENERATIONS = 4


def generation_count(count, workload):
    require(type(count) is int and 1 <= count <= MAX_GENERATIONS,
            f'Recognition count must be between one and {MAX_GENERATIONS}; '
            'all generations share the 59-second capture window.')
    require(workload == 4 or workload in (0, 2) and count == 1,
            'Repeated capture requires keyboard workload four.')


def source_files():
    return [('mimus-board-host-tool.py', Path(__file__)),
            ('mimus-board-preparation.py', Path(preparation.__file__)),
            ('mimus-board-transport.py', Path(host.__file__)),
            ('board-preparation.py', Path(preparation.board.__file__)),
            ('usb-access.py', ROOT / 'tools/usb-access.py')]


def software_sources():
    return dict(source_files() + [
        ('supervised-host-tool.py', Path(supervisor.__file__)),
        ('base-host-tool.py', Path(supervisor.base.__file__)),
        ('observer-host-tool.py', Path(supervisor.observer.__file__))])


def freeze_bundle(bundle, count):
    """Reuse preparation and preserve its nested board/assets identities."""
    bundle = Path(bundle).resolve()
    metadata = (bundle / 'prepared.json').read_bytes()
    outer = preparation.validate_bundle(bundle)
    require(json.loads(metadata) == outer, 'Combined metadata changed during validation.')
    board = preparation.board.validate_bundle(bundle / 'board')
    generation_count(count, board['workload'])
    board_metadata = (bundle / 'board/prepared.json').read_bytes()
    require(json.loads(board_metadata) == {k: v for k, v in board.items() if not k.startswith('_')},
            'Board metadata changed during validation.')
    assets_metadata = (bundle / 'assets/assets.json').read_bytes()
    require(hashlib.sha256(board_metadata).hexdigest() == outer['board_prepared_sha256'] and
            hashlib.sha256(assets_metadata).hexdigest() == outer['assets_sha256'],
            'Nested metadata changed after validation.')
    assets = json.loads(assets_metadata)['assets']
    expected = {'prepared.json': hashlib.sha256(metadata).hexdigest(),
                'board/prepared.json': outer['board_prepared_sha256'],
                'assets/assets.json': outer['assets_sha256']}
    expected.update({'board/' + name: value for name, value in board['files'].items()})
    for item in assets:
        require(Path(item['name']).name == item['name'], 'Invalid asset filename.')
        expected['assets/' + item['name']] = item['sha256']
    for item in outer['input_evidence'].values():
        require(Path(item['name']).name == item['name'], 'Invalid input evidence filename.')
        expected[item['name']] = item['sha256']
    frozen = {}
    for name, identity in expected.items():
        data = (bundle / name).read_bytes()
        require(hashlib.sha256(data).hexdigest() == identity,
                'Combined input changed before freezing: ' + name)
        frozen[name] = data
    manifest = json.loads(frozen['board/image-input-manifest.json'])
    if board['workload'] == 4:
        require(manifest.get('keyboard_result_version') == 3 and
                manifest.get('keyboard_generation_limit') == 0xffffffff and
                manifest.get('keyboard_rearm') == 'full-result-and-ack-tx-completion',
                'Repeated keyboard protocol requires its exact image manifest.')
    board['_source_hashes'] = {name: digest(path) for name, path in software_sources().items()}
    board['_combined_files'] = expected
    board['_combined_frozen'] = frozen
    board['_combined_bundle'] = bundle
    board['_recognitions'] = count
    return board


@contextmanager
def launch_view(prepared):
    """Flatten only the validated bytes for the existing sealed/readback preloader.

    Original prepared metadata remains unchanged. The derived upload map is kept
    separately and is included in the supervisor's durable evidence sources.
    """
    frozen = prepared['_combined_frozen']
    view = dict(prepared)
    view['files'] = dict(prepared['files'])
    view['_sizes'] = dict(prepared['_sizes'])
    view['segments'] = list(prepared['segments'])
    payloads = {name: frozen['board/' + name] for name in prepared['files']}
    for item in json.loads(frozen['assets/assets.json'])['assets']:
        name = item['name']
        require(name not in payloads, 'Asset collides with board launch input.')
        payloads[name] = frozen['assets/' + name]
        view['files'][name] = item['sha256']
        view['_sizes'][name] = item['bytes']
        view['segments'].append({'name': name, 'address': item['address']})
    payloads['prepared.json'] = frozen['board/prepared.json']
    plan = {'schema': 'mimus-board-supervised-launch-view-v1',
            'original_files': prepared['_combined_files'],
            'uploads': view['segments'], 'files': view['files'],
            'recognitions': prepared['_recognitions'],
            'session_epoch': prepared['epoch'], 'session_generation': prepared['generation']}
    with tempfile.TemporaryDirectory(prefix='sunstone-mimus-board-launch-') as directory:
        directory = Path(directory)
        for name, data in payloads.items():
            require(Path(name).name == name, 'Invalid flattened launch filename.')
            (directory / name).write_bytes(data)
        (directory / 'combined-launch-view.json').write_text(json.dumps(plan, indent=2) + '\n')
        view['_launch_plan_sha256'] = digest(directory / 'combined-launch-view.json')
        (directory / 'combined-prepared.json').write_bytes(frozen['prepared.json'])
        yield directory, view


def reference_bytes(item):
    require(isinstance(item, dict) and set(item) == {'path', 'sha256'} and
            Path(item['path']).is_absolute(), 'Absolute reviewed reference required.')
    path = Path(item['path'])
    data = path.read_bytes()
    require(hashlib.sha256(data).hexdigest() == item['sha256'], 'Reviewed reference changed.')
    return path.resolve(), data


def validate_software(value, prepared):
    require(value.get('schema') == 'a733-mimus-board-supervised-software-v1' and
            value.get('physical_access') is False and
            all(value.get(key) is True for key in
                ('admission_cases', 'launcher_cases', 'host_capture', 'host_lifecycle')),
            'Combined Mimus software evidence is incomplete.')
    for key, name in [('image', 'image.elf'), ('launcher', 'launch.bin')]:
        _, data = reference_bytes(value[key])
        require(hashlib.sha256(data).hexdigest() == prepared['files'][name],
                'Software evidence covers different executable bytes.')
    require(set(value.get('sources', {})) == set(prepared['_source_hashes']),
            'Software source identities are incomplete.')
    for name, identity in prepared['_source_hashes'].items():
        _, data = reference_bytes(value['sources'][name])
        require(hashlib.sha256(data).hexdigest() == identity,
                'Software evidence covers another implementation: ' + name)
    require(value.get('evidence'), 'Software result evidence is missing.')
    for item in value['evidence']:
        reference_bytes(item)


def claimed(database, test_id, owner, bundle, prepared):
    """Read the existing coordinator claim; never create or mutate queue work."""
    for name, identity in prepared['_combined_files'].items():
        require(digest(bundle / name) == identity, 'Frozen combined bundle changed: ' + name)
    db = sqlite3.connect(database.resolve().as_uri() + '?mode=ro', uri=True)
    db.row_factory = sqlite3.Row
    try:
        require(not db.execute("SELECT value FROM settings WHERE key='halted'").fetchone()[0],
                'Coordinator queue is halted.')
        row = db.execute('SELECT * FROM tests WHERE id=?', (test_id,)).fetchone()
        require(row is not None and row['state'] == 'claimed' and row['owner'] == owner,
                'Matching coordinator claim is required.')
        submission, review = json.loads(row['submission']), json.loads(row['review'])
        require(set(submission['prerequisites']) == PREREQUISITES and
                set(review['prerequisites']) == PREREQUISITES, 'Combined prerequisites differ.')
        path, data = reference_bytes(review['prerequisites']['supervised-ram-recovery'])
        require(path == (bundle / 'board/admission.json').resolve() and
                hashlib.sha256(data).hexdigest() == prepared['files']['admission.json'],
                'Reviewed entry differs from the combined bundle.')
        _, data = reference_bytes(review['prerequisites']['mimus-board-supervised-software'])
        validate_software(json.loads(data), prepared)
        covered = {reference_bytes(item)[0]: item['sha256'] for item in submission['artifacts']}
        # Include all three asset bodies, not merely an unauthenticated upload list.
        required = {'prepared.json', 'board/prepared.json', 'board/image.elf', 'board/launch.bin',
                    'assets/assets.json'} | {name for name in prepared['_combined_files']
                                             if name.startswith('assets/asset-')}
        require(all(covered.get((bundle / name).resolve()) == prepared['_combined_files'][name]
                    for name in required), 'Claim does not cover this combined image and assets.')
    finally:
        db.close()


def capture_device(device, same, deadline, prepared, evidence, record, save,
                   *, now=time.monotonic, pause=time.sleep, before=None):
    """Preserve raw fragments and complete outputs durably before each ACK write.

    before(generation), when given, runs before each recognition's first
    request; the OS launcher switches performance modes there."""
    session = prepared['_combined_frozen']['board/session.bin']
    epoch, attachment = struct.unpack_from('<2Q', session, 24)
    keyboard = prepared['workload'] == 4
    staged = prepared['workload'] == 2
    identity = {'image_sha256': prepared['image_sha256'],
                'combined_bundle_sha256': prepared['_combined_files']['prepared.json'],
                'epoch': epoch, 'attachment_generation': attachment}
    record.update(mimus_board_result='unobserved',
                  mimus_board_retention='whole-board-power-removal',
                  requested_recognitions=prepared['_recognitions'],
                  combined_identity=identity, recognitions=[])
    save()
    completed = []
    try:
        for generation in range(1, prepared['_recognitions'] + 1):
            require(now() < deadline, 'Combined capture deadline expired.')
            if before is not None:
                before(generation)
            current = {**identity, 'recognition_generation': generation,
                       'state': 'unobserved', 'frames': [], 'fragments': []}
            record['recognitions'].append(current)
            save()
            raw_frames = []
            copied = False
            response_bytes = bytearray()
            response_fragments = []

            def note_ack(frame):
                if keyboard and len(frame) == 64 and struct.unpack_from('<8I', frame) == (
                        0x424d494d, 3, 1, 0,
                        current.get('copied_outputs', {}).get('transcript.txt', {}).get('bytes', -1), 0, 1, 4):
                    require(struct.unpack_from('<2Q', frame, 32) == (epoch, attachment) and
                            struct.unpack_from('<QII', frame, 48) == (generation, 0xffffffff, 0),
                            'ACK response identity differs.')
                    current['ack_response_observed'] = {
                        'sha256': hashlib.sha256(frame).hexdigest(),
                        'received_response_hex': bytes(frame).hex(),
                        'fragments': response_fragments.copy()}

            def persist_copies():
                nonlocal copied
                if copied:
                    return
                text, sector, trigger = bytearray(), None, None
                total = None
                for frame in raw_frames:
                    magic, version, status, size, count, offset, storage, op = struct.unpack_from('<8I', frame)
                    if status != 1:
                        continue
                    require((magic, version, storage) == (0x424d494d, 3 if keyboard else 1, 3 if staged else 1) and
                            struct.unpack_from('<2Q', frame, 32) == (epoch, attachment),
                            'Copied response identity differs.')
                    if keyboard:
                        require(struct.unpack_from('<Q', frame, 48)[0] == generation,
                                'Copied response recognition generation differs.')
                    require(len(frame) == 64 + size and 0 < count <= 16384,
                            'Copied response extent differs.')
                    require(total is None or total == count, 'Transcript extent changed.')
                    total = count
                    if op == 1:
                        require(offset == len(text), 'Transcript copy is not contiguous.')
                        text.extend(frame[64:])
                    elif op == 2:
                        require(size == 512, 'Copied sector extent differs.')
                        sector = frame[64:]
                    elif op == 3:
                        require(size == 128, 'Copied trigger extent differs.')
                        trigger = frame[64:]
                require(total == len(text) and (staged or sector is not None) and (not keyboard or trigger is not None),
                        'Complete copied outputs required before ACK.')
                outputs = {}
                for name, data in [('transcript.txt', bytes(text)), *([] if staged else [('sector.bin', sector)]),
                                   *([('trigger.bin', trigger)] if keyboard else [])]:
                    path = evidence / f'recognition-{generation:03}-{name}'
                    supervisor.durable_frame(path, data)
                    outputs[name] = {'path': path.name, 'bytes': len(data), 'sha256': digest(path)}
                current['copied_outputs'] = outputs
                current['state'] = 'copied-before-ack' if keyboard else 'copied'
                save()
                copied = True

            class RetainedDevice:
                def write(self, endpoint, packet, timeout):
                    require(now() < deadline, 'Combined deadline before request.')
                    op, zero, count, offset, expected = struct.unpack('<BBHIQ', packet)
                    if op == 4:
                        require(keyboard and (zero, count, offset, expected) == (0, 0, 0, generation),
                                'ACK request generation differs.')
                        persist_copies()
                    # Both filesystem work and identity checks can consume time.
                    require(now() < deadline, 'Combined deadline after durable copies.')
                    same()
                    current.setdefault('requests', []).append({'op': op, 'generation': expected})
                    save()
                    require(now() < deadline, 'Combined deadline after request journal.')
                    remaining = max(1, int((deadline - now()) * 1000))
                    response_bytes.clear(); response_fragments.clear()
                    sent = device.write(endpoint, packet, timeout=min(timeout, remaining))
                    if op == 4:
                        current['ack_request_write_completed'] = sent == len(packet)
                        save()
                    return sent

                def read(self, endpoint, size, timeout):
                    data = bytes(device.read(endpoint, size, timeout=timeout))
                    path = evidence / f"recognition-{generation:03}-fragment-{len(current['fragments']) + 1:04}.bin"
                    response_bytes.extend(data)
                    note_ack(response_bytes)
                    supervisor.durable_frame(path, data)
                    fragment = {'path': path.name, 'bytes': len(data), 'sha256': digest(path)}
                    current['fragments'].append(fragment)
                    response_fragments.append(fragment)
                    note_ack(response_bytes)
                    save()
                    return data

            def retain(frame):
                path = evidence / f"recognition-{generation:03}-frame-{len(current['frames']) + 1:04}.bin"
                supervisor.durable_frame(path, frame)
                current['frames'].append({'path': path.name, 'sha256': digest(path)})
                raw_frames.append(frame)
                note_ack(frame)
                if 'ack_response_observed' in current and struct.unpack_from('<I', frame, 28)[0] == 4:
                    current['ack_response_observed']['path'] = path.name
                save()

            exchange = host.device_exchange(RetainedDevice(), same, deadline, retain, now=now)
            result = host.capture(exchange, epoch, attachment, deadline, now=now, pause=pause,
                                  keyboard=keyboard, recognition_generation=generation, staged=staged)
            if not keyboard:
                persist_copies()
            require(copied and now() < deadline, 'Combined output capture incomplete or expired.')
            current.update(state='acknowledged' if keyboard else 'observed-success',
                           transcript_bytes=len(result['transcript']))
            save()
            completed.append(generation)
        record['mimus_board_result'] = 'observed-success'
        record['durable_result'] = {**identity, 'recognition_generations': completed.copy()}
        try:
            save()
        except BaseException:
            record['mimus_board_result'] = 'unobserved'
            record.pop('durable_result', None)
            raise
    except BaseException as error:
        record['mimus_board_result'] = 'observed-failure'
        record.pop('durable_result', None)
        record['capture_failure'] = {'type': type(error).__name__, 'message': str(error),
                                     'completed_generations': completed.copy()}
        # Files already committed remain available even if this save also fails.
        save()
        raise


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('bundle', 'fel', 'evidence', 'database'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--claim', required=True)
    parser.add_argument('--owner', required=True)
    parser.add_argument('--generations', type=int, default=1)
    parser.add_argument('--supervised-manual-reconnect', action='store_true', required=True)
    args = parser.parse_args(argv)
    bundle = args.bundle.resolve()
    prepared = freeze_bundle(bundle, args.generations)
    frozen = prepared['_combined_frozen']
    admission = json.loads(frozen['board/admission.json'])
    entry_evidence = {path: frozen['board/' + item['name']]
                      for path, item in prepared['entry_evidence'].items()}

    def validate_entry(identity, port):
        preparation.board.validate_admission(admission, prepared['image_sha256'],
            hashlib.sha256(frozen['board/session-template.bin']).hexdigest(), entry_evidence,
            True, prepared.get('execution_profile', 'board-supervised-v2'))
        retained = json.loads(frozen['prepared.json'])['input_evidence']
        def read(item):
            value = retained[item['path']]
            require(value['sha256'] == item['sha256'], 'Mimus input evidence differs.')
            return frozen[value['name']]
        preparation.input_admission(admission, prepared['image_sha256'], read)
        require(admission['identity'] == identity and admission['port'] == port,
                'Combined entry covers another USB instance.')

    with launch_view(prepared) as (view, launch):
        def ready():
            claimed(args.database, args.claim, args.owner, bundle, prepared)
            for name, expected in launch['files'].items():
                require(digest(view / name) == expected, 'Derived launch bytes changed.')
            require(digest(view / 'combined-launch-view.json') == launch['_launch_plan_sha256'],
                    'Derived upload plan changed.')
            require(digest(view / 'prepared.json') == prepared['_prepared_sha256'],
                    'Derived launcher metadata changed.')
        return supervisor.run_session(args, view, launch, ready, validate_entry,
            lambda device, same, deadline, record, save: capture_device(
                device, same, deadline, prepared, args.evidence, record, save),
            product=PRODUCT, serial=SERIAL, bcd_device=0x0100,
            capture_seconds=59, result_key='mimus_board_result',
            source_files=source_files() + [('combined-launch-view.json', view / 'combined-launch-view.json'),
                                         ('combined-prepared.json', view / 'combined-prepared.json')],
            description='Supervised combined Mimus; retain resources until whole-board power removal')


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, sqlite3.Error, struct.error) as error:
        sys.exit(f'Stopped: {error}')
