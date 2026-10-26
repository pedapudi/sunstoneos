#!/usr/bin/env python3
"""Check exact-sum bundle admission, capture parsing and the actual runner entry offline."""
import argparse
from contextlib import contextmanager
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'checks'))
import harness


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def refuse(action, reason):
    try:
        action()
    except ValueError as error:
        assert reason in str(error), error
    else:
        raise AssertionError('accepted: ' + reason)


def runner_case(runner, out, image, log, callback, accepted):
    """Run main with a complete modeled upload, returned ROM and result read."""
    out.mkdir()
    initial, returned = out / '001' / '001', out / '001' / '002'
    initial.parent.mkdir()
    initial.write_bytes(b''); returned.write_bytes(b'')
    port = out / 'port-3-1'; port.mkdir()
    fel = out / 'fel'; fel.write_bytes(b'fixture executable identity')
    evidence = out / 'capture'
    calls = []

    @contextmanager
    def locked(*args):
        yield initial

    @contextmanager
    def reenumeration(*args):
        yield

    access = SimpleNamespace(board_lock=locked, require_session=lambda n: None,
        require_rom=lambda n: None, identity=lambda n: {'node': str(n)}, target=lambda n: port,
        session_node=lambda config: returned,
        quarantine_instance=lambda *args: calls.append('quarantine'))

    def stopped(record, save, quarantine, error, hold):
        calls.append('retain'); quarantine(); save(); hold()

    def hold():
        raise AssertionError('unexpected unresolved return')

    failure = SimpleNamespace(stop_after_failure=stopped, hold_ownership=hold)
    guard = SimpleNamespace(check_health=lambda: None, reenumeration=reenumeration)
    result = bytearray(16384)
    encoded = log.encode()
    assert len(encoded) <= 16384 - 257
    struct.pack_into('<16Q', result, 0, 0x534b4552, 5, 0, len(encoded), *([0]*12))
    result[256:256+len(encoded)] = encoded

    def transaction(command, **kwargs):
        args = command[3:]
        if args == ['version']:
            return subprocess.CompletedProcess(command, 0, 'soc=00001903(A733)', '')
        if args[0] == 'readl':
            return subprocess.CompletedProcess(command, 0, 'eb00008f e3a00911 e3a0193d e3a0605c ea00004d', '')
        i = 0
        while i < len(args):
            op = args[i]
            if op == 'read':
                _, address, size, path = args[i:i+4]
                if Path(path).name == 'before.bin': data = struct.pack('<I', 0x534b4552) + bytes(124)
                elif Path(path).name == 'uploaded.bin': data = image.read_bytes()
                else: data = bytes(result)
                Path(path).write_bytes(data); i += 4
            elif op == 'write': i += 3
            elif op == 'reset64': i += 2
            else: raise AssertionError(args)
        return subprocess.CompletedProcess(command, 0, '', '')

    with patch.object(runner, 'load_module', side_effect=lambda name, path: access if name == 'access' else failure), \
         patch.object(runner.subprocess, 'run', side_effect=transaction), patch.dict(sys.modules, {'usb_guard': guard}):
        try:
            runner.main(['--fel', str(fel), '--image', str(image), '--evidence', str(evidence),
                         '--expect', 'COMPLETE GPU exact sums:', '--mode', 'kernel'], validate_result=callback)
        except SystemExit as error:
            assert not accepted and 'Application result rejected' in str(error), error
    record = json.loads((evidence / 'session.json').read_text())
    assert record['complete'] is accepted and record['transport_failed'] is False
    assert calls == ([] if accepted else ['retain', 'quarantine'])
    if callback:
        assert record['application_result']['accepted'] is accepted
    else:
        assert 'application_result' not in record
    return {'case': out.name, 'complete': record['complete'], 'transport_failed': record['transport_failed'],
            'failure_calls': calls}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build', type=Path, required=True)
    p.add_argument('--native', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args(); a.output.mkdir(parents=True, exist_ok=False)
    host = load('exact_sum_host', ROOT / 'apps/gpu-power-id/mimus-host.py')
    prepare = load('exact_sum_prepare', ROOT / 'apps/gpu-power-id/prepare.py')
    runner = load('kernel_runner', ROOT / 'tools/check-a733-kernel.py')
    native = json.loads((a.native / 'good.json').read_text())
    log = ''.join(native['messages'])
    terminal = {'magic': 0x534b4552, 'stage': 5, 'status': native['exit_status']}
    assert host.validate_result(terminal, log)['accepted']
    prefix = 'GPU exact sums '
    line = next(x for x in log.splitlines() if x.startswith(prefix))
    refuse(lambda: host.validate_result(terminal, log + line + '\n'), 'duplicate')
    refuse(lambda: host.validate_result(terminal, log.replace(line+'\n', '')), 'missing')
    refuse(lambda: host.validate_result(terminal, log.replace('sums=1122', 'sums=oops')), 'malformed')
    refuse(lambda: host.validate_result(terminal, log.replace('bad_sums=0', 'bad_sums=1')), 'incorrect')
    refuse(lambda: host.validate_result({**terminal, 'status': 1}, log), 'terminal')
    checked = json.loads((a.native / 'results.json').read_text())
    build = json.loads(a.build.read_text())
    assert checked['image_sha256'] == build['image_sha256']
    row = {'name': 'gpu-exact-sum', 'variant': 'fw-mimus', 'expect': host.EXPECTED}
    bundle = a.output / 'bundle'
    admitted = prepare.prepare_bundle(row, a.build, a.native / 'results.json', bundle)
    refuse(lambda: prepare.validate({**row, 'variant': 'fw-job'}, bundle), 'variant')
    frozen_host = load('frozen_exact_host', bundle / 'host/tools/check-a733-kernel.py')
    dispatched = []
    delegate = SimpleNamespace(main=lambda argv, validate_result: dispatched.append((argv, validate_result)))
    loader = SimpleNamespace(exec_module=lambda module: None)
    with patch.object(frozen_host.importlib.util, 'spec_from_file_location', return_value=SimpleNamespace(loader=loader)), \
         patch.object(frozen_host.importlib.util, 'module_from_spec', return_value=delegate):
        frozen_host.main(['--image', str(bundle / 'probe.bin'), '--mode', 'kernel', '--expect', host.EXPECTED])
        frozen_host.main(['--image', str(bundle / 'probe.bin')])
        refuse(lambda: frozen_host.main(['--image', str(bundle / 'probe.bin'), '--mode', 'timeout']), 'host mode')
        wrong = a.output / 'wrong.bin'; wrong.write_bytes(b'wrong image')
        refuse(lambda: frozen_host.main(['--image', str(wrong)]), 'image identity')
    assert len(dispatched) == 2 and all(callback is frozen_host.validate_result and
        argv[-2:] == ['--expect', host.EXPECTED] for argv, callback in dispatched)
    result = [runner_case(runner, a.output / name, bundle / 'probe.bin', text, callback, accept)
        for name, text, callback, accept in [
            ('default', log, None, True), ('accepted', log, host.validate_result, True),
            ('rejected', log.replace('bad_sums=0', 'bad_sums=1'), host.validate_result, False)]]
    (a.output / 'results.json').write_text(json.dumps({'physical_access': False, 'admission': admitted,
        'parser': ['good', 'duplicate', 'missing', 'malformed', 'wrong-sum', 'wrong-terminal'],
        'runner': result}, indent=2)+'\n')
    print('PASS exact-sum bundle, captured reports and default/accepted/rejected runner entry')


if __name__ == '__main__':
    main()
