#!/usr/bin/env python3
"""Check HEVC capture journal cost and durability with a modeled USB device.

The session is a synthetic 856-byte record; its geometry admission is replaced
by a no-op so only journal behaviour is exercised. No board or USB is accessed.
"""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('audit_hevc_host', ROOT / 'apps/hevc/host.py')
host = importlib.util.module_from_spec(spec)
spec.loader.exec_module(host)
decoder = host.decoder
CLIP = 524288
PENDING = 200


def session(clip):
    raw = bytearray(856)
    struct.pack_into('<2Q', raw, 24, 31, 37)
    raw[768:800] = hashlib.sha256(clip).digest()
    struct.pack_into('<4Q', raw, 800, 0, CLIP // 2, CLIP // 2, CLIP // 2)
    return bytes(raw)


def frames(raw):
    pictures, pending = [], []
    for ordinal in range(2):
        data = bytes((i * 3 + ordinal) & 255 for i in range(decoder.MAXIMUM))
        header = bytearray(160)
        struct.pack_into('<4I2Q', header, 0, 0x44363235, 1, 1, len(data), 31, 37)
        header[32:64] = raw[768:800]
        header[64:96] = hashlib.sha256(data).digest()
        struct.pack_into('<11I', header, 96, ordinal, 2, 7 if ordinal else 0, 640, 480, 640, 640, 307200, 6,
                         1 if ordinal else 2, 0)
        pictures.append(bytes(header) + data)
        empty = bytearray(header)
        struct.pack_into('<II', empty, 8, 0, 0)
        empty[64:96] = bytes(32)
        if ordinal:
            struct.pack_into('<I', empty, 104, 0)
        pending.append(bytes(empty))
    return pictures, pending


def run(evidence, clip, raw, fail_query=False):
    upload = host.preparation.upload_bytes(clip, None, raw)
    pictures, pending = frames(raw)
    record, saves, state = {}, [0], {'queries': [0, 0], 'reply': b'', 'ord': 0, 'acks': []}

    def save():
        saves[0] += 1
        host.supervisor.save_json(evidence / 'session.json', record)

    class Device:
        received = bytearray()

        def write(self, endpoint, packet, timeout):
            assert endpoint == 2 and timeout > 0
            if len(self.received) < len(upload):
                self.received.extend(packet)
                assert upload.startswith(self.received)
                return len(packet)
            if len(packet) == 45:
                saved = json.loads((evidence / 'session.json').read_text())
                assert saved['hevc_outputs'][-1]['ack_write_intent'] and not saved['hevc_outputs'][-1]['ack_sent']
                state['acks'].append(state['ord'])
                return len(packet)
            state['ord'] = packet[1]
            state['queries'][state['ord']] += 1
            if fail_query and state['queries'][0] == 50:
                return 0
            ready = state['queries'][state['ord']] > PENDING
            state['reply'] = pictures[state['ord']] if ready else pending[state['ord']]
            return len(packet)

        def read(self, endpoint, size, timeout):
            count = min(size, len(state['reply']))
            data, state['reply'] = state['reply'][:count], state['reply'][count:]
            return data
    clock = [1.0]
    try:
        result = host.capture_device(Device(), lambda: None, 1000, raw, evidence, record, save, upload=upload,
                                     now=lambda: clock[0], pause=lambda delay: None)
    except ValueError:
        result = None
    return result, record, saves[0], state


def main():
    clip = bytes((i * 7) & 255 for i in range(CLIP))
    raw = session(clip)
    with tempfile.TemporaryDirectory(prefix='audit-host-tools-hevc-') as directory, \
            patch.object(host.preparation, 'session_check', lambda value: None):
        evidence = Path(directory) / 'nominal'
        evidence.mkdir()
        result, record, saves, state = run(evidence, clip, raw)
        assert result, record.get('hevc_capture_failure')
        assert result['status'] == 1 and record['hevc_result'] == 'observed-success' and state['acks'] == [0, 1]
        # O(buffers and state changes), not O(512-byte packets) or O(polls).
        assert saves < 40, saves
        upload = [w for w in record['hevc_writes'] if w['kind'] == 'upload']
        assert len(upload) == 1 and upload[0]['accepted_bytes'] == CLIP + 32
        queries = [w for w in record['hevc_writes'] if w['kind'] == 'query']
        assert sum(1 + w.get('repeats', 0) for w in queries) == sum(state['queries'])
        pending = [f for f in record['hevc_frames'] if f['status'] == 0]
        assert [f['repeats'] for f in pending] == [PENDING - 1, PENDING - 1]
        for entry in pending:  # Copied pictures carry their pixel digest instead.
            assert hashlib.sha256((evidence / entry['path']).read_bytes()).hexdigest() == entry['sha256']
        files = list(evidence.glob('picture-*-*.bin')) + list(evidence.glob('fragment-*.bin'))
        assert len(files) < 80, len(files)
        # A failed repeated query write remains visible on its journal entry.
        failure = Path(directory) / 'failed-repeat'
        failure.mkdir()
        result, record, _, _ = run(failure, clip, raw, fail_query=True)
        saved = json.loads((failure / 'session.json').read_text())
        last = saved['hevc_writes'][-1]
        assert result is None and saved['hevc_result'] == 'unobserved'
        assert last['kind'] == 'query' and last['repeats'] == 49 and not last['last_accepted_bytes'], last
        assert saved['hevc_capture_failure']['message'] == 'Incomplete HEVC write.'
    print(f'PASS: {CLIP}-byte HEVC upload and {PENDING} pending polls per picture took {saves} journal saves; '
          'repeats are counted and failures stay journaled.')


if __name__ == '__main__':
    main()
