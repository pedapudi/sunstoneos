#!/usr/bin/env python3
"""Exercise combined coordinator capture with fake transport and claim records.

No device, USB, queue database, native execution or compiler is accessed.
Existing prepared bytes are read; all authority used by the claim fixture is
explicitly modeled. The real supervisor entry check stops before readiness.
"""
import argparse
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import struct
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('combined_supervised_host', ROOT / 'tools/check-a733-mimus-board-supervised.py')
caller = importlib.util.module_from_spec(spec)
spec.loader.exec_module(caller)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', type=Path, required=True)
    parser.add_argument('--fel', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    results = {}
    prepared = caller.freeze_bundle(args.bundle, 2)
    assert prepared['workload'] == 4
    session = prepared['_combined_frozen']['board/session.bin']
    epoch, attachment = struct.unpack_from('<2Q', session, 24)
    trigger = bytearray((args.bundle.parent.parent / 'native-keyboard.trigger-frame.bin').read_bytes()[64:])
    # Use the accepted native producer's trigger bytes, changing only its modeled
    # attachment generation to match the accepted preparation fixture.
    struct.pack_into('<Q', trigger, 16, attachment)
    text = b'FORMATTED copied immutable generation output.\n' * 27
    sector = bytes(range(256)) * 2
    current = None

    class FakeDevice:
        def __init__(self, folder, record, mode='ok'):
            self.folder, self.record, self.mode = folder, record, mode
            self.generation = 1
            self.writes, self.reads = [], 0
            self.response, self.op = bytearray(), None
            self.clock = 1
            self.single_ack = False

        def write(self, endpoint, packet, timeout):
            assert endpoint == 2 and timeout > 0
            if self.mode == 'journal-budget': assert timeout == 1
            op, zero, count, offset, expected = struct.unpack('<BBHIQ', packet)
            assert not zero and expected == (self.generation if op else 0)
            self.writes.append((self.generation, op))
            if self.mode == 'second-fault' and self.generation == 2:
                raise OSError('modeled second-generation transport failure')
            if op == 4:
                # The real durable save and files must precede the ACK write.
                saved = json.loads((self.folder / 'session.json').read_text())
                value = saved['recognitions'][-1]
                assert value['state'] == 'copied-before-ack'
                assert set(value['copied_outputs']) == {'transcript.txt', 'sector.bin', 'trigger.bin'}
                for item in value['copied_outputs'].values():
                    assert caller.digest(self.folder / item['path']) == item['sha256']
                for item in value['frames'] + value['fragments']:
                    assert caller.digest(self.folder / item['path']) == item['sha256']
                assert sum(struct.unpack_from('<I', (self.folder / item['path']).read_bytes(), 28)[0] == 1
                           for item in value['frames']) == 2
            body = text[offset:offset + count] if op == 1 else sector if op == 2 else bytes(trigger) if op == 3 else b''
            observed = self.generation + (1 if self.mode == 'wrong-generation' else 0)
            self.response = bytearray(struct.pack('<8I2QQII', 0x424d494d, 3, 1, len(body), len(text), offset,
                1, op, epoch, attachment, observed, 0xffffffff, 0) + body)
            self.op = op
            return len(packet)

        def read(self, endpoint, size, timeout):
            assert endpoint == 0x81 and timeout > 0
            self.reads += 1
            if self.mode == 'fragment-fault' and self.reads == 2:
                raise OSError('modeled response fragment failure')
            count = min(1088 if self.single_ack and self.op == 4 else 29, size, len(self.response))
            data = bytes(self.response[:count])
            del self.response[:count]
            if not self.response and self.op == 4:
                self.generation += 1
                if self.mode == 'deadline':
                    self.clock = 101
            return data

    def run(name, mode='ok', failure=None, count=2):
        nonlocal current
        folder = args.output / name
        folder.mkdir()
        record = {}
        state = copy.copy(prepared)
        state['_recognitions'] = count
        device = FakeDevice(folder, record, mode)
        device.single_ack = failure == 'ack-single-save'
        failed_once = False
        real_durable = caller.supervisor.durable_frame
        def durable(path, data):
            nonlocal failed_once
            if failure == 'raw' and '-frame-' in path.name and not failed_once:
                failed_once = True
                raise OSError('modeled complete-frame durability failure')
            if failure == 'output' and path.name.endswith('sector.bin') and not failed_once:
                failed_once = True
                raise OSError('modeled copied-output durability failure')
            return real_durable(path, data)
        def save():
            nonlocal failed_once
            value = record.get('recognitions', [{}])[-1] if record.get('recognitions') else {}
            hit = (failure == 'pre-ack-save' and value.get('state') == 'copied-before-ack' or
                   failure == 'post-ack-save' and value.get('ack_response_observed', {}).get('path') or
                   failure in ('ack-fragment-save', 'ack-single-save') and value.get('ack_response_observed'))
            if hit and not failed_once:
                failed_once = True
                raise OSError('modeled publication failure')
            if mode == 'journal-budget' and value.get('requests'): device.clock = 99.999
            caller.supervisor.save_json(folder / 'session.json', record)
        caller.supervisor.durable_frame = durable
        try:
            caller.capture_device(device, lambda: None, 100, state, folder, record, save,
                                  now=lambda: device.clock, pause=lambda _: None)
            assert mode in ('ok', 'journal-budget') and failure is None
            assert record['mimus_board_result'] == 'observed-success'
            assert record['durable_result']['recognition_generations'] == list(range(1, count + 1))
            assert device.writes.count((1, 4)) == 1 and device.writes.count((2, 4)) == (count == 2)
        except (OSError, ValueError):
            assert mode != 'ok' or failure is not None
            assert record['mimus_board_result'] == 'observed-failure' and 'durable_result' not in record
            if failure in ('raw', 'output', 'pre-ack-save'):
                assert all(op != 4 for _, op in device.writes)
            if failure in ('post-ack-save', 'ack-fragment-save', 'ack-single-save'):
                assert device.writes[-1] == (1, 4)
                assert record['recognitions'][0]['ack_response_observed']
                assert len(record['recognitions']) == 1
            if mode == 'fragment-fault':
                assert len(device.writes) == 1 and len(record['recognitions'][0]['fragments']) == 1
            if mode == 'second-fault':
                assert record['capture_failure']['completed_generations'] == [1]
                assert record['recognitions'][0]['state'] == 'acknowledged'
                assert record['recognitions'][0]['copied_outputs']
            if mode == 'deadline':
                assert device.writes[-1] == (1, 4) and not any(g == 2 for g, _ in device.writes)
        finally:
            caller.supervisor.durable_frame = real_durable
        current = record
        results[name] = True

    run('two-generations')
    for name in ('raw', 'output', 'pre-ack-save', 'post-ack-save', 'ack-fragment-save', 'ack-single-save'):
        run(name, failure=name)
    for mode in ('fragment-fault', 'second-fault', 'wrong-generation', 'deadline', 'journal-budget'):
        run(mode, mode=mode)
    with caller.launch_view(prepared) as (view, launch):
        assert (view / 'prepared.json').read_bytes() == prepared['_combined_frozen']['board/prepared.json']
        assert caller.digest(view / 'combined-launch-view.json') == launch['_launch_plan_sha256']
        assert launch['segments'][-3:] == [dict(name=item['name'], address=item['address'])
            for item in json.loads(prepared['_combined_frozen']['assets/assets.json'])['assets']]
        assert all(caller.digest(view / name) == identity for name, identity in launch['files'].items())
        results['exact_frozen_launch_view_and_assets'] = True
    for count, workload in [(0, 4), (33, 4), (2, 0), (True, 4)]:
        try:
            caller.generation_count(count, workload)
        except ValueError:
            pass
        else:
            raise AssertionError('unsupported count accepted')
    results['bounded_generations'] = True

    # A fake connection exercises the real read-only claim validator without
    # opening or creating any queue database.
    model = args.output / 'claim-model'
    model.mkdir()
    def ref(path):
        return dict(path=str(path.resolve()), sha256=caller.digest(path))
    proof = model / 'software-model.json'
    proof.write_text(json.dumps({'schema': 'a733-mimus-board-supervised-software-v1',
        'physical_access': False, 'admission_cases': True, 'launcher_cases': True,
        'host_capture': True, 'host_lifecycle': True,
        'image': ref(args.bundle / 'board/image.elf'), 'launcher': ref(args.bundle / 'board/launch.bin'),
        'sources': {name: ref(path) for name, path in caller.software_sources().items()},
        'evidence': [ref(Path(__file__))], 'fixture_only': 'Not board admission or queue submission.'}))
    prereqs = {'supervised-ram-recovery': ref(args.bundle / 'board/admission.json'),
               'mimus-board-supervised-software': ref(proof)}
    submission = {'prerequisites': list(prereqs), 'artifacts': [ref(args.bundle / name)
        for name in prepared['_combined_files']]}
    row = dict(state='claimed', owner='model-owner', submission=json.dumps(submission),
               review=json.dumps({'prerequisites': prereqs}))
    class Query:
        def __init__(self, value): self.value = value
        def fetchone(self): return self.value
    class Connection:
        def execute(self, sql, parameters=()):
            if 'settings' in sql: return Query([''])
            assert parameters == ('model-claim',)
            return Query(row)
        def close(self): pass
    def connect(database_uri, **kwargs):
        assert database_uri.endswith('?mode=ro') and kwargs == {'uri': True}
        return Connection()
    original = caller.sqlite3.connect
    caller.sqlite3.connect = connect
    try:
        for case in ('accepted', 'owner', 'missing-asset', 'recovery'):
            candidate = copy.deepcopy(row)
            if case == 'owner': row['owner'] = 'different'
            if case == 'missing-asset':
                changed = copy.deepcopy(submission)
                changed['artifacts'] = [v for v in changed['artifacts'] if not v['path'].endswith('/asset-1.bin')]
                row['submission'] = json.dumps(changed)
            if case == 'recovery':
                changed = copy.deepcopy(prereqs)
                changed['supervised-ram-recovery'] = ref(proof)
                row['review'] = json.dumps({'prerequisites': changed})
            try:
                caller.claimed(model / 'never-opened.sqlite', 'model-claim', 'model-owner', args.bundle, prepared)
                assert case == 'accepted'
            except ValueError:
                assert case != 'accepted'
            row.clear(); row.update(candidate)
            results['claim-' + case] = True
    finally:
        caller.sqlite3.connect = original
    assert not (model / 'never-opened.sqlite').exists()
    # Actual shared policy and FEL-byte check stop before claim/evidence/access.
    class Ready(Exception): pass
    def ready(): raise Ready
    def forbidden(*unused): raise AssertionError('device or claim callback reached')
    unused = args.output / 'entry-must-not-exist'
    try:
        caller.supervisor.run_session(SimpleNamespace(fel=args.fel, evidence=unused), unused, {},
            ready, forbidden, forbidden, product=caller.PRODUCT, serial=caller.SERIAL,
            bcd_device=0x0100, capture_seconds=59, result_key='mimus_board_result')
    except Ready:
        pass
    else:
        raise AssertionError('readiness boundary not reached')
    assert not unused.exists()
    results['actual-supervisor-entry-before-access'] = True
    record = {'passed': True, 'physical_access': False, 'scope': __doc__,
              'caller_sha256': caller.digest(Path(caller.__file__)), 'cases': results}
    (args.output / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(record))


if __name__ == '__main__':
    main()
