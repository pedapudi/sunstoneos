#!/usr/bin/env python3
"""Check host transport bounds with fixtures only; no USB, FEL or board access.

Covers size-aware FEL timeouts through the real preload and their refusal
before any transfer, the guarded identity lstat during observation, queue port
binding before the first FEL request, named lock contention and the console
summary printed before a retained hold, including a closed console.
"""
from contextlib import contextmanager, redirect_stdout
import errno
import fcntl
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import types
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


host = module('audit_transport_supervised', ROOT / 'tools/check-a733-smp-supervised.py')
base = host.base
MEASURED_BYTES_PER_SECOND = 570000  # 14.3 MB read in about 25 s at full speed.


def refused(call, text=''):
    try:
        call()
    except ValueError as error:
        assert text in str(error), (text, error)
        return
    raise AssertionError('Accepted: ' + text)


def timeouts(directory):
    small = directory / 'small.bin'
    small.write_bytes(bytes(128))
    assert base.fel_timeout(['version']) == base.fel_timeout(['readl', '0x02050014']) == 15
    assert base.fel_timeout(['write', '0x52c00', small, 'read', '0x52c00', 128, directory / 'r']) == 16
    for size in (14758784, 7501968, 6560592, 0x1200000):
        source = directory / f'segment-{size}.bin'
        source.write_bytes(bytes(size))
        bound = base.fel_timeout(['write', '0x41000000', source, 'read', '0x41000000', size, directory / 'r'])
        assert bound >= 15 + 2 * size / base.FEL_BYTES_PER_SECOND and bound <= base.FEL_TIMEOUT_LIMIT
        assert bound > 2 * 2 * size / MEASURED_BYTES_PER_SECOND  # Twice the measured transfer time.
        source.unlink()
    refused(lambda: base.fel_timeout(['read', '0x41000000', 80 * 1024 * 1024, directory / 'r']),
            'bounded transfer time')
    # An oversized later segment is refused before the first request.
    sizes = {'segment-0.bin': 4096, 'segment-1.bin': 40 * 1024 * 1024, 'result-zero.bin': 16384,
             'contract.bin': 4096, 'request.bin': 128, 'report-zero.bin': 128, 'launch.bin': 1024}
    oversized = {'segments': [{'name': 'segment-0.bin', 'address': 0x41000000},
                              {'name': 'segment-1.bin', 'address': 0x41100000}], '_sizes': sizes}
    refused(lambda: base.require_upload_bounds(oversized), 'bounded transfer time')
    calls = []
    refused(lambda: base.preload(lambda *a, **k: calls.append(a), directory, oversized, directory),
            'bounded transfer time')
    assert not calls
    sizes['segment-1.bin'] = 14758784
    base.require_upload_bounds(oversized)


def preload(directory):
    """Run the real preload through fel_request with a proportional fake FEL."""
    bundle = directory / 'bundle'
    bundle.mkdir()
    evidence = directory / 'preload-evidence'
    evidence.mkdir()
    prepared = {'files': {}, '_sizes': {}, 'segments': []}
    payloads = [('segment-0.bin', 0x41000000, bytes([0x5a]) * 4096),
                ('assets.bin', 0x43000000, bytes(range(256)) * (14758784 // 256)),
                ('result-zero.bin', 0x42000000, bytes(16384)), ('contract.bin', base.CONTRACT, bytes(4096)),
                ('request.bin', 0x52c00, bytes(128)), ('report-zero.bin', 0x52d00, bytes(128)),
                ('launch.bin', 0x52000, bytes(1024))]
    for name, address, data in payloads:
        (bundle / name).write_bytes(data)
        prepared['files'][name] = hashlib.sha256(data).hexdigest()
        prepared['_sizes'][name] = len(data)
        if name.startswith(('segment-', 'assets')):
            prepared['segments'].append({'name': name, 'address': address})
    memory, journal = {}, []
    node = Path('/fixture-usb/003/056')

    def fel(command, **kwargs):
        arguments = command[3:]
        moved, index = 0, 0
        while index < len(arguments):
            operation = arguments[index]
            if operation == 'write':
                data = Path(arguments[index + 2]).read_bytes()
                memory[int(arguments[index + 1], 16)] = data
                moved += len(data); index += 3
            elif operation == 'read':
                size = int(arguments[index + 2])
                data = memory.get(int(arguments[index + 1], 16), bytes(size))[:size]
                Path(arguments[index + 3]).write_bytes(data.ljust(size, b'\0'))
                moved += size; index += 4
            else:
                index = len(arguments)
        # Half the measured full-speed rate plus fixed request overhead.
        if 1 + moved / (MEASURED_BYTES_PER_SECOND / 2) > kwargs['timeout']:
            raise subprocess.TimeoutExpired(command, kwargs['timeout'])
        stdout = '0x00000000\n' if arguments[:1] == ['readl'] else ''
        return types.SimpleNamespace(returncode=0, stdout=stdout, stderr='')

    def transaction(*arguments, pass_fds=()):
        entry = {'arguments': [str(x) for x in arguments]}
        journal.append(entry)
        return base.fel_request(Path('/fixture/fel'), node, arguments, entry, pass_fds).stdout

    with patch.object(subprocess, 'run', fel):
        base.preload(transaction, bundle, prepared, evidence)
    large = next(e for e in journal if 'assets.bin' in ' '.join(e['arguments']) or
                 e['arguments'][:2] == ['write', hex(0x43000000)])
    assert large['timeout_seconds'] >= 15 + 2 * 14758784 / base.FEL_BYTES_PER_SECOND
    assert all(e['finished_monotonic'] >= e['started_monotonic'] for e in journal)
    assert (evidence / 'readback-assets.bin').read_bytes() == payloads[1][2]
    with patch.object(subprocess, 'run', lambda command, **kwargs: (_ for _ in ()).throw(
            subprocess.TimeoutExpired(command, kwargs['timeout']))):
        entry = {}
        try:
            base.fel_request(Path('/fixture/fel'), node, ['version'], entry)
        except subprocess.TimeoutExpired:
            assert entry['timeout_seconds'] == 15 and 'finished_monotonic' in entry
        else:
            raise AssertionError('Timeout was not propagated.')


def instances():
    node = Path('/fixture-usb/003/057')

    def access(error):
        def identity(_):
            if error:
                raise error
            return [1, 57]
        return types.SimpleNamespace(session_node=lambda config: node, identity=identity)
    assert base.present_instance(access(None), {}) == (node, [1, 57])
    for code in (errno.ENOENT, errno.ENODEV):
        assert base.present_instance(access(OSError(code, 'gone')), {}) == (None, None)
    assert base.present_instance(access(FileNotFoundError(errno.ENOENT, 'gone')), {}) == (None, None)
    try:
        base.present_instance(access(PermissionError(errno.EACCES, 'denied')), {})
    except PermissionError:
        pass
    else:
        raise AssertionError('Unexpected identity error was hidden.')
    empty = types.SimpleNamespace(session_node=lambda config: None, identity=None)
    assert base.present_instance(empty, {}) == (None, None)


def queue(path, port):
    with sqlite3.connect(path) as db:
        db.execute('CREATE TABLE settings(key TEXT PRIMARY KEY, value TEXT NOT NULL)')
        db.execute("INSERT INTO settings VALUES('halted','')")
        if port:
            db.execute("INSERT INTO settings VALUES('port',?)", (port,))


def observe(directory, scenario):
    """Drive run_session's observe() through a disconnect race, or refuse an unbound queue."""
    old, native = Path('/fixture-usb/003/056'), Path('/fixture-usb/003/057')
    port = directory / scenario / '3-1'
    port.mkdir(parents=True)
    for name, value in [('bcdDevice', '0100'), ('product', 'fixture product'), ('serial', 'fixture serial')]:
        (port / name).write_text(value)
    polls, events = [0], []

    def session_node(config):
        polls[0] += 1
        return old if polls[0] < 6 else native

    def identity(node):
        # The old node disappears between session_node and the second lstat.
        if node == old and 1 < polls[0] < 6:
            raise FileNotFoundError(errno.ENOENT, 'removed during disconnect', str(node))
        return [1, 56] if node == old else [1, 57]

    @contextmanager
    def board_lock():
        yield old
    access = types.SimpleNamespace(board_lock=board_lock, require_session=lambda node: None,
                                   require_rom=lambda node: None, identity=identity, target=lambda node: port,
                                   session_node=session_node, quarantine_instance=lambda *a: events.append('quarantine'))
    device = types.SimpleNamespace(bcdDevice=0x0100, set_configuration=lambda value: None)
    usb = types.ModuleType('usb'); core = types.ModuleType('usb.core'); usb.core = core
    core.find = lambda **kw: device
    guard = types.ModuleType('usb_guard'); guard.__file__ = str(directory / 'guard.py')
    (directory / 'guard.py').write_text('# Fixture only.\n')
    guard.check_health = lambda: None

    @contextmanager
    def reenumeration(*arguments):
        yield
    guard.reenumeration = reenumeration
    database = directory / scenario / 'queue.sqlite3'
    queue(database, {'wrong-port': '3-3', 'unbound': None}.get(scenario, '3-1'))
    requests = []

    def fel(*arguments, **kwargs):
        requests.append(arguments)
        return types.SimpleNamespace(returncode=0, stdout='soc=00001903(A733)', stderr='')

    def launch(transaction, observe, bundle, prepared, evidence, record, save, ready, *, result_key):
        ready()
        observe(0.0)
        record['complete'] = record[result_key] == 'observed-success'

    def capture(device, same, deadline, record, save):
        same(); events.append('capture'); record['board_result'] = 'observed-success'
    args = types.SimpleNamespace(fel=directory / 'fel', evidence=directory / scenario / 'evidence',
                                 claim='fixture', owner='fixture', database=database)
    args.fel.write_bytes(b'fixture')
    bundle = directory / scenario / 'bundle'
    bundle.mkdir()
    for name in ('admission.json', 'prepared.json'):
        (bundle / name).write_text('{}')
    clock = [0.0]

    def now():
        clock[0] += 0.01
        return clock[0]
    with patch.object(host, 'module', lambda *a: access), patch.object(base, 'FEL_HASH', base.digest(args.fel)), \
            patch.object(subprocess, 'run', fel), \
            patch.object(host, 'launch_once', launch), patch.object(host, 'finish_after_observation', lambda *a: None), \
            patch.object(base, 'stop_after_failure', lambda *a: events.append('stopped')), \
            patch.object(host.os, 'access', lambda *a: True), patch.object(host.time, 'monotonic', now), \
            patch.object(host.time, 'sleep', lambda value: None), \
            patch.dict(sys.modules, {'usb': usb, 'usb.core': core, 'usb_guard': guard}):
        try:
            code = host.run_session(args, bundle, {}, lambda: None, lambda identity, selected: events.append('entry'),
                                    capture, product='fixture product', serial='fixture serial', bcd_device=0x0100,
                                    capture_seconds=5, result_key='board_result')
        except ValueError as error:
            # Refused before any FEL request and without the quarantining handler.
            assert scenario != 'disconnect-race' and 'not bound to locked USB port 3-1' in str(error), error
            assert events == ['entry'] and not requests and polls[0] == 0, (events, requests)
            assert not (args.evidence / 'session.json').exists()
            return
    assert scenario == 'disconnect-race' and code == 0 and events == ['entry', 'capture'], events
    assert len(requests) == 1
    record = json.loads((args.evidence / 'session.json').read_text())
    assert record['observer_identity'] == [1, 57] and record['result_key'] == 'board_result'


def lock(directory):
    access = module('audit_transport_access', ROOT / 'tools/usb-access.py')
    port = directory / '3-1'
    port.mkdir()
    (port / 'idVendor').write_text('1f3a')
    original = access.board_lock_path
    with patch.object(access, 'board_lock_path', lambda name: directory / original(name).name), \
            patch.object(access, 'target', lambda node: port), patch.object(access, 'identity', lambda node: [1]):
        with access.board_lock(Path('/fixture/003/001')):
            try:
                with access.board_lock(Path('/fixture/003/002')):
                    raise AssertionError('Same port admitted twice.')
            except BlockingIOError as error:
                assert '3-1' in str(error) and str(directory / 'sunstoneos-a733-3-1.lock') in str(error), error
        with access.board_lock(Path('/fixture/003/003')):
            pass


def summary():
    events = []
    record = {'result_key': 'board_result', 'board_result': 'observed-success'}
    output = io.StringIO()
    with redirect_stdout(output):
        host.retain_after_capture(record, lambda: events.append('save'),
                                  lambda: events.append('quarantine:' + output.getvalue()),
                                  lambda: events.append('hold'))
    assert events[0] == 'save' and events[2] == 'hold'
    assert 'Diagnostic: observed-success. Manual reconnection required.' in events[1]

    # A closed console must not skip the persistent quarantine.
    class Closed(io.StringIO):
        def write(self, text):
            raise BrokenPipeError(errno.EPIPE, 'closed')
    events.clear()
    with redirect_stdout(Closed()):
        host.retain_after_capture(record, lambda: events.append('save'), lambda: events.append('quarantine'),
                                  lambda: events.append('hold'))
    assert events == ['save', 'quarantine', 'hold'], events


def generations():
    caller = module('audit_transport_mimus_board', ROOT / 'tools/check-a733-mimus-board-supervised.py')
    for count in range(1, caller.MAX_GENERATIONS + 1):
        caller.generation_count(count, 4)
    for count, workload in [(caller.MAX_GENERATIONS + 1, 4), (32, 4), (2, 0)]:
        refused(lambda: caller.generation_count(count, workload))


def main():
    with tempfile.TemporaryDirectory(prefix='audit-host-tools-transport-') as directory:
        directory = Path(directory)
        timeouts(directory)
        preload(directory)
        instances()
        observe(directory, 'disconnect-race')
        observe(directory, 'wrong-port')
        observe(directory, 'unbound')
        lock(directory)
        summary()
        generations()
    print('PASS: size-aware FEL timeouts through preload and before any transfer, disconnect-race absence, '
          'queue port binding before any FEL request, named lock contention, summary before hold with a closed '
          'console and lifetime-bounded generations; no device access.')


if __name__ == '__main__':
    main()
