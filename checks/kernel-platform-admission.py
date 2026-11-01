#!/usr/bin/env python3
"""Check live RAM readiness, kernel results and retained failures without device access."""
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

# A level-1 translation fault on a load from 0x4_0000_0000 in a task named fault.
CONTROLLED_FAULT = dict(name='level-1-translation-fault', status=1, interrupt=None, fail_lines=1,
                        fault=dict(esr_mask=0xfe00007f, esr_value=0x96000005, far=0x400000000,
                                   elr_low=0x41001000, elr_high=0x41001240, task='fault'),
                        log_contains=['CHECK fault capture: load 0x400000000'])
FAULT_LOG = (b'CHECK fault capture: load 0x400000000\n'
             b'FAIL exception core=0 ESR=0x0000000096000005 ELR=0x0000000041001234 '
             b'FAR=0x0000000400000000 SPSR=0x00000000000003c5 task=fault\n')
# A forced return after a WFI stage that stopped waking: the progress record
# between the result header and the log shows fewer wakes than its bound and
# a last wake more than one tick before the monitor's end_counter.
CONTROLLED_PROGRESS = dict(name='wfi-stopped-waking', status=2, interrupt=29, fail_lines=0, fault=None,
                           log_contains=['STAGE 2 walk complete', 'STAGE 3 wfi begin'],
                           progress=dict(offset=128, magic=0x3130535342344242, stage=3, count_below=1000,
                                         age_above=24000))
PROGRESS_LOG = b'STAGE 1 counters complete\nSTAGE 2 walk complete\nSTAGE 3 wfi begin\n'
PROGRESS_END = 10**9


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--case', choices=('prelaunch-interruption',))
    args = parser.parse_args()
    runner = load('kernel_runner', ROOT/'tools/check-a733-kernel.py')
    helper = load('kernel_failure_actual', ROOT/'tools/check-a733-smp.py')
    args.output = helper.output_directory(args.output)
    node = Path('/dev/bus/usb/003/051'); identity = [1, 2, 3, 4, 5]; port = 'fixture-port'
    tool = args.output/'fel-fixture'; tool.write_bytes(b'host fixture; never executed')
    results = []
    def passed(name):
        results.append(name)
        (args.output/'results.json').write_text(json.dumps(dict(passed=results, physical_access=False), indent=2)+'\n')
    # Readiness is established by a reversible write/read in reserved result RAM.
    # A successful but stale read is restored and refused; a failed transfer
    # stops immediately without attempting restoration over the failed transport.
    for mode in ('good', 'stale', 'restore-corrupt', 'test-transport-loss', 'restore-transport-loss'):
        evidence = args.output/mode; evidence.mkdir()
        original = bytes(range(8)); memory = [original]; calls = []
        def memory_transaction(*arguments, pass_fds=()):
            calls.append(arguments)
            if arguments[0] == 'read':
                Path(arguments[3]).write_bytes(memory[0]); return ''
            if (mode == 'test-transport-loss' and len(calls) == 2 or
                    mode == 'restore-transport-loss' and len(calls) == 3):
                raise OSError('injected transport failure')
            data = Path(arguments[2]).read_bytes()
            memory[0] = data
            readback = original if mode == 'stale' and len(calls) == 2 else data
            if mode == 'restore-corrupt' and len(calls) == 3: readback = bytes(8)
            Path(arguments[6]).write_bytes(readback)
        try: helper.verify_dram(memory_transaction, evidence)
        except (ValueError, OSError): assert mode != 'good'
        else: assert mode == 'good'
        assert len(calls) == (2 if mode == 'test-transport-loss' else 3)
        if mode in ('good', 'stale'): assert memory[0] == original
        passed('direct-dram-' + mode)
    # Exercise actual main/transaction/exception ordering and shared retention
    # helper. Only the device transport and lock are modeled; no USB discovery.
    modes = ('success-without-process-records', 'prelaunch-transport-loss', 'launch-transport-loss', 'prelaunch-interruption',
             'dram-restore-corrupt', 'dram-test-transport-loss', 'dram-restore-transport-loss', 'dram-test-interruption')
    modes += ('controlled-outcome-match', 'controlled-outcome-unlisted', 'controlled-progress-match',
              'controlled-progress-unlisted', 'returned-status-failure', 'returned-quarantine-failure')
    for mode in modes:
        if args.case and mode != args.case:
            continue
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td); image = tmp/'image.bin'; image.write_bytes(b'bounded-image')
            run_node, run_identity = node, identity
            held = [False]; requests = []; quarantine = []; memory = {}; active = [run_identity]; probe_writes = [0]
            @contextmanager
            def lock():
                held[0] = True
                try: yield run_node
                finally: held[0] = False
            @contextmanager
            def reenumeration(*args):
                yield
            def retain():
                assert held[0], 'Lock released before retention'
                raise Held()
            def quarantine_instance(*a):
                assert held[0], 'Lock released before quarantine'
                if mode == 'returned-quarantine-failure': raise OSError('modeled quarantine failure')
                quarantine.append(a)
            access = SimpleNamespace(board_lock=lock, require_session=lambda n: None,
                require_rom=lambda n: None, identity=lambda n: active[0], target=lambda n: Path('/mock')/port,
                session_node=lambda config: run_node, quarantine_instance=quarantine_instance)
            def transact(command, **kwargs):
                assert held[0]
                parts = command[3:]; requests.append(parts)
                if mode == 'prelaunch-interruption': raise KeyboardInterrupt()
                if parts[0] == 'write' and int(parts[1], 0) == 0x42000000 and len(Path(parts[2]).read_bytes()) == 8:
                    probe_writes[0] += 1
                    if (mode == 'dram-test-transport-loss' and probe_writes[0] == 1 or
                            mode == 'dram-restore-transport-loss' and probe_writes[0] == 2):
                        return SimpleNamespace(returncode=1, stdout='', stderr='modeled DRAM probe transport failure')
                    if mode == 'dram-test-interruption': raise KeyboardInterrupt()

                if mode == 'prelaunch-transport-loss' or (mode == 'launch-transport-loss' and parts[0] == 'reset64'):
                    return SimpleNamespace(returncode=1, stdout='', stderr='modeled transport failure')
                output = ''
                if parts[0] == 'version': output = 'AWUSBFEX soc=00001903(A733)'
                elif parts[0] == 'readl': output = 'eb00008f e3a00911 e3a0193d e3a0605c ea00004d'
                elif parts[0] == 'reset64':
                    active[0] = run_identity[:-1]+[1]
                    data = bytearray(16384); log=b'PASS kernel: modeled completion\n'; status = 0
                    interrupt = end = 0
                    if mode.startswith('controlled-progress'):
                        log, status, interrupt, end = PROGRESS_LOG, 2, 29, PROGRESS_END
                        # The unlisted case woke one tick before the forced return.
                        age = 24000 if mode == 'controlled-progress-unlisted' else 100000
                        struct.pack_into('<4Q', data, 128, 0x3130535342344242, 3, 412, PROGRESS_END - age)
                    elif mode.startswith('returned-'):
                        # The 2026-10-24 port 3-1 return: a bounded refusal, status 1.
                        log, status = b'GPU power phase=5 error=5 step=8 last=0x101\nFAIL GPU ON poll\n', 1
                    elif mode.startswith('controlled-'):
                        log, status = FAULT_LOG, 1
                        if mode == 'controlled-outcome-unlisted': log = log.replace(b'FAR=0x0000000400000000',
                                                                                     b'FAR=0x0000000400000008')
                    struct.pack_into('<16Q', data, 0, 0x534b4552,5,status,len(log), 0,0,0,interrupt, 0,end, *([0]*6))
                    data[256:256+len(log)] = log; memory[0x42000000] = bytes(data)
                else:
                    while parts:
                        op, address = parts[:2]; parts=parts[2:]; address=int(address,0)
                        if op == 'write': memory[address]=Path(parts[0]).read_bytes(); parts=parts[1:]
                        elif op == 'read':
                            size=int(parts[0]); data=memory.get(address,b'\xa5'*size)[:size]
                            if mode == 'dram-restore-corrupt' and address == 0x42000000 and probe_writes[0] == 2: data=bytes(size)
                            Path(parts[1]).write_bytes(data); parts=parts[2:]
                        else: raise AssertionError(op)
                return SimpleNamespace(returncode=0,stdout=output,stderr='')
            def modules(name, path):
                return access if name == 'access' else helper
            argv=['--fel',str(tool),'--image',str(image),'--evidence',str(tmp/'run')]
            if mode.startswith('controlled-'):
                listed = CONTROLLED_PROGRESS if mode.startswith('controlled-progress') else CONTROLLED_FAULT
                (tmp/'controlled.json').write_text(json.dumps(dict(schema='kernel-controlled-outcomes-v1',
                                                                   outcomes=[listed])))
                argv += ['--controlled', str(tmp/'controlled.json')]
            guard=SimpleNamespace(check_health=lambda:None, reenumeration=reenumeration)
            with patch.object(runner,'load_module',modules), patch.dict(sys.modules,usb_guard=guard), \
                 patch.object(runner.subprocess,'run',transact), patch.object(runner.os,'access',lambda *a:True), \
                 patch.object(helper,'hold_ownership',retain), \
                 patch.object(helper.stop_after_failure,'__defaults__',(retain,)), redirect_stdout(io.StringIO()):
                try:
                    runner.main(argv)
                except Held:
                    if mode.startswith('dram-'):
                        assert len(quarantine)==1 and not any(r[0]=='reset64' for r in requests)
                        assert requests[-1][0]=='write'
                        saved=json.loads((tmp/'run/session.json').read_text())
                        assert saved['dram_probe_dirty'] and not saved['launch_attempted']
                        assert probe_writes[0] == (1 if mode in ('dram-test-transport-loss','dram-test-interruption') else 2)
                    elif mode == 'returned-quarantine-failure':
                        # A returned instance that could not be quarantined keeps the port lock.
                        assert not quarantine and requests[-1][0] == 'read'
                    else:
                        assert (mode.endswith('transport-loss') or mode == 'prelaunch-interruption') and len(quarantine)==1
                        assert requests[-1][0] == ('reset64' if mode.startswith('launch-') else 'version')
                except SystemExit as stop:
                    # An unlisted nonzero status after a ROM return quarantines the
                    # returned instance and exits with the recorded result, without
                    # holding the port lock.
                    assert mode in ('controlled-outcome-unlisted', 'controlled-progress-unlisted',
                                    'returned-status-failure'), mode
                    assert len(quarantine)==1 and quarantine[0][2] == run_identity[:-1]+[1]
                    assert requests[-1][0] == 'read' and sum(r[0]=='reset64' for r in requests)==1
                    saved = json.loads((tmp/'run/session.json').read_text())
                    assert saved['complete'] is False and saved['transport_failed'] is False
                    assert 'Unexpected kernel result' in saved['failure'] and saved['result']['status'] in (1, 2)
                    assert str(stop.code).startswith('Stopped: Unexpected kernel result')
                    if mode == 'returned-status-failure':
                        assert (tmp/'run/kernel.log').read_text().endswith('FAIL GPU ON poll\n')
                else:
                    assert mode in ('success-without-process-records', 'controlled-outcome-match',
                                    'controlled-progress-match')
                    assert sum(r[0]=='reset64' for r in requests)==1 and not quarantine
                    saved = json.loads((tmp/'run/session.json').read_text())
                    assert saved['complete']
                    if mode == 'controlled-outcome-match':
                        assert saved['controlled']['outcome'] == CONTROLLED_FAULT
                        assert saved['controlled']['faults'] == [dict(core=0, esr=0x96000005, elr=0x41001234,
                                                                      far=0x400000000, spsr=0x3c5, task='fault')]
                        assert runner.controlled_return(saved)
                    if mode == 'controlled-progress-match':
                        assert saved['controlled']['outcome'] == CONTROLLED_PROGRESS and saved['controlled']['faults'] == []
                        assert saved['controlled']['progress'] == dict(magic=0x3130535342344242, stage=3, count=412,
                                                                       stamp=PROGRESS_END - 100000)
                        assert runner.controlled_return(saved)
            assert not held[0] # Test sentinel unwinds only after observing retained lock.
            passed(mode)
    if not args.case:
        el1 = load('el1_runner', ROOT/'tools/check-a733-el1.py')
        for mode in ('restore-corrupt', 'test-transport-loss', 'restore-transport-loss', 'test-interruption'):
            with tempfile.TemporaryDirectory() as td:
                tmp=Path(td);image=tmp/'image';image.write_bytes(b'bounded EL1 fixture')
                held=[];requests=[];quarantine=[];memory={};writes=[0]
                @contextmanager
                def lock():
                    held.append(True)
                    try:yield node
                    finally:held.pop()
                @contextmanager
                def enumeration(*args):yield
                def retain():assert held;raise Held()
                access=SimpleNamespace(board_lock=lock,require_session=lambda n:None,require_rom=lambda n:None,
                    identity=lambda n:identity,target=lambda n:Path('/mock')/port,
                    quarantine_instance=lambda *args:quarantine.append(args))
                def transact(command,**kwargs):
                    assert held
                    parts=command[3:];requests.append(parts)
                    if parts[0]=='version':return SimpleNamespace(returncode=0,stdout='soc=00001903(A733)',stderr='')
                    if parts[0]=='readl':return SimpleNamespace(returncode=0,stdout='eb00008f e3a00911 e3a0193d e3a0605c ea00004d',stderr='')
                    if parts[0]=='write':
                        writes[0]+=1
                        if mode=='test-interruption':raise KeyboardInterrupt()
                        if (mode=='test-transport-loss' and writes[0]==1 or mode=='restore-transport-loss' and writes[0]==2):
                            return SimpleNamespace(returncode=1,stdout='',stderr='injected failure')
                    while parts:
                        operation,address=parts[:2];parts=parts[2:]
                        if operation=='write':memory[address]=Path(parts[0]).read_bytes();parts=parts[1:]
                        else:
                            size=int(parts[0]);data=memory.get(address,b'\xa5'*size)
                            if mode=='restore-corrupt' and writes[0]==2:data=bytes(size)
                            Path(parts[1]).write_bytes(data);parts=parts[2:]
                    return SimpleNamespace(returncode=0,stdout='',stderr='')
                guard=SimpleNamespace(check_health=lambda:None,reenumeration=enumeration)
                with patch.object(el1,'load_module',lambda name,path:access if name=='access' else helper), \
                     patch.dict(sys.modules,usb_guard=guard),patch.object(el1.subprocess,'run',transact), \
                     patch.object(helper.stop_after_failure,'__defaults__',(retain,)):
                    try:el1.main(['--fel',str(tool),'--image',str(image),'--evidence',str(tmp/'run')])
                    except Held:pass
                    else:raise AssertionError('EL1 dirty RAM released lock')
                assert not held and len(quarantine)==1 and not any(r[0]=='reset64' for r in requests)
                saved=json.loads((tmp/'run/session.json').read_text())
                assert saved['dram_probe_dirty'] and not saved['launch_attempted']
                assert writes[0] == (1 if mode in ('test-transport-loss','test-interruption') else 2)
                passed('el1-dram-'+mode)
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
    # A returned record exits only after its quarantine succeeds; a launch
    # without a read result, or with a transport failure, still holds.
    returned = dict(launch_attempted=True, transport_failed=False, automatic_access=True,
                    returned_node='/dev/bus/usb/003/052', result=dict(status=1))
    for name, record, quarantine_ok, holds in [
            ('returned', returned, True, False),
            ('returned-quarantine-failure', returned, False, True),
            ('no-result', dict(returned, result=None), True, True),
            ('no-returned-node', {k: v for k, v in returned.items() if k != 'returned_node'}, True, True),
            ('transport-failed-after-return', dict(returned, transport_failed=True), True, True),
            ('no-access', dict(returned, automatic_access=False), True, True)]:
        calls=[]
        def hold(): calls.append('held'); raise Held()
        def quarantine():
            if not quarantine_ok: raise OSError('modeled quarantine failure')
            calls.append('quarantined')
        with patch.object(helper,'hold_ownership',hold), patch.object(helper.stop_after_failure,'__defaults__',(hold,)):
            try:
                runner.retain_failure(dict(record),lambda: None,quarantine,RuntimeError('status 1'),helper)
            except (Held, OSError): pass
        assert ('held' in calls) == holds and ('quarantined' in calls) == quarantine_ok, (name, calls)
    passed('returned-failure-exits-after-quarantine')
    print(json.dumps({'passed':len(results),'evidence':str(args.output),'physical_access':False}))

if __name__ == '__main__': main()
