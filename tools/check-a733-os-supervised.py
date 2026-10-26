#!/usr/bin/env python3
"""Launch one prepared supervised integrated OS session under an existing coordinator claim.

The bundle comes from apps/os/prepare.py and is frozen and revalidated here,
including its reviewed entry record. The transport, the board lock of
tools/usb-access.py, the single launch attempt and the retention after entry
are those of the shared supervised launcher (tools/check-a733-smp-supervised.py).
The OS image serves the mimus board application's USB0 result protocol, so
capture reuses tools/check-a733-mimus-board-supervised.py. With
--fault-report, the capture first requests the previous boot's fault report
once through tools/fault-report.py and saves it with its decoding. For an
image whose link check records the NPU profile, a capture that completes is
followed by one request for the NPU report (USB0 operation 6), which is saved
with its decoding and classification from apps/os/npu-report.py; other
images never send operation 6. CPU-clock images also return the first
recognition's immutable thermal window through paged operation 7; it is saved
separately as thermal-report.bin and thermal-report.json.
With --perf-sequence, for an image whose
link check records the performance-mode service, each recognition is
preceded by one switch to the next listed mode through USB0 operation 8
(tools/sunstone-perf.py), and the capture ends with one readback; each
report is saved with its decoding. Host checks of this tool
(checks/os-image-launch.py) stop before any device access.
"""
import argparse
import hashlib
import importlib.util
import io
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


combined = module('os_supervised_capture', ROOT / 'tools/check-a733-mimus-board-supervised.py')
preparation = module('os_supervised_preparation', ROOT / 'apps/os/prepare.py')
faults = module('os_supervised_fault_report', ROOT / 'tools/fault-report.py')
fault_decoder = module('os_supervised_fault_decode', ROOT / 'checks/fault-decode.py')
npu_report = module('os_supervised_npu_report', ROOT / 'apps/os/npu-report.py')
thermal_report = module('os_supervised_thermal_report', ROOT / 'apps/os/thermal-report.py')
perf_modes = module('os_supervised_perf_modes', ROOT / 'tools/sunstone-perf.py')
supervisor = combined.supervisor
require, digest, reference_bytes = combined.require, combined.digest, combined.reference_bytes
PREREQUISITES = {'supervised-ram-recovery', 'os-supervised-software'}
# Each generation of the audio image first captures up to 5 s and then
# recognizes it; at the recorded 4.41 times real time two fit the window.
MAX_AUDIO_GENERATIONS = 2
# The NPU report needs one request and one 104-byte reply. With less than
# this left of the capture window it is not requested.
NPU_REPORT_MIN_SECONDS = 0.1


def source_files():
    return [('os-host-tool.py', Path(__file__)), ('os-preparation.py', Path(preparation.__file__)),
            ('mimus-board-host-tool.py', Path(combined.__file__)),
            ('mimus-board-preparation.py', Path(preparation.mimus.__file__)),
            ('mimus-board-transport.py', Path(preparation.host.__file__)),
            ('thermal-preparation.py', Path(preparation.thermal.__file__)),
            ('board-preparation.py', Path(preparation.board.__file__)),
            ('dma-nonsecure-access.py', Path(preparation.security_authority.__file__)),
            ('fault-report.py', Path(faults.__file__)), ('fault-decode.py', Path(fault_decoder.__file__)),
            ('npu-report.py', Path(npu_report.__file__)),
            ('thermal-report.py', Path(thermal_report.__file__)),
            ('sunstone-perf.py', Path(perf_modes.__file__)),
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
                                   template, prepared['audio'], cache, True,
                                   perf_modes=json.loads(frozen['board/image-input-manifest.json']).get('perf_modes', False))
    require(admission['identity'] == identity and admission['port'] == port, 'OS entry covers another USB instance.')


def require_fault_report(prepared):
    """Only an image built with SUNSTONE_A733_FAULT_PAGE answers operation 5."""
    from elftools.elf.elffile import ELFFile
    symbols = ELFFile(io.BytesIO(prepared['_combined_frozen']['board/image.elf'])).get_section_by_name('.symtab')
    found = symbols.get_symbol_by_name('a733_fault_previous') if symbols else None
    require(bool(found) and all(item['st_shndx'] != 'SHN_UNDEF' for item in found),
            'The OS image keeps no fault report.')


def read_fault_report(device, same, deadline, evidence, record, save):
    """One operation 5 exchange before the capture; any error ends the run."""
    record['fault_report'] = 'unobserved'
    save()
    path = evidence / 'fault-report.bin'
    raw = faults.fetch(device, same, deadline, lambda data: supervisor.durable_frame(path, data))
    result = {'path': path.name, 'bytes': len(raw), 'sha256': digest(path)}
    try:
        decoded = fault_decoder.decode_report(raw)
    except ValueError as error:
        result['decode_error'] = str(error)
    else:
        decoded_path = evidence / 'fault-report.json'
        supervisor.durable_frame(decoded_path, (json.dumps(decoded, indent=2) + '\n').encode())
        result['decoded'] = decoded_path.name
    record['fault_report'] = result
    save()


def npu_profile(prepared):
    """True when the frozen link check records the NPU profile; preparation
    already bound that record to the image."""
    manifest = json.loads(prepared['_combined_frozen']['board/image-input-manifest.json'])
    return manifest.get('npu', False) is True


def read_npu_report(device, same, deadline, prepared, evidence, record, save, now=time.monotonic):
    """One operation 6 exchange after the capture; any error ends the run.

    A capture that leaves less than NPU_REPORT_MIN_SECONDS of its window
    gets no request. A retained NPU failure fails the board, after which USB0
    sends nothing, so that outcome appears here as an error with no report."""
    if deadline - now() < NPU_REPORT_MIN_SECONDS:
        record['npu_report'] = {'state': 'not-requested', 'reason': 'less than 100 ms of the capture window left'}
        save()
        return
    record['npu_report'] = 'unobserved'
    save()
    path = evidence / 'npu-report.bin'
    epoch, generation = struct.unpack_from('<2Q', prepared['_combined_frozen']['board/session.bin'], 24)
    try:
        raw = faults.fetch(device, same, deadline, lambda data: supervisor.durable_frame(path, data),
                           op=npu_report.OP, size=npu_report.REPORT_BYTES, name='NPU report')
    except BaseException as error:
        record['npu_report'] = {'state': 'no-report', 'type': type(error).__name__, 'message': str(error)}
        save()
        raise
    result = {'path': path.name, 'bytes': len(raw), 'sha256': digest(path)}
    try:
        decoded = npu_report.decode(raw, epoch, generation)
    except ValueError as error:
        result['decode_error'] = str(error)
    else:
        result['report'] = decoded
        result['classification'] = npu_report.classify(decoded)
    record['npu_report'] = result
    save()


def thermal_profile(prepared):
    manifest = json.loads(prepared['_combined_frozen']['board/image-input-manifest.json'])
    return manifest.get('cpu_clock', False) is True and manifest.get('thermal_report', False) is True


def read_thermal_report(device, same, deadline, prepared, evidence, record, save, now=time.monotonic):
    """Keep recognition outputs even when the separate thermal window is unavailable."""
    if deadline - now() < 0.1:
        record['thermal_report'] = {'state': 'not-requested', 'reason': 'less than 100 ms of the capture window left'}
        save()
        return
    record['thermal_report'] = {'state': 'unobserved', 'frames': []}
    result = record['thermal_report']
    save()
    def retain(frame):
        path = evidence / f"thermal-frame-{len(result['frames']):03}.bin"
        supervisor.durable_frame(path, frame)
        result['frames'].append({'path': path.name, 'sha256': digest(path)})
        save()
    exchange = preparation.host.device_exchange(device, same, deadline, retain, now=now)
    epoch, generation = struct.unpack_from('<2Q', prepared['_combined_frozen']['board/session.bin'], 24)
    try:
        raw = thermal_report.capture(exchange, epoch, generation)
        path = evidence / 'thermal-report.bin'
        supervisor.durable_frame(path, raw)
        result.update(path=path.name, bytes=len(raw), sha256=digest(path))
        decoded = thermal_report.decode(raw, epoch, generation)
        decoded_path = evidence / 'thermal-report.json'
        supervisor.durable_frame(decoded_path, (json.dumps(decoded, indent=2) + '\n').encode())
        result.update(state=thermal_report.classify(decoded), decoded=decoded_path.name)
    except BaseException as error:
        result.update(state='unavailable', type=type(error).__name__, message=str(error))
        save()
        raise
    save()


def perf_sequence(prepared, text, generations):
    """The modes to switch to, one per recognition; only for an image whose
    link check records the performance-mode service."""
    if text is None:
        return None
    manifest = json.loads(prepared['_combined_frozen']['board/image-input-manifest.json'])
    perf_modes.manifest_check(manifest)
    modes = text.split(',')
    require(len(modes) == generations and all(m in perf_modes.MODES for m in modes),
            'Name one mode, high or low, per recognition.')
    return modes


def perf_exchange(device, same, deadline, evidence, record, save, name, command, mode=None):
    """One performance-mode command; every answer is saved before decoding."""
    answers = []

    def retain(raw):
        path = evidence / ('%s-%d.bin' % (name, len(answers)))
        supervisor.durable_frame(path, raw)
        answers.append({'path': path.name, 'sha256': digest(path)})
    entry = {'command': name, 'mode': mode, 'answers': answers, 'state': 'unobserved'}
    record.setdefault('perf_modes', []).append(entry)
    save()
    report = perf_modes.run(perf_modes.device_exchange(device, same, deadline, retain), command, mode)
    entry.update(state='observed', report=report)
    save()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('bundle', 'fel', 'evidence', 'database'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--claim', required=True)
    parser.add_argument('--owner', required=True)
    parser.add_argument('--generations', type=int, default=1)
    parser.add_argument('--supervised-manual-reconnect', action='store_true', required=True)
    parser.add_argument('--perf-sequence', help='modes to switch to before each recognition, for example high,low,high')
    parser.add_argument('--fault-report', action='store_true',
                        help="request the previous boot's fault report once before the capture")
    args = parser.parse_args(argv)
    bundle = args.bundle.resolve()
    prepared = freeze_bundle(bundle, args.generations)
    if args.fault_report:
        require_fault_report(prepared)
    npu = npu_profile(prepared)
    thermal = thermal_profile(prepared)
    modes = perf_sequence(prepared, args.perf_sequence, args.generations)

    def capture(device, same, deadline, record, save):
        if args.fault_report:
            read_fault_report(device, same, deadline, args.evidence, record, save)
        before = None if modes is None else lambda g: perf_exchange(
            device, same, deadline, args.evidence, record, save, 'perf-switch-%d' % g, perf_modes.SWITCH, modes[g - 1])
        combined.capture_device(device, same, deadline, prepared, args.evidence, record, save, before=before)
        if modes is not None:
            perf_exchange(device, same, deadline, args.evidence, record, save, 'perf-status', perf_modes.READ)
        if npu:
            read_npu_report(device, same, deadline, prepared, args.evidence, record, save)
        if thermal:
            read_thermal_report(device, same, deadline, prepared, args.evidence, record, save)
    with combined.launch_view(prepared) as (view, launch):
        def ready():
            claimed(args.database, args.claim, args.owner, bundle, prepared)
            for name, expected in launch['files'].items():
                require(digest(view / name) == expected, 'Derived launch bytes changed.')
            require(digest(view / 'combined-launch-view.json') == launch['_launch_plan_sha256'],
                    'Derived upload plan changed.')
            require(digest(view / 'prepared.json') == prepared['_prepared_sha256'], 'Derived launcher metadata changed.')
        return supervisor.run_session(args, view, launch, ready, lambda identity, port: entry_check(prepared, identity, port),
            capture,
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
