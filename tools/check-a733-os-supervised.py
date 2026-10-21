#!/usr/bin/env python3
"""Launch one prepared supervised integrated OS session under an existing coordinator claim.

The bundle comes from apps/os/prepare.py and is frozen and revalidated here,
including its reviewed entry record. The transport, the board lock of
tools/usb-access.py, the single launch attempt and the retention after entry
are those of the shared supervised launcher (tools/check-a733-smp-supervised.py).
The OS image serves the mimus board application's USB0 result protocol, so
capture reuses tools/check-a733-mimus-board-supervised.py. Host checks of this
tool (checks/os-image-launch.py) stop before any device access.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import sqlite3
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


combined = module('os_supervised_capture', ROOT / 'tools/check-a733-mimus-board-supervised.py')
preparation = module('os_supervised_preparation', ROOT / 'apps/os/prepare.py')
supervisor = combined.supervisor
require, digest, reference_bytes = combined.require, combined.digest, combined.reference_bytes
PREREQUISITES = {'supervised-ram-recovery', 'os-supervised-software'}
# Each generation of the audio image first captures up to 5 s and then
# recognizes it; at the recorded 4.41 times real time two fit the window.
MAX_AUDIO_GENERATIONS = 2


def source_files():
    return [('os-host-tool.py', Path(__file__)), ('os-preparation.py', Path(preparation.__file__)),
            ('mimus-board-host-tool.py', Path(combined.__file__)),
            ('mimus-board-preparation.py', Path(preparation.mimus.__file__)),
            ('mimus-board-transport.py', Path(preparation.host.__file__)),
            ('thermal-preparation.py', Path(preparation.thermal.__file__)),
            ('board-preparation.py', Path(preparation.board.__file__)),
            ('dma-nonsecure-access.py', Path(preparation.security_authority.__file__)),
            ('usb-access.py', ROOT / 'tools/usb-access.py')]


def software_sources():
    return dict(source_files() + [
        ('supervised-host-tool.py', Path(supervisor.__file__)),
        ('base-host-tool.py', Path(supervisor.base.__file__)),
        ('observer-host-tool.py', Path(supervisor.observer.__file__))])


def freeze_bundle(bundle, count):
    """Validate once, then keep every byte the launch and capture will use."""
    bundle = Path(bundle).resolve()
    metadata = (bundle / 'prepared.json').read_bytes()
    outer = preparation.validate_bundle(bundle)
    require(json.loads(metadata) == outer, 'OS metadata changed during validation.')
    board = preparation.validate_board(bundle / 'board')
    combined.generation_count(count, board['workload'])
    require(not board['audio'] or count <= MAX_AUDIO_GENERATIONS,
            f'At most {MAX_AUDIO_GENERATIONS} audio recognitions fit the capture window.')
    board_metadata = (bundle / 'board/prepared.json').read_bytes()
    assets_metadata = (bundle / 'assets/assets.json').read_bytes()
    require(hashlib.sha256(board_metadata).hexdigest() == outer['board_prepared_sha256'] and
            hashlib.sha256(assets_metadata).hexdigest() == outer['assets_sha256'],
            'Nested metadata changed after validation.')
    expected = {'prepared.json': hashlib.sha256(metadata).hexdigest(),
                'board/prepared.json': outer['board_prepared_sha256'],
                'assets/assets.json': outer['assets_sha256']}
    expected.update({'board/' + name: value for name, value in board['files'].items()})
    for item in json.loads(assets_metadata)['assets']:
        require(Path(item['name']).name == item['name'], 'Invalid asset filename.')
        expected['assets/' + item['name']] = item['sha256']
    frozen = {}
    for name, identity in expected.items():
        data = (bundle / name).read_bytes()
        require(hashlib.sha256(data).hexdigest() == identity, 'OS input changed before freezing: ' + name)
        frozen[name] = data
    board['_source_hashes'] = {name: digest(path) for name, path in software_sources().items()}
    board['_combined_files'] = expected
    board['_combined_frozen'] = frozen
    board['_combined_bundle'] = bundle
    board['_recognitions'] = count
    return board


def validate_software(value, prepared):
    require(value.get('schema') == 'a733-os-supervised-software-v1' and value.get('physical_access') is False and
            value.get('audio') is prepared['audio'] and
            all(value.get(key) is True for key in
                ('admission_cases', 'launcher_cases', 'host_capture', 'host_lifecycle')),
            'OS software evidence is incomplete.')
    for key, name in [('image', 'image.elf'), ('launcher', 'launch.bin')]:
        _, data = reference_bytes(value[key])
        require(hashlib.sha256(data).hexdigest() == prepared['files'][name],
                'Software evidence covers different executable bytes.')
    require(set(value.get('sources', {})) == set(prepared['_source_hashes']), 'Software source identities are incomplete.')
    for name, identity in prepared['_source_hashes'].items():
        _, data = reference_bytes(value['sources'][name])
        require(hashlib.sha256(data).hexdigest() == identity, 'Software evidence covers another implementation: ' + name)
    require(value.get('evidence'), 'Software result evidence is missing.')
    for item in value['evidence']:
        reference_bytes(item)


def claimed(database, test_id, owner, bundle, prepared):
    """Read the existing coordinator claim; never create or mutate queue work."""
    for name, identity in prepared['_combined_files'].items():
        require(digest(bundle / name) == identity, 'Frozen OS bundle changed: ' + name)
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
                set(review['prerequisites']) == PREREQUISITES, 'OS prerequisites differ.')
        path, data = reference_bytes(review['prerequisites']['supervised-ram-recovery'])
        require(path == (bundle / 'board/admission.json').resolve() and
                hashlib.sha256(data).hexdigest() == prepared['files']['admission.json'],
                'Reviewed entry differs from the OS bundle.')
        _, data = reference_bytes(review['prerequisites']['os-supervised-software'])
        validate_software(json.loads(data), prepared)
        covered = {reference_bytes(item)[0]: item['sha256'] for item in submission['artifacts']}
        required = {'prepared.json', 'board/prepared.json', 'board/image.elf', 'board/launch.bin',
                    'board/session.bin', 'board/admission.json', 'assets/assets.json'} | {
                    name for name in prepared['_combined_files'] if name.startswith('assets/asset-')}
        require(all(covered.get((bundle / name).resolve()) == prepared['_combined_files'][name] for name in required),
                'Claim does not cover this OS image, session and inputs.')
    finally:
        db.close()


def entry_check(prepared, identity, port):
    """The frozen reviewed record must name this USB instance."""
    frozen = prepared['_combined_frozen']
    admission = json.loads(frozen['board/admission.json'])
    cache = {path: frozen['board/' + item['name']] for path, item in prepared['entry_evidence'].items()}
    template = frozen['board/session-template.bin']
    preparation.validate_admission(admission, prepared['image_sha256'], hashlib.sha256(template).hexdigest(),
                                   template, prepared['audio'], cache, True)
    require(admission['identity'] == identity and admission['port'] == port, 'OS entry covers another USB instance.')


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
    with combined.launch_view(prepared) as (view, launch):
        def ready():
            claimed(args.database, args.claim, args.owner, bundle, prepared)
            for name, expected in launch['files'].items():
                require(digest(view / name) == expected, 'Derived launch bytes changed.')
            require(digest(view / 'combined-launch-view.json') == launch['_launch_plan_sha256'],
                    'Derived upload plan changed.')
            require(digest(view / 'prepared.json') == prepared['_prepared_sha256'], 'Derived launcher metadata changed.')
        return supervisor.run_session(args, view, launch, ready, lambda identity, port: entry_check(prepared, identity, port),
            lambda device, same, deadline, record, save: combined.capture_device(
                device, same, deadline, prepared, args.evidence, record, save),
            product=combined.PRODUCT, serial=combined.SERIAL, bcd_device=0x0100,
            capture_seconds=59, result_key='mimus_board_result',
            source_files=source_files() + [('combined-launch-view.json', view / 'combined-launch-view.json'),
                                           ('combined-prepared.json', view / 'combined-prepared.json')],
            description='Supervised integrated OS; retain resources until whole-board power removal')


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, sqlite3.Error, struct.error) as error:
        sys.exit(f'Stopped: {error}')
