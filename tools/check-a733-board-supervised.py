#!/usr/bin/env python3
"""Capture one supervised board session with direct target and session validation."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
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
        audio = result.pop('audio_pcm',None)
        entry.update(result)
        require(now() < deadline, 'Board response arrived after capture deadline.')
        if result['usb_state'] == 2 or result['radio_state'] == 3:
            record['board_result'] = 'observed-failure'
            save()
            return result
        host_done = result['usb_state'] == (1 if workload < 2 or workload in (4,6,8,10,12,14) else 3)
        radio_done = result['radio_state'] == (2 if workload & 1 else 0)
        if host_done and radio_done:
            if sector is not None:
                sector_path = evidence / 'sector.bin'
                supervisor.durable_frame(sector_path, sector)
                record['copied_sector'] = {'path': sector_path.name, 'sha256': digest(sector_path)}
            if audio is not None:
                pcm_path=evidence/'audio.s16le';supervisor.durable_frame(pcm_path,audio)
                metadata_path=evidence/'audio.json';supervisor.durable_frame(metadata_path,(json.dumps(result,sort_keys=True)+'\n').encode())
                record['copied_audio']={'path':pcm_path.name,'sha256':digest(pcm_path),'metadata':metadata_path.name,'metadata_sha256':digest(metadata_path)}
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




def claimed(database, test_id, owner, bundle, prepared):
    """Check the input snapshot immediately before upload."""
    expected = preparation.snapshot_files(prepared)
    if '_prepared_sha256' in prepared:
        expected['prepared.json'] = prepared['_prepared_sha256']
    for name, identity in expected.items():
        require(Path(name).name == name and digest(bundle / name) == identity,
                'Prepared input changed before upload: ' + name)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('bundle', 'fel', 'evidence'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--claim')
    parser.add_argument('--owner')
    parser.add_argument('--supervised-manual-reconnect', action='store_true')
    parser.add_argument('--database', type=Path)
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
    except (OSError, ValueError, RuntimeError, struct.error) as error:
        sys.exit(f'Stopped: {error}')
