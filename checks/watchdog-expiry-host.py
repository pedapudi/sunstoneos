"""Check watchdog host request boundaries and restoration without USB access."""
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
from unittest.mock import patch

path = Path(__file__).resolve().parents[1] / 'tools/check-watchdog-expiry.py'
spec = importlib.util.spec_from_file_location('expiry_runner', path)
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


def refused(callback):
    try:
        callback()
    except (ValueError, RuntimeError):
        return
    raise AssertionError('Expected refusal')


with tempfile.TemporaryDirectory(prefix='watchdog-expiry-host-') as directory:
    root = Path(directory)
    image = b'current image fixture'
    image_path = root / 'image.bin'
    image_path.write_bytes(image)
    before = [0xa2, 0x22, 7, 0, 0x12340000, 0, 0x76540000, 0x10203040]
    observations = []

    def exercise(phase, fail=None, corrupt=False):
        calls, saved, returns = [], [], []
        record = {}

        def save():
            saved.append(json.loads(json.dumps(record)))

        def transaction(*arguments, **options):
            calls.append((arguments, options))
            if len(calls) == fail:
                raise RuntimeError('Injected transport failure')
            if arguments[0] == 'write':
                (root / 'uploaded.bin').write_bytes(image if not corrupt else b'corrupted')
                (root / 'uploaded-request.bin').write_bytes((root / 'request.bin').read_bytes())
            if arguments[0] == 'exe':
                assert saved[-1]['execution_intent']['command'] == (1 if phase == 'arm-disarm' else 2)
            if arguments[0] == 'read':
                values = list(struct.unpack('<16I', (root / 'request.bin').read_bytes()))
                values[4:10] = [0x57525032, 0, 5, 0x43, 0, 0]
                (root / 'result.bin').write_bytes(struct.pack('<16I', *values))

        def wait_return(started):
            assert started == 100
            returns.append(True)

        with patch.object(runner.time, 'monotonic', side_effect=[100, 104]):
            operation = lambda: runner.run_phase(phase, transaction, root, image_path, image,
                                                 b'', before, wait_return, record, save)
            if fail or corrupt:
                refused(operation)
                assert len(calls) == (fail if fail else 1)
                assert not returns
            else:
                operation()
                assert len(calls) == (4 if phase == 'arm-disarm' else 3)
                assert bool(returns) == (phase == 'expiry')
                assert sum(arguments[0] == 'exe' for arguments, _ in calls) == 1
                assert calls[1][1] == {'low_input': True}
        observations.append({'phase': phase, 'failure_at_request': fail, 'upload_corrupt': corrupt})

    exercise('arm-disarm')
    exercise('expiry')
    exercise('expiry', corrupt=True)
    for phase, request_count in (('arm-disarm', 4), ('expiry', 3)):
        for request in range(1, request_count + 1):
            exercise(phase, fail=request)
    restored = []
    runner.restore_registers(lambda *args, **kwargs: restored.append((args, kwargs)), before,
                             [0, 1, 2, 0xfa50392f, 0x20, 0, before[6], 0x5aa5a55a])
    assert restored[0][0][:3] == ('writel', '0x070501c0', '0')
    assert restored[0][1] == {'expected_words': [0]}
    assert restored[1][0][:3] == ('writel', '0x070501c4', hex(before[4]))
    for index, value in ((0, 1), (3, 0xdeadbeef), (4, 0xbad), (5, 0xefe8), (6, 0)):
        observed = before.copy()
        observed[index] = value
        writes = []
        refused(lambda: runner.restore_registers(lambda *args, **kwargs: writes.append(args), before, observed))
        assert not writes
    calls = []

    def fail_disable(*args, **kwargs):
        calls.append(args)
        raise ValueError('Redirect disable did not read back')

    refused(lambda: runner.restore_registers(fail_disable, before, before))
    assert len(calls) == 1
    print(json.dumps({'cases': observations, 'restoration': 'ordering and owner refusal passed',
                      'physical_access': False}, indent=2))

# Exercise the main runner's reservation and terminal-failure path with a local
# fake ROM transport. No real access helper, device node, or shared lock is used.
from contextlib import contextmanager
from types import SimpleNamespace
import io
from contextlib import redirect_stdout
import sys
import builtins

with tempfile.TemporaryDirectory(prefix='watchdog-main-host-') as directory:
    root = Path(directory)
    fake_port = root / '3-1'
    fake_port.mkdir()
    node = Path('/virtual/003/064')
    identity = [1, 2, 3, 4, 5]
    image_path, rom_path, fel_path = (root / name for name in ('image', 'rom', 'fel'))
    image_path.write_bytes(b'image fixture')
    rom_path.write_bytes(bytes(0x11000))
    fel_path.write_bytes(b'tool fixture')
    import hashlib
    rom_hash = hashlib.sha256(rom_path.read_bytes()).hexdigest()
    real_hash = runner.hash_file
    state = {'memory': {}, 'transactions': [], 'reservations': [], 'quarantine': []}

    def fake_run(command, **kwargs):
        arguments = list(command[3:])
        state['transactions'].append(arguments.copy())
        output = []
        while arguments:
            operation = arguments.pop(0)
            if operation == 'version':
                output.append('soc=00001903(A733)')
            elif operation == 'read':
                address, length, destination = arguments[:3]
                del arguments[:3]
                address, length = int(address, 0), int(length)
                Path(destination).write_bytes(state['memory'].get(address, bytes(length))[:length])
            elif operation == 'write':
                address, source = arguments[:2]
                del arguments[:2]
                state['memory'][int(address, 0)] = Path(source).read_bytes()
            elif operation == 'writel':
                del arguments[:2]
            elif operation == 'memmove':
                destination, source, length = map(lambda value: int(value, 0), arguments[:3])
                del arguments[:3]
                state['memory'][destination] = bytes(length)
            elif operation == 'readl':
                address = int(arguments.pop(0), 0)
                values = dict(zip(runner.REGISTERS, [0, 1, 0x1f, 0, 0, 0, 0, 0]))
                values[0x03000024] = 0x100 if state.get('high_input') else 0
                output.append(f'{values[address]:08x}')
            else:
                raise AssertionError(f'Unexpected preflight command: {operation}')
        return SimpleNamespace(returncode=0, stdout=' '.join(output), stderr='')

    @contextmanager
    def reserve(*arguments):
        outcome = {'count': arguments[-1]}
        state['reservations'].append(outcome)
        yield outcome

    access = SimpleNamespace(discover=lambda: node, identity=lambda value: identity,
                             require_session=lambda value: None, require_rom=lambda value: None,
                             target=lambda value: fake_port,
                             quarantine_instance=lambda *args: state['quarantine'].append(args))
    guard = SimpleNamespace(check_health=lambda: None, reenumeration=reserve)
    for phase in ('arm-disarm', 'expiry'):
        state.update(memory={}, transactions=[], reservations=[], quarantine=[])
        evidence = root / phase
        argv = [str(path), '--phase', phase, '--confirm-bridge-stable', '--fel', str(fel_path), '--rom', str(rom_path),
                '--image', str(image_path), '--evidence', str(evidence)]

        def malformed_result(*args):
            assert len(state['reservations']) == 1
            state['at_failure'] = len(state['transactions'])
            raise struct.error('Synthetic truncated diagnostic result')

        @contextmanager
        def fixture_lock():
            yield node
        access.board_lock = fixture_lock

        with patch.object(sys, 'argv', argv), patch.dict(sys.modules, {'usb_guard': guard}), \
             patch.object(runner, 'load_module', return_value=access), \
             patch.object(runner, 'ROM_HASH', rom_hash), \
             patch.object(runner.subprocess, 'run', side_effect=fake_run), \
             patch.object(runner, 'run_phase', side_effect=malformed_result):
            try:
                runner.main()
            except SystemExit as error:
                assert 'Synthetic truncated' in str(error)
            else:
                raise AssertionError('Malformed result did not stop runner')
        assert len(state['transactions']) == state['at_failure']
        assert len(state['quarantine']) == 1 and state['reservations'][0]['count'] == 1
        saved = json.loads((evidence / 'session.json').read_text())
        assert saved['complete'] is False and 'Synthetic truncated' in saved['failure']
    for phase, high in (('arm-disarm', False), ('expiry', False), ('expiry', True)):
        state.update(memory={}, transactions=[], reservations=[], quarantine=[], high_input=high)
        evidence = root / (phase + '-existing-' + str(high))
        evidence.mkdir()
        sentinel = evidence / 'earlier.log'
        sentinel.write_text('preserve earlier evidence')
        argv = [str(path), '--phase', phase, '--confirm-bridge-stable', '--fel', str(fel_path),
                '--rom', str(rom_path), '--image', str(image_path), '--evidence', str(evidence)]
        @contextmanager
        def fixture_lock():
            yield node
        access.board_lock = fixture_lock
        def completed(*args):
            assert state['reservations']
        with patch.object(sys, 'argv', argv), patch.dict(sys.modules, {'usb_guard': guard}), \
             patch.object(runner, 'load_module', return_value=access), \
             patch.object(runner, 'ROM_HASH', rom_hash), \
             patch.object(runner.subprocess, 'run', side_effect=fake_run), \
             patch.object(runner, 'run_phase', side_effect=completed):
            if high:
                try: runner.main()
                except SystemExit as error: assert 'not consistently low' in str(error)
                else: raise AssertionError('High input accepted')
            else:
                runner.main()
        assert sentinel.read_text() == 'preserve earlier evidence'
        sessions = list(evidence.glob('*/session.json'))
        assert len(sessions) == 1
        saved = json.loads(sessions[0].read_text())
        assert saved['complete'] is (not high)
        assert saved['image_sha256'] == hashlib.sha256(image_path.read_bytes()).hexdigest()
        if high: assert not state['reservations'] and state['quarantine']
        else: assert saved['input_inspected_in_run'] and not state['quarantine']
    print('PASS: main-runner phases accept current images without prior records, preserve evidence and refuse high input.')
