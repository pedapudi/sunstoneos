#!/usr/bin/env python3
"""Check copied platform admission and retained failure without device access."""
import argparse
from contextlib import contextmanager, redirect_stdout
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]

def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    obj = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(obj)
    return obj

class Held(BaseException):
    pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--records', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--case', choices=('prelaunch-interruption',))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    paths = [Path(__file__), ROOT/'tools/check-a733-kernel.py', ROOT/'tools/check-a733-smp.py',
             args.records/'ddr/session.json', args.records/'survey/session.json', Path(sys.executable)]
    snapshots = args.output/'inputs'; snapshots.mkdir()
    inputs = []
    for i, path in enumerate(paths):
        raw = path.read_bytes(); target = snapshots/str(i); target.write_bytes(raw)
        inputs.append(dict(path=str(path), snapshot=str(target), sha256=hashlib.sha256(raw).hexdigest()))
    (args.output/'inputs.json').write_text(json.dumps(inputs, indent=2)+'\n')
    runner = load('kernel_admission', ROOT/'tools/check-a733-kernel.py')
    helper = load('kernel_failure_actual', ROOT/'tools/check-a733-smp.py')
    raw = tuple(p.read_bytes() for p in paths[3:5])
    ddr, survey = map(json.loads, raw)
    node = Path(ddr['device']); identity = ddr['node_identity']; port = survey['port']
    tool = Path(ddr['transactions'][0]['command'][0])
    results = []
    def passed(name):
        results.append(name)
        (args.output/'results.json').write_text(json.dumps(dict(passed=results, physical_access=False), indent=2)+'\n')
    if not args.case:
        runner.validate_platform_admission(raw, node, identity, port, ddr['tool_sha256'], helper.FEL_HASH)
        passed('actual-saved-board-ddr-and-survey')
        for name, which, key, value in [
            ('changed-ddr-identity',0,'node_identity',[0]),
            ('incomplete-survey',1,'complete',False),
            ('wrong-port',1,'port','3-1'),
            ('unrestored-survey',1,'sram_restored',False),
            ('wrong-tool',0,'tool_sha256','0'*64),
            ('missing-gates',1,'result',{'launcher_gates':{}})]:
            records = list(map(json.loads, raw)); records[which][key] = value
            try:
                runner.validate_platform_admission(tuple(json.dumps(r) for r in records), node, identity, port,
                                                   ddr['tool_sha256'], helper.FEL_HASH)
            except RuntimeError:
                passed(name)
            else:
                raise AssertionError(name)
    
    # Exercise actual main/transaction/exception ordering and shared retention
    # helper. Only the device transport and lock are modeled; no USB discovery.
    for mode in ('success-new-admission', 'legacy-signature-refusal', 'prelaunch-transport-loss', 'launch-transport-loss', 'prelaunch-interruption'):
        if args.case and mode != args.case:
            continue
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td); image = tmp/'image.bin'; image.write_bytes(b'bounded-image')
            held = [False]; requests = []; quarantine = []; memory = {}; active = [identity]
            @contextmanager
            def lock():
                held[0] = True
                try: yield node
                finally: held[0] = False
            @contextmanager
            def reenumeration(*args):
                yield
            def retain():
                assert held[0], 'Lock released before retention'
                raise Held()
            access = SimpleNamespace(board_lock=lock, require_session=lambda n: None,
                require_rom=lambda n: None, identity=lambda n: active[0], target=lambda n: Path('/mock')/port,
                session_node=lambda config: node,
                quarantine_instance=lambda *a: quarantine.append(a))
            def transact(command, **kwargs):
                assert held[0]
                parts = command[3:]; requests.append(parts)
                if mode == 'prelaunch-interruption': raise KeyboardInterrupt()
                if mode == 'prelaunch-transport-loss' or (mode == 'launch-transport-loss' and parts[0] == 'reset64'):
                    return SimpleNamespace(returncode=1, stdout='', stderr='modeled transport failure')
                output = ''
                if parts[0] == 'version': output = 'AWUSBFEX soc=00001903(A733)'
                elif parts[0] == 'readl': output = 'eb00008f e3a00911 e3a0193d e3a0605c ea00004d'
                elif parts[0] == 'reset64':
                    active[0] = identity[:-1]+[1]
                    data = bytearray(16384); log=b'PASS kernel: modeled completion\n'
                    struct.pack_into('<16Q', data, 0, 0x534b4552,5,0,len(log), *([0]*12))
                    data[256:256+len(log)] = log; memory[0x42000000] = bytes(data)
                else:
                    while parts:
                        op, address = parts[:2]; parts=parts[2:]; address=int(address,0)
                        if op == 'write': memory[address]=Path(parts[0]).read_bytes(); parts=parts[1:]
                        elif op == 'read':
                            size=int(parts[0]); Path(parts[1]).write_bytes(memory.get(address,b'\xa5'*size)[:size]); parts=parts[2:]
                        else: raise AssertionError(op)
                return SimpleNamespace(returncode=0,stdout=output,stderr='')
            def modules(name, path):
                return access if name == 'access' else helper
            argv=['--fel',str(tool),'--image',str(image),'--evidence',str(tmp/'run')]
            if mode != 'legacy-signature-refusal':
                argv += ['--ddr-evidence',str(paths[3]),'--platform-survey',str(paths[4])]
            guard=SimpleNamespace(check_health=lambda:None, reenumeration=reenumeration)
            with patch.object(runner,'load_module',modules), patch.dict(sys.modules,usb_guard=guard), \
                 patch.object(runner.subprocess,'run',transact), patch.object(runner.os,'access',lambda *a:True), \
                 patch.object(helper,'hold_ownership',retain), \
                 patch.object(helper.stop_after_failure,'__defaults__',(retain,)), redirect_stdout(io.StringIO()):
                try:
                    runner.main(argv)
                except Held:
                    assert (mode.endswith('transport-loss') or mode == 'prelaunch-interruption') and len(quarantine)==1
                    assert requests[-1][0] == ('reset64' if mode.startswith('launch-') else 'version')
                except SystemExit:
                    assert mode == 'legacy-signature-refusal' and not quarantine
                    assert not any(r[0] in ('write','reset64') for r in requests)
                else:
                    assert mode == 'success-new-admission'
                    assert sum(r[0]=='reset64' for r in requests)==1
                    assert json.loads((tmp/'run/session.json').read_text())['complete']
            assert not held[0] # Test sentinel unwinds only after observing retained lock.
            passed(mode)
    if args.case:
        print(json.dumps({'passed':len(results),'evidence':str(args.output),'physical_access':False}))
        return
    # Even an evidence/quarantine failure cannot bypass the shared finally hold.
    for launched in (False, True):
        calls=[]
        def fail(): raise OSError('modeled journal failure')
        def hold(): calls.append('held'); raise Held()
        with patch.object(helper,'hold_ownership',hold), patch.object(helper.stop_after_failure,'__defaults__',(hold,)):
            try:
                runner.retain_failure(dict(launch_attempted=launched,transport_failed=True),fail,fail,RuntimeError('lost'),helper)
            except Held: pass
        assert calls==['held']
    passed('journal-failure-retains-before-and-after-launch')
    print(json.dumps({'passed':len(results),'evidence':str(args.output),'physical_access':False}))

if __name__ == '__main__': main()
