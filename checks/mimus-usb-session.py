#!/usr/bin/env python3
"""Exercise USB Mimus admission and failure ownership with modeled transports only."""
import argparse
from contextlib import contextmanager, redirect_stdout
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
from types import ModuleType, SimpleNamespace
from unittest.mock import patch

if not __debug__:
    raise SystemExit('checks require assertions; unset PYTHONOPTIMIZE and do not pass -O')

ROOT = Path(__file__).resolve().parents[1]
CASES = ('baseline', 'warm', 'wrong-port', 'missing-hop', 'changed-tool', 'legacy-refusal',
         'fel-loss', 'native-loss', 'launch-interrupt', 'returned-failure', 'frozen-source')

class Held(BaseException):
    pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('inputs', 'reference', 'image', 'fel', 'ddr', 'warm-return', 'survey', 'baseline', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--case', action='append', choices=CASES, help='Run only newly affected cases')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    source = ROOT / 'tools/usb-mimus.py'
    warm = json.loads(args.warm_return.read_bytes())
    initial = Path(warm['returned_node']); initial_identity = warm['returned_identity']
    survey = json.loads(args.survey.read_bytes()); port_name = survey['port']
    results = []
    input_paths = [source, Path(__file__), ROOT/'tools/check-a733-kernel.py', ROOT/'tools/check-a733-smp.py',
                   args.inputs/'inputs.json', args.reference, args.image, args.fel, args.ddr,
                   args.warm_return, args.survey, args.baseline]
    snapshots = args.output/'sources'; snapshots.mkdir()
    identities = []
    for i, path in enumerate(input_paths):
        raw = path.read_bytes(); (snapshots/str(i)).write_bytes(raw)
        identities.append(dict(path=str(path), file=f'sources/{i}', sha256=hashlib.sha256(raw).hexdigest()))
    (args.output/'inputs.json').write_text(json.dumps(identities, indent=2)+'\n')
    for mode in CASES:
        if args.case and mode not in args.case: continue
        root = args.output/mode; root.mkdir(); evidence = root/'run'
        nodes = [initial, initial.with_name(f'{int(initial.name)+1:03}'), initial.with_name(f'{int(initial.name)+2:03}')]
        identities_live = [initial_identity, initial_identity[:-1]+[initial_identity[-1]+1],
                           initial_identity[:-1]+[initial_identity[-1]+2]]
        state = dict(index=0, locked=False, held=False, quarantine=[], requests=[], commands=[], generation=0)
        port = root/port_name; port.mkdir(); (port/'product').write_text('FEL')
        @contextmanager
        def board_lock():
            state['locked'] = True
            if mode == 'frozen-source':
                for path in frozen_sources: path.write_bytes(b'MODELED changed source after preparation')
            try: yield nodes[0]
            finally: state['locked'] = False
        @contextmanager
        def reenumeration(key, *unused):
            assert state['locked']
            assert key == ('sunstoneos-a733' if mode == 'baseline' else 'sunstoneos-a733-'+port_name)
            yield
        def require_rom(node):
            assert node == nodes[state['index']] and state['index'] != 1
        def next_node(config):
            state['index'] += 1
            (port/'product').write_text('sunstoneos RAM diagnostics' if state['index'] == 1 else 'FEL')
            return nodes[state['index']]
        def quarantine(node, reason, identity):
            assert state['locked'] and identity == identities_live[state['index']]
            state['quarantine'].append(str(node))
        access = SimpleNamespace(board_lock=board_lock, require_session=lambda n:None, require_rom=require_rom,
            identity=lambda n:identities_live[nodes.index(n)], target=lambda n:port,
            session_node=next_node, quarantine_instance=quarantine)
        def hold():
            assert state['locked']; state['held']=True; raise Held()
        def transact(command, **unused):
            if str(command[1]).endswith('mimus-parity.py'):
                assert Path(command[command.index('--reference')+1]).read_bytes() == args.reference.read_bytes()
                return SimpleNamespace(returncode=0, stdout='', stderr='')
            assert state['locked']
            assert Path(command[0]).read_bytes() == args.fel.read_bytes()
            request = list(map(str, command[3:])); state['requests'].append(request)
            if mode == 'fel-loss':
                return SimpleNamespace(returncode=1, stdout='', stderr='MODELED transport failure')
            if request[0] == 'reset64' and mode == 'launch-interrupt':
                raise KeyboardInterrupt('MODELED interrupt at launch')
            if request[0] == 'write' and request[1] == '0x41000000':
                assert Path(request[2]).read_bytes() == args.image.read_bytes()
                if mode != 'baseline': assert Path(request[2]) == evidence/'image.bin'
                Path(request[-1]).write_bytes(args.image.read_bytes())
            if request[0] == 'read':
                data = bytearray(int(request[2]))
                if len(data) == 128:
                    struct.pack_into('<I', data, 0, 0x534b4552 if mode == 'baseline' else 0)
                else:
                    struct.pack_into('<3Q', data, 0, 0x534b4552, 5, 7 if mode == 'returned-failure' else 0)
                Path(request[3]).write_bytes(data)
            return SimpleNamespace(returncode=0, stdout='soc=00001903(A733)' if request[0]=='version' else '', stderr='')
        class Device:
            bcdDevice = 0x0100
            def set_configuration(self, value): pass
            def write(self, endpoint, packet, timeout):
                if mode == 'native-loss': raise OSError('MODELED native transfer failure')
                magic, seq, cmd, size = struct.unpack_from('<4I', packet)
                state['commands'].append(cmd); payload = packet[16:]; status = 0
                if cmd == 1: body = struct.pack('<5Q', 0, 4, 0, 0, 0)
                elif cmd == 4: body = b''
                else:
                    if cmd == 0x14:
                        if not state.get('models'): status = 5
                        elif state.get('busy'): status = 2
                        else: state['generation'] += 1; state['busy'] = True
                    if cmd == 0x11:
                        if state.get('busy'): status = 2
                        state['slot'] = struct.unpack_from('<I', payload)[0]
                    if cmd == 0x12 and struct.unpack_from('<I', payload, 4)[0] == 1: status = 3
                    if cmd == 0x13:
                        if not state.get('models') and state['slot'] == 2: status = 4
                        if state['slot'] == 1: state['models'] = True
                    body = struct.pack('<I', status)
                    if cmd == 0x10:
                        body += struct.pack('<4I', 2, state['generation'], 1, 7); state['busy']=False
                    if cmd == 0x15: body += b'\n'
                self.response = struct.pack('<4I', magic, seq, cmd|0x80000000, len(body))+body
                return len(packet)
            def read(self, *a, **kw): return self.response
        usb = ModuleType('usb'); core = ModuleType('usb.core'); util = ModuleType('usb.util')
        core.find = lambda **kw:Device(); core.USBError = type('USBError', (Exception,), {})
        util.claim_interface = util.release_interface = util.dispose_resources = lambda *a:None
        usb.core, usb.util = core, util
        guard = SimpleNamespace(check_health=lambda:None, reenumeration=reenumeration)
        original_spec = importlib.util.spec_from_file_location
        def spec(name, path, *a, **kw):
            value = original_spec(name, path, *a, **kw)
            if Path(path).name == 'usb-access.py':
                value.loader = SimpleNamespace(create_module=lambda spec:None,
                    exec_module=lambda module:module.__dict__.update(vars(access)))
            elif Path(path).name == 'check-a733-smp.py':
                loader = value.loader
                def execute(module): loader.exec_module(module); module.hold_ownership=hold
                value.loader = SimpleNamespace(create_module=lambda spec:None, exec_module=execute)
            return value
        argv = [str(source), '--inputs', str(args.inputs), '--reference', str(args.reference),
                '--reference', str(args.reference), '--wav', json.loads((args.inputs/'inputs.json').read_text())['audio'],
                '--wav', json.loads((args.inputs/'inputs.json').read_text())['audio'],
                '--image', str(args.image), '--fel', str(args.fel), '--evidence', str(evidence)]
        if mode not in ('baseline', 'legacy-refusal'):
            current_survey = args.survey
            if mode == 'wrong-port':
                changed = dict(survey, port='99-9'); current_survey=root/'survey.json'
                current_survey.write_text(json.dumps(changed))
            argv += ['--ddr-evidence', str(args.ddr), '--platform-survey', str(current_survey)]
            if mode != 'missing-hop': argv += ['--warm-return', str(args.warm_return)]
            if mode == 'changed-tool':
                tool = root/'fel'; tool.write_bytes(b'MODELED wrong executable')
                argv[argv.index('--fel')+1] = str(tool)
        frozen_sources = []
        if mode == 'frozen-source':
            for option in ('--fel', '--image', '--reference', '--ddr-evidence', '--platform-survey', '--warm-return'):
                for index in [i+1 for i, value in enumerate(argv) if value == option]:
                    original = Path(argv[index]); copied = root/f'input-{index}'
                    copied.write_bytes(original.read_bytes()); argv[index] = str(copied); frozen_sources.append(copied)
        raw = args.baseline.read_bytes() if mode == 'baseline' else source.read_bytes()
        with patch.dict(sys.modules, {'usb':usb, 'usb.core':core, 'usb.util':util, 'usb_guard':guard}), \
             patch.object(sys, 'argv', argv), patch.object(importlib.util, 'spec_from_file_location', spec), \
             patch.object(subprocess, 'run', transact), patch.object(os, 'access', lambda *a:True), \
             redirect_stdout(io.StringIO()):
            try: exec(compile(raw, str(source), 'exec'), {'__file__':str(source), '__name__':'__main__'})
            except (SystemExit, Held): pass
        record = json.loads((evidence/'session.json').read_text())
        if mode in ('baseline', 'warm', 'frozen-source'):
            assert record['complete'] and len(record['cases']) == 2
            assert sum(r[0]=='reset64' for r in state['requests']) == 1
        elif mode in ('wrong-port', 'missing-hop', 'changed-tool'):
            assert not state['requests'] and not state['held'] and not state['quarantine']
        elif mode == 'legacy-refusal':
            assert [r[0] for r in state['requests']] == ['version', 'read']
        elif mode == 'returned-failure':
            assert not state['held'] and state['quarantine'] == [str(nodes[2])]
        else:
            assert state['held'] and state['quarantine'] == [str(nodes[state['index']])]
            assert not record['complete']
        if mode == 'warm':
            assert (evidence/'warm-return-1.json').read_bytes() == args.warm_return.read_bytes()
            assert (evidence/'ddr-admission.json').read_bytes() == args.ddr.read_bytes()
            assert record['mode'] == 'mimus-usb'
        results.append(dict(case=mode, native_exchanges=len(state['commands']), fel_requests=len(state['requests']),
                            held=state['held'], quarantined=bool(state['quarantine'])))
        if mode == 'warm' and results[0]['case'] == 'baseline':
            assert results[0]['native_exchanges'] == results[-1]['native_exchanges']
            assert results[0]['fel_requests'] == results[-1]['fel_requests']
        (args.output/'results.json').write_text(json.dumps(dict(physical_access=False, passed=results), indent=2)+'\n')
    print(f'PASS {len(results)} modeled launcher cases; no device access; {args.output}')

if __name__ == '__main__': main()
