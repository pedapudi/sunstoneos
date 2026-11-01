#!/usr/bin/env python3
"""Exercise SD 1.8 V consent and retained ownership with fake host access only."""
import argparse
from contextlib import contextmanager, redirect_stdout
import importlib.util
import io
import json
from pathlib import Path

import harness
import struct
import sys
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    value = importlib.util.module_from_spec(spec); spec.loader.exec_module(value)
    return value


class Held(BaseException):
    pass


def consent_refused(call):
    try:
        call()
    except (ValueError, SystemExit) as error:
        assert '--allow-sd-1v8' in str(error), str(error)
    else:
        raise AssertionError('SD profile 3 accepted without --allow-sd-1v8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args(); args.output.mkdir(parents=True, exist_ok=False)
    producer = load('sd_voltage_returning', 'tools/storage-read-returning.py')
    config = {'fabric': [0x03000002, 0x83000000, 0x84000000],
              'fabric_hz': [200000000, 600000000, 400000000],
              'pmic_pins': [[2, 1, 1]] * 2, 'expected_pins': [[0, 1, 0]] * 7,
              'desired_pins': [[2, 1, 0]] * 6 + [[0, 1, 0]]}
    config_path = args.output / 'config.json'; config_path.write_text(json.dumps(config))
    # This is the production input command. Refusal precedes even parent creation.
    denied = args.output / 'denied' / 'input.bin'
    input_args = ['input', '--config', str(config_path), '--profile', '3', '--output', str(denied)]
    consent_refused(lambda: producer.main(input_args))
    assert not denied.parent.exists()
    producer.main(input_args + ['--allow-sd-1v8'])
    assert producer.parse_input(denied.read_bytes())['profile'] == 3
    input2 = args.output / 'input2.bin'
    producer.main(['input', '--config', str(config_path), '--output', str(input2)])
    assert producer.parse_input(input2.read_bytes())['profile'] == 2

    # Model only image/ABI/build dependencies; exercise actual prepare, bundle
    # validation and supervised main. No compiler, native image or USB is used.
    prep = load('sd_voltage_prepare', 'apps/storage-read/prepare.py')
    image = args.output / 'image.elf'; image.write_bytes(b'modeled ELF')
    admission = args.output / 'admission.json'; admission.write_text('{"reservation":1}')
    template = args.output / 'template.bin'
    raw = bytearray(880); struct.pack_into('<3Q', raw, 592, 0x4153373352444131, 2, 880)
    struct.pack_into('<I', raw, 860, 3); template.write_bytes(raw)
    bundle = args.output / 'bundle'
    def launcher(out, clang, lld): (out / 'launch.bin').write_bytes(b'modeled launch')
    with patch.object(prep, 'session_check', lambda raw: None), \
         patch.object(prep, 'embedded_reference', lambda *a: None), \
         patch.object(prep, 'image_manifest_check', lambda *a: None), \
         patch.object(prep, 'validate_admission', lambda *a: {}), \
         patch.object(prep, 'build_launcher', launcher), \
         patch.object(prep.base, 'image_parts', lambda image: ([], 0x41000000, bytes(range(32)))), \
         patch.object(prep, 'request_bytes', lambda *a: (b'request', b'contract')):
        prepare_args = (image, None, admission, template, bundle, 'unused-clang', 'unused-lld')
        consent_refused(lambda: prep.prepare(*prepare_args))
        assert not bundle.exists()
        prep.prepare(*prepare_args, allow_sd_1v8=True)
        consent_refused(lambda: prep.validate_bundle(bundle))
        assert prep.validate_bundle(bundle, allow_sd_1v8=True)['_frozen']['session.bin']
        supervised = load('sd_voltage_supervised', 'tools/check-a733-storage-read-supervised.py')
        calls = []
        def run_session(*a, **kw): calls.append(kw); return 0
        argv = ['--bundle', str(bundle), '--fel', 'unused', '--evidence', str(args.output/'supervised'),
                '--supervised-manual-reconnect']
        with patch.object(supervised, 'preparation', prep), \
             patch.object(supervised.supervisor, 'run_session', run_session):
            consent_refused(lambda: supervised.main(argv))
            assert not calls and not (args.output/'supervised').exists()
            assert supervised.main(argv + ['--allow-sd-1v8']) == 0
            assert calls[0]['power_removal_reason']
        # A normal 3.3 V bundle remains usable with the original call signatures.
        struct.pack_into('<I', raw, 860, 2); template.write_bytes(raw)
        normal = args.output/'bundle2'
        prep.prepare(image, None, admission, template, normal, 'unused', 'unused')
        prep.validate_bundle(normal)
        # The eMMC wrapper reuses these functions with a different session ABI.
        emmc = bytearray(912); struct.pack_into('<Q', emmc, 592, 0x4153373345524131)
        struct.pack_into('<I', emmc, 860, 3)
        prep.require_sd_1v8_consent(emmc)

    supervisor = load('sd_voltage_launch', 'tools/check-a733-smp-supervised.py')
    for outcome in ('success', 'transport-failure', 'clean-refusal', 'prelaunch-refusal'):
        record = {'launch_attempted': False}; saved = []; executed = []; quarantines = []; holds = []
        def save(): saved.append(json.loads(json.dumps(record)))
        def transaction(*parts):
            if parts[0] == 'readl': return '0'
            assert parts[0] == 'exe'
            assert saved[-1]['power_removal_required'] and saved[-1]['launch_attempted']
            executed.append(parts)
            if outcome == 'transport-failure': raise OSError('modeled transport failure')
        def ready():
            if outcome == 'prelaunch-refusal': raise ValueError('modeled prelaunch refusal')
        def observe(start):
            if outcome == 'clean-refusal':
                record.update(clean_refusal=True, refusal_probe_attempted=True, sram_restored=True, rom_usable=True)
            else: record['storage_read_result'] = 'observed-success'
        with patch.object(supervisor.base, 'preload', lambda *a: None):
            try:
                supervisor.launch_once(transaction, observe, None,
                    {'epoch': 1, 'generation': 2, '_backup_hashes': {}}, None, record, save, ready,
                    result_key='storage_read_result', power_removal_reason='SD profile 3 may switch to 1.8 V')
            except OSError: assert outcome == 'transport-failure'
            except ValueError: assert outcome == 'prelaunch-refusal'
        if outcome == 'prelaunch-refusal':
            assert not executed and not record.get('power_removal_required')
        elif outcome == 'clean-refusal':
            supervisor.finish_after_observation(record, save, lambda: quarantines.append(True), lambda: holds.append(True))
            assert record['power_removal_required'] is False and record['ownership_release_permitted']
            assert not quarantines and not holds
        else:
            supervisor.retain_after_capture(record, save, lambda: quarantines.append(True), lambda: holds.append(True))
            assert record['power_removal_required'] and quarantines == holds == [True]

    runner = load('sd_voltage_kernel', 'tools/check-a733-kernel.py')
    helper = load('sd_voltage_failure', 'tools/check-a733-smp.py')
    fel = args.output/'fel-fixture'; fel.write_bytes(b'never executed')
    binary = args.output/'image.bin'; binary.write_bytes(b'modeled binary')
    node = Path('/fake/003/051'); initial = [1, 2, 3]; returned = [1, 2, 4]
    for mode in ('denied', 'success', 'result-failure', 'transport-failure', 'prelaunch-refusal', 'normal'):
        run = args.output / ('kernel-' + mode); locked = []; requests = []; quarantines = []; holds = []
        memory = {}; active = [initial]
        @contextmanager
        def lock():
            locked.append(True)
            try: yield node
            finally: locked.pop()
        @contextmanager
        def enumeration(*a): yield
        def retain():
            assert locked
            holds.append(True); raise Held()
        def quarantine(*a): assert locked; quarantines.append(a)
        def require_rom(n):
            if mode == 'prelaunch-refusal': raise ValueError('modeled prelaunch refusal')
        access = SimpleNamespace(board_lock=lock, require_session=lambda n: None, require_rom=require_rom,
            identity=lambda n: active[0], target=lambda n: Path('/fake/fixture-port'),
            session_node=lambda config: node, quarantine_instance=quarantine)
        def transact(command, **kwargs):
            assert locked
            parts = command[3:]; requests.append(parts.copy())
            output = ''
            if parts[0] == 'version': output = 'soc=00001903(A733)'
            elif parts[0] == 'readl': output = 'eb00008f e3a00911 e3a0193d e3a0605c ea00004d'
            elif parts[0] == 'reset64':
                saved = json.loads((run/'session.json').read_text())
                assert saved['launch_attempted']
                assert saved.get('power_removal_required', False) == (mode != 'normal')
                if mode == 'transport-failure':
                    return SimpleNamespace(returncode=1, stdout='', stderr='modeled loss')
                active[0] = returned
                data = bytearray(16384); log = b'PASS kernel: modeled completion\n'
                status = 1 if mode == 'result-failure' else 0
                struct.pack_into('<16Q', data, 0, 0x534b4552, 5, status, len(log), *([0]*12))
                data[256:256+len(log)] = log; memory[0x42000000] = data
            else:
                while parts:
                    operation, address = parts[:2]; parts = parts[2:]; address = int(address, 0)
                    if operation == 'write': memory[address] = Path(parts[0]).read_bytes(); parts = parts[1:]
                    else:
                        size = int(parts[0]); Path(parts[1]).write_bytes(memory.get(address, b'\xa5'*size)[:size])
                        parts = parts[2:]
            return SimpleNamespace(returncode=0, stdout=output, stderr='')
        argv = ['--fel', str(fel), '--image', str(binary), '--evidence', str(run),
                '--ram-input', str(input2 if mode == 'normal' else denied)]
        if mode not in ('denied', 'normal'): argv += ['--allow-sd-1v8']
        guard = SimpleNamespace(check_health=lambda: None, reenumeration=enumeration)
        with patch.object(runner, 'load_module', lambda name, path: access if name == 'access' else helper), \
             patch.dict(sys.modules, usb_guard=guard), patch.object(runner.subprocess, 'run', transact), \
             patch.object(runner.os, 'access', lambda *a: True), patch.object(helper, 'hold_ownership', retain), \
             patch.object(helper.stop_after_failure, '__defaults__', (retain,)), redirect_stdout(io.StringIO()):
            if mode == 'denied':
                consent_refused(lambda: runner.main(argv))
                assert not run.exists() and not requests and not locked
                continue
            try: runner.main(argv)
            except Held: assert mode in ('success', 'result-failure', 'transport-failure')
            except SystemExit: assert mode == 'prelaunch-refusal'
            else: assert mode == 'normal'
        saved = json.loads((run/'session.json').read_text())
        if mode in ('success', 'result-failure', 'transport-failure'):
            assert holds == [True] and len(quarantines) == 1 and saved['power_removal_required']
            assert sum(r[0] == 'reset64' for r in requests) == 1
            if mode == 'success': assert saved['complete'] is True and runner.returned_to_rom(saved)
            if mode == 'transport-failure': assert requests[-1][0] == 'reset64' and saved['transport_failed']
        else:
            assert not holds and not quarantines and not saved.get('power_removal_required')
    (args.output/'result.json').write_text(json.dumps({'passed': True, 'physical_access': False,
        'scope': 'Production host admission and lifecycle with modeled ABI/image and fake transport.'}, indent=2)+'\n')
    print('PASS SD 1.8 V consent, durable launch marking and retained ownership')


if __name__ == '__main__': main()
