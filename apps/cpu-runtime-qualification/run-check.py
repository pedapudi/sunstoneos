#!/usr/bin/env python3
"""Exercise the actual CPU callback and shared transport with modeled USB only."""
import argparse
import ast
import contextlib
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import types
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('cpu_run_host', HERE / 'host.py')
host = importlib.util.module_from_spec(spec)
spec.loader.exec_module(host)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ['image', 'manifest', 'native-report', 'launcher', 'output']:
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--epoch', type=lambda x: int(x, 0), required=True)
    parser.add_argument('--case', action='append', help='Run only named affected callback cases')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    geometry = host.manifest_check(args.image, json.loads(args.manifest.read_text()))
    native = args.native_report.read_bytes()
    host.decode_report(native, args.epoch)
    results = []
    with tempfile.TemporaryDirectory(prefix='cpu-return-fixture-') as temporary:
        root = Path(temporary)
        bundle = root / 'bundle'
        bundle.mkdir()
        segments, address, build_id = host.supervised.image_parts(args.image)
        payloads = []
        for i, segment in enumerate(segments):
            if segment['data']:
                name = f'segment-{i}.bin'
                (bundle / name).write_bytes(segment['data'])
                payloads.append({'name': name, 'address': segment['address']})
        # Exercise the actual request producer with model-only identifiers.
        # No admission record or physically valid bundle is fabricated.
        request, contract = host.supervised.request_bytes(address, build_id, args.epoch, 2, 3, 4, bytes([5]) * 32)
        for name, raw in [('request.bin', request), ('contract.bin', contract), ('result-zero.bin', bytes(16384)), ('report-zero.bin', bytes(128))]:
            (bundle / name).write_bytes(raw)
        shutil.copy2(args.launcher, bundle / 'launch.bin')
        prepared = {'image_sha256': host.digest(args.image), 'qualification': geometry,
                    'epoch': args.epoch, 'segments': payloads,
                    'files': {p.name: host.digest(p) for p in bundle.iterdir()},
                    '_sizes': {p.name: p.stat().st_size for p in bundle.iterdir()}}
        fel = root / 'modeled-fel'
        fel.write_bytes(b'never executed')
        variants = ['normal', 'admission-refusal', 'rom-signature', 'short-backup', 'missing-backup', 'backup-failure',
                    'preload-failure', 'late-admission-refusal', 'watchdog', 'exe-failure',
                    'same-ROM', 'wrong-return-port', 'short-report', 'bad-report',
                    'changed-backup', 'restore-failure', 'restore-mismatch']
        if args.case:
            assert set(args.case) <= set(variants)
            variants = args.case
        for variant in variants:
            state = {'locked': False, 'held': 0, 'quarantined': 0, 'commands': [], 'admissions': 0,
                     'launched': False, 'returned': False, 'restores': [], 'backups': []}
            old, new = Path('/dev/bus/usb/003/056'), Path('/dev/bus/usb/003/057')
            port = Path('/sys/bus/usb/devices/3-1')
            evidence = root / variant
            p = copy.deepcopy(prepared)

            class Access:
                @contextlib.contextmanager
                def board_lock(self):
                    state['locked'] = True
                    try:
                        yield old
                    finally:
                        state['locked'] = False

                def require_session(self, node):
                    assert state['locked']

                require_rom = require_session

                def identity(self, node):
                    return [1, 57 if node == new else 56, 3, 4, 5]

                def target(self, node):
                    return port.with_name('3-3') if node == new and variant == 'wrong-return-port' else port

                def session_node(self, config):
                    assert state['launched']
                    state['returned'] = variant != 'same-ROM'
                    return old if variant == 'same-ROM' else new

                def quarantine_instance(self, *unused):
                    assert state['locked']
                    state['quarantined'] += 1

            class Guard:
                def check_health(self):
                    pass

                @contextlib.contextmanager
                def reenumeration(self, *unused):
                    yield

            def admit(*unused):
                state['admissions'] += 1
                if variant == 'admission-refusal' or variant == 'late-admission-refusal' and state['admissions'] == 3:
                    raise ValueError('modeled admission refusal')

            def hold():
                assert state['locked']
                state['held'] += 1

            def process(command, **kwargs):
                assert state['locked'] and command[2] == ('3:57' if state['returned'] else '3:56')
                v = command[3:]
                state['commands'].append(v)
                out = ''
                if v[0] == 'version':
                    out = 'soc=00001903(A733)'
                elif v[0] == 'readl':
                    if len(v) > 2:
                        assert v == [item for a in [0x20, 0x24, 0x28, 0x3c, 0x40] for item in ['readl', hex(a)]]
                        out = '0 0 0 0 0' if variant == 'rom-signature' else 'eb00008f e3a00911 e3a0193d e3a0605c ea00004d'
                    else:
                        assert v[1] == '0x02050014'
                        out = '1' if variant == 'watchdog' else '0'
                elif v[0] == 'read':
                    address, size, destination = int(v[1], 16), int(v[2]), Path(v[3])
                    if state['launched']:
                        assert state['returned'] and address == geometry['report_address'] and size == host.REPORT_BYTES
                        raw = native[:-1] if variant == 'short-report' else bytes(size) if variant == 'bad-report' else native
                        destination.write_bytes(raw)
                        if variant == 'changed-backup':
                            (evidence / 'before-dram-4.bin').write_bytes(b'changed')
                    else:
                        if variant == 'missing-backup':
                            return types.SimpleNamespace(returncode=0, stdout='', stderr='')
                        if address >= host.RAM_START:
                            if variant == 'backup-failure':
                                return types.SimpleNamespace(returncode=1, stdout='', stderr='modeled transport failure')
                            state['backups'].append((address, size))
                        destination.write_bytes(bytes([address >> 20 & 255]) * (size - 1 if variant == 'short-backup' else size))
                elif v[0] == 'write':
                    address, source, size, destination = int(v[1], 16), Path(v[2]), int(v[5]), Path(v[6])
                    raw = source.read_bytes()
                    assert size == len(raw) and kwargs['pass_fds']
                    if state['launched']:
                        assert state['returned'] and host.RAM_START <= address < address + size <= host.RAM_START + host.RAM_BYTES
                        if variant == 'restore-failure':
                            return types.SimpleNamespace(returncode=1, stdout='', stderr='modeled restore failure')
                        state['restores'].append((address, size))
                        if variant == 'restore-mismatch':
                            raw = bytes([raw[0] ^ 1]) + raw[1:]
                    else:
                        assert len(state['backups']) == 5
                        if variant == 'preload-failure':
                            return types.SimpleNamespace(returncode=1, stdout='', stderr='modeled preload failure')
                    destination.write_bytes(raw)
                elif v[0] == 'exe':
                    assert v[1] == '0x52000' and not state['launched']
                    state['launched'] = True
                    if variant == 'exe-failure':
                        return types.SimpleNamespace(returncode=1, stdout='', stderr='modeled exe failure')
                else:
                    raise AssertionError(v)
                return types.SimpleNamespace(returncode=0, stdout=out, stderr='')

            options = types.SimpleNamespace(bundle=bundle, fel=fel, evidence=evidence, root_owned_claim='model-only external claim')
            real_digest = host.base.digest
            def digest(path):
                return host.base.FEL_HASH if path == fel else real_digest(path)
            error = None
            with patch.object(host.base, 'digest', digest), patch.object(host.base, 'hold_ownership', hold), \
                 patch.object(host.returning.subprocess, 'run', process), patch.object(host.returning.os, 'access', return_value=True), \
                 patch.object(host.returning.time, 'monotonic', side_effect=[0, 51 if variant == 'same-ROM' else 0]):
                try:
                    host.returning.run(options, base=host.base, profile=host.PROFILE, validate_bundle=lambda _: p,
                        admit=admit, execute=lambda tx, wait, ready, p, record, journal: host.run_once(tx, wait, ready, bundle, p, evidence, record, journal),
                        description='Modeled CPU adapter', access=Access(), guard=Guard(), return_timeout=host.RETURN_TIMEOUT_SECONDS)
                except (ValueError, OSError) as failure:
                    error = str(failure)
            record = json.loads((evidence / 'session.json').read_text())
            assert (error is None) == (variant == 'normal'), (variant, error)
            if variant == 'normal':
                expected = [(host.RAM_START + off, min(host.CHUNK_BYTES, host.RAM_BYTES - off)) for off in range(0, host.RAM_BYTES, host.CHUNK_BYTES)]
                assert state['backups'] == state['restores'] == expected
                assert record['complete'] and record['dram_restored'] and record['rom_usable'] and not record['sram_restored']
                assert 'mode' not in record and record['physical_access'] and record['ownership_release_permitted']
                assert record['returned_identity'] != record['initial_identity'] and not state['held']
            else:
                assert not record['complete'] and not record.get('ownership_release_permitted')
                clean = variant in ('admission-refusal', 'rom-signature')
                assert state['held'] == state['quarantined'] == (0 if clean else 1), (variant, state['held'])
                if variant != 'restore-mismatch':
                    assert not state['restores']
            results.append({'case': variant, 'passed': True, 'retained': bool(state['held']), 'requests': len(state['commands'])})
            shutil.rmtree(evidence)
    # Reuse unchanged geometry/producer evidence. host-check.py separately
    # validates the v2 manifest and report parser, including timer counts.
    prior = subprocess.check_output(['git', 'show', 'f96b17621429a9395d92559277699e9cad5aa058:apps/cpu-runtime-qualification/host.py'],
                                    cwd=host.ROOT, text=True)
    selected = {'geometry', 'prepare', 'validate_bundle'}
    def functions(text):
        return {node.name: ast.dump(node, include_attributes=False) for node in ast.parse(text).body if isinstance(node, ast.FunctionDef) and node.name in selected}
    assert functions(prior) == functions((HERE / 'host.py').read_text())
    for sp in [0x52010, 0x53020, 0x40038, 0x100004, 0x80001]:
        try:
            host.caller_interval({'complete': True, 'sram_restored': True, 'rom_usable': True, 'result': {'sp': sp}})
        except ValueError:
            pass
        else:
            raise AssertionError('Invalid caller stack admitted')
    assert host.caller_interval({'complete': True, 'sram_restored': True, 'rom_usable': True, 'result': {'sp': 0x80000}}) == (0x7ffc4, 60)
    sources = [HERE / 'host.py', Path(__file__), Path(host.returning.__file__), Path(host.supervised.__file__),
               Path(host.base.__file__), Path(host.supervised.observer.__file__), Path(host.kernel.__file__),
               host.ROOT / 'tools/usb-access.py']
    host.save_json(args.output / 'result.json', {'passed': True, 'cases': results, 'caller_stack_cases': 6,
        'geometry_producer_reused_from': 'f96b17621429a9395d92559277699e9cad5aa058', 'image_sha256': host.digest(args.image),
        'native_report_sha256': host.digest(args.native_report), 'launcher_sha256': host.digest(args.launcher),
        'source_hashes': {str(p): host.digest(p) for p in sources}, 'physical_access': False,
        'scope': 'Actual callback, protected preload and shared transport; modeled admission/USB, real linked image, request producer, launcher and native-model report. No physical authority created.'})
    print(f'PASS {len(results)} callback/transport cases and 6 caller-stack cases; geometry/producer evidence reused')


if __name__ == '__main__':
    main()
