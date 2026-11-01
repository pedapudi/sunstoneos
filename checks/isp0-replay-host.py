#!/usr/bin/env python3
"""Check the shared host patch against actual linked ISP0 replay responses.

No USB runs. Capture uses native-produced response bytes and a modeled final
receipt; this checks host durability/sequence handling without physical authority.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('host-source', 'session', 'input', 'responses', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    # Execute the exact patch result with the production module location so
    # its existing local imports keep their ordinary repository boundaries.
    namespace = {'__file__': str(ROOT / 'apps/isp0/host.py'), '__name__': 'isp0_replay_host_checked'}
    exec(compile(args.host_source.read_text(), namespace['__file__'], 'exec'), namespace)
    session = args.session.read_bytes()
    raw = args.input.read_bytes()
    upload = namespace['preparation'].upload_bytes(raw, session)
    wires = [(args.responses / f'full-speed-{sequence}.bin').read_bytes() for sequence in range(1, 5)]
    results = {}
    for case in ('legacy', 'replay', 'previous-sequence', 'wrong-sequence', 'save-failure'):
        output = args.output / case
        output.mkdir()
        frames = 1 if case == 'legacy' else 4
        supplied = list(wires[:frames])
        if case == 'previous-sequence':
            supplied[1] = wires[0]
        if case == 'wrong-sequence':
            changed = bytearray(supplied[1]);struct.pack_into('<I', changed, 148, 3);supplied[1] = bytes(changed)
        final = bytearray(wires[frames - 1][:192])
        struct.pack_into('<2I', final, 8, 3, 0)
        supplied.append(bytes(final))
        record = {};acks = [];uploads = [];journals = []
        def save():
            journals.append(json.loads(json.dumps(record)))
        def exchange(packet, timeout):
            assert packet == b'\0' and timeout > 0
            return supplied.pop(0)
        def send_ack(packet):
            sequence = len(acks) + 1
            path = output / ('output.nv16' if frames == 1 else f'output-{sequence:04}.nv16')
            assert path.read_bytes() == wires[sequence - 1][192:]
            assert record['isp0_output']['ack_write_intent'] is True
            acks.append(packet)
        def send_upload(data):
            assert data == upload;uploads.append(data)
        original = namespace['supervisor'].durable_frame
        def durable(path, data):
            if case == 'save-failure' and path.name == 'output-0002.nv16':
                raise OSError('injected output durability failure')
            return original(path, data)
        namespace['supervisor'].durable_frame = durable
        try:
            result = namespace['capture'](exchange, session, output, record, save, 1000,
                upload=upload, send_upload=send_upload, send_ack=send_ack, frames=frames,
                now=lambda: 1, pause=lambda duration: None)
        except (ValueError, OSError) as error:
            assert case in ('previous-sequence', 'wrong-sequence', 'save-failure'), (case, error)
            assert len(acks) == 1 and record['isp0_result'] == 'unobserved'
            assert 'durable_result' not in record
        else:
            assert case in ('legacy', 'replay') and result['frames'] == frames
            assert len(acks) == frames and record['isp0_result'] == 'observed-success'
            assert all(value['ack_observed'] for value in record['isp0_outputs'])
            for sequence, packet in enumerate(acks, 1):
                if frames == 1:
                    assert packet == b'\1' + session[32:40] + wires[0][96:128]
                else:
                    assert packet == bytes([2 if sequence < frames else 3]) + session[32:40] + struct.pack('<Q', sequence) + wires[sequence - 1][96:128]
        finally:
            namespace['supervisor'].durable_frame = original
        assert len(uploads) == 1 and journals
        (output / 'journal.json').write_text(json.dumps(record, indent=2) + '\n')
        results[case] = dict(passed=True, acknowledgments=len(acks), uploads=len(uploads))
    report = dict(status='pass', cases=results, host_source_sha256=hashlib.sha256(args.host_source.read_bytes()).hexdigest(),
                  session_sha256=hashlib.sha256(session).hexdigest(), physical_access=False, scope=__doc__)
    (args.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS host replay sequences, durable outputs and partial failure retention')


if __name__ == '__main__':
    main()
