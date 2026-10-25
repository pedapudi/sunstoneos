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
    parser.add_argument('--records', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--warm-return', type=Path, help='Saved successful kernel return for focused continuity checks')
    parser.add_argument('--warm-survey', type=Path, help='Saved survey after that return')
    parser.add_argument('--timeout-return', type=Path, help='Saved forced-stall return that follows --warm-return')
    parser.add_argument('--native-return', type=Path, help='Saved native returning CPU session, rebound after --timeout-return')
    parser.add_argument('--case', choices=('prelaunch-interruption',))
    args = parser.parse_args()
    if bool(args.warm_return) != bool(args.warm_survey):
        parser.error('--warm-return and --warm-survey must be supplied together')
    args.output.mkdir(parents=True, exist_ok=False)
    paths = [Path(__file__), ROOT/'tools/check-a733-kernel.py', ROOT/'tools/check-a733-smp.py',
             args.records/'ddr/session.json', args.records/'survey/session.json', Path(sys.executable)]
    if args.warm_return:
        paths += [args.warm_return, args.warm_survey]
    if args.native_return and not args.timeout_return:
        parser.error('--native-return requires --timeout-return')
    if args.timeout_return and not args.warm_return:
        parser.error('--timeout-return requires --warm-return')
    paths += [x for x in (args.timeout_return, args.native_return) if x]
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
    
    if args.warm_return and not args.case:
        warm_raw = args.warm_return.read_bytes()
        warm = json.loads(warm_raw)
        warm_survey_raw = args.warm_survey.read_bytes()
        warm_node = Path(warm['returned_node']); warm_identity = warm['returned_identity']
        warm_admission = (raw[0], warm_survey_raw)
        def validate_warm(records=warm_admission, chain=(warm_raw,)):
            runner.validate_platform_admission(records, warm_node, warm_identity, port,
                                               ddr['tool_sha256'], helper.FEL_HASH, chain)
        validate_warm()
        passed('actual-saved-warm-return-continuity')
        for name, key, value in [
            ('warm-broken-initial-node', 'initial_node', warm['returned_node']),
            ('warm-broken-initial-identity', 'initial_identity', warm['returned_identity']),
            ('warm-incomplete', 'complete', False),
            ('warm-transport-failure', 'transport_failed', True),
            ('warm-missing-transport-outcome', 'transport_failed', None),
            ('warm-wrong-port', 'port', '3-1'),
            ('warm-wrong-tool', 'tool_sha256', '0'*64),
            ('warm-no-launch', 'launch_attempted', False),
            ('warm-no-access', 'automatic_access', False),
            ('warm-same-identity', 'returned_identity', warm['initial_identity']),
            ('warm-same-node', 'returned_node', warm['initial_node']),
            ('warm-failed-terminal', 'result', dict(warm['result'], status=1)),
            ('warm-incomplete-terminal', 'result', dict(warm['result'], stage=4)),
            ('warm-wrong-magic', 'result', dict(warm['result'], magic=0)),
            ('warm-timeout-mode', 'mode', 'timeout')]:
            changed = dict(warm); changed[key] = value
            try: validate_warm(chain=(json.dumps(changed),))
            except RuntimeError: passed(name)
            else: raise AssertionError(name)
        for name, records, chain in [
            ('warm-missing-hop', warm_admission, ()),
            ('warm-old-survey', raw, (warm_raw,)),
            ('warm-duplicate-hop', warm_admission, (warm_raw, warm_raw)),
            ('warm-invalid-original-ddr', (json.dumps(dict(ddr, complete=False)), warm_survey_raw), (warm_raw,))]:
            try: validate_warm(records, chain)
            except RuntimeError: passed(name)
            else: raise AssertionError(name)
        unbound_ddr = dict(ddr); unbound_warm = dict(warm)
        for key in ('device', 'node_identity'): unbound_ddr.pop(key)
        for key in ('initial_node', 'initial_identity'): unbound_warm.pop(key)
        try: validate_warm((json.dumps(unbound_ddr), warm_survey_raw), (json.dumps(unbound_warm),))
        except RuntimeError: passed('warm-missing-ddr-and-initial-origin')
        else: raise AssertionError('warm-missing-ddr-and-initial-origin')
        # Model a second execution with explicit new host identity, using the
        # saved producer's successful fields. This is offline fixture evidence.
        second = dict(warm, initial_node=warm['returned_node'], initial_identity=warm_identity,
                      returned_node='/dev/bus/usb/003/094',
                      returned_identity=warm_identity[:1]+[warm_identity[1]+1]+warm_identity[2:])
        second_survey = dict(json.loads(warm_survey_raw), node=second['returned_node'],
                             identity=second['returned_identity'])
        runner.validate_platform_admission((raw[0], json.dumps(second_survey)),
            Path(second['returned_node']), second['returned_identity'], port,
            ddr['tool_sha256'], helper.FEL_HASH, (warm_raw, json.dumps(second)))
        passed('modeled-two-contiguous-kernel-returns')
        try:
            runner.validate_platform_admission((raw[0], json.dumps(second_survey)),
                Path(second['returned_node']), second['returned_identity'], port,
                ddr['tool_sha256'], helper.FEL_HASH, (json.dumps(second), warm_raw))
        except RuntimeError: passed('warm-reordered-hops')
        else: raise AssertionError('warm-reordered-hops')

    if args.timeout_return and not args.case:
        # The saved forced-stall return began on the saved kernel return's
        # instance. The survey on its returned instance is modeled from the
        # saved warm survey, as for the second kernel return above.
        timeout_raw = args.timeout_return.read_bytes(); timeout = json.loads(timeout_raw)
        assert timeout['mode'] == 'timeout' and timeout['initial_node'] == warm['returned_node']
        def after(record, survey_base):
            return dict(survey_base, node=record['returned_node'], identity=record['returned_identity'])
        def validate_chain(chain, last):
            runner.validate_platform_admission((raw[0], json.dumps(after(last, json.loads(warm_survey_raw)))),
                Path(last['returned_node']), last['returned_identity'], port,
                ddr['tool_sha256'], helper.FEL_HASH, chain)
        validate_chain((warm_raw, timeout_raw), timeout)
        passed('actual-saved-forced-stall-continuity')
        for name, key, value in [
            ('timeout-kernel-status', 'result', dict(timeout['result'], status=0)),
            ('timeout-wrong-interrupt', 'result', dict(timeout['result'], interrupt=30)),
            ('timeout-transport-failure', 'transport_failed', True),
            ('timeout-no-access', 'automatic_access', False),
            ('timeout-wrong-tool', 'tool_sha256', '0'*64)]:
            changed = dict(timeout); changed[key] = value
            try: validate_chain((warm_raw, json.dumps(changed)), changed)
            except RuntimeError: passed(name)
            else: raise AssertionError(name)
        # A kernel-mode return that ended in a listed controlled outcome: the
        # saved kernel return rebound after the forced stall, with status 1
        # and one parsed FAIL line. Offline fixture only.
        outcome = dict(CONTROLLED_FAULT)
        fault = dict(core=0, esr=0x96000005, elr=0x41001234, far=0x400000000, spsr=0x3c5, task='fault')
        controlled = dict(warm, initial_node=timeout['returned_node'], initial_identity=timeout['returned_identity'],
                          returned_node='/dev/bus/usb/003/121',
                          returned_identity=timeout['returned_identity'][:1]+[timeout['returned_identity'][1]+9]
                          +timeout['returned_identity'][2:],
                          result=dict(warm['result'], status=1),
                          controlled=dict(outcome=outcome, faults=[fault], log_found=list(outcome['log_contains'])))
        validate_chain((warm_raw, timeout_raw, json.dumps(controlled)), controlled)
        passed('modeled-controlled-outcome-continuity')
        for name, change in [
            ('controlled-other-status', lambda c: c.update(result=dict(c['result'], status=3))),
            ('controlled-missing-record', lambda c: c.pop('controlled')),
            ('controlled-two-fail-lines', lambda c: c.update(controlled=dict(c['controlled'], faults=[fault, fault]))),
            ('controlled-other-far', lambda c: c.update(controlled=dict(c['controlled'], faults=[dict(fault, far=0)]))),
            ('controlled-other-esr', lambda c: c.update(controlled=dict(c['controlled'],
                                                                        faults=[dict(fault, esr=0x96000045)]))),
            ('controlled-elr-outside', lambda c: c.update(controlled=dict(c['controlled'],
                                                                          faults=[dict(fault, elr=0x41002000)]))),
            ('controlled-missing-log-text', lambda c: c.update(controlled=dict(c['controlled'], log_found=[]))),
            ('controlled-malformed-outcome', lambda c: c.update(controlled=dict(
                c['controlled'], outcome=dict(outcome, status=0)))),
            ('controlled-timeout-mode', lambda c: c.update(mode='timeout'))]:
            changed = json.loads(json.dumps(controlled)); change(changed)
            try: validate_chain((warm_raw, timeout_raw, json.dumps(changed)), changed)
            except RuntimeError: passed(name)
            else: raise AssertionError(name)
        # The same link for an outcome with a progress record.
        progress = dict(magic=0x3130535342344242, stage=3, count=412, stamp=PROGRESS_END - 100000)
        stopped = dict(controlled, result=dict(warm['result'], status=2, interrupt=29, end_counter=PROGRESS_END),
                       controlled=dict(outcome=CONTROLLED_PROGRESS, faults=[], progress=progress,
                                       log_found=list(CONTROLLED_PROGRESS['log_contains'])))
        validate_chain((warm_raw, timeout_raw, json.dumps(stopped)), stopped)
        passed('modeled-controlled-progress-continuity')
        for name, change in [
            ('progress-missing', lambda c: c['controlled'].pop('progress')),
            ('progress-count-complete', lambda c: c['controlled']['progress'].update(count=1000)),
            ('progress-recent-stamp', lambda c: c['controlled']['progress'].update(stamp=PROGRESS_END - 24000)),
            ('progress-stamp-after-end', lambda c: c['controlled']['progress'].update(stamp=PROGRESS_END + 1)),
            ('progress-wrong-magic', lambda c: c['controlled']['progress'].update(magic=0)),
            ('progress-wrong-stage', lambda c: c['controlled']['progress'].update(stage=2)),
            ('progress-other-interrupt', lambda c: c['result'].update(interrupt=30)),
            ('progress-malformed-offset', lambda c: c['controlled']['outcome']['progress'].update(offset=96))]:
            changed = json.loads(json.dumps(stopped)); change(changed)
            try: validate_chain((warm_raw, timeout_raw, json.dumps(changed)), changed)
            except RuntimeError: passed(name)
            else: raise AssertionError(name)
        if args.native_return:
            # A saved native returning CPU session from another lifetime,
            # rebound to follow the forced-stall return. Offline fixture only.
            native = dict(json.loads(args.native_return.read_text()), port=port,
                          initial_node=timeout['returned_node'], initial_identity=timeout['returned_identity'],
                          returned_node='/dev/bus/usb/003/120',
                          returned_identity=timeout['returned_identity'][:1]+[timeout['returned_identity'][1]+7]+timeout['returned_identity'][2:])
            validate_chain((warm_raw, timeout_raw, json.dumps(native)), native)
            passed('modeled-native-return-continuity')
            for name, key, value in [
                ('native-dram-not-restored', 'dram_restored', False),
                ('native-rom-unusable', 'rom_usable', None),
                ('native-incomplete', 'complete', False),
                ('native-not-physical', 'physical_access', False),
                ('native-with-kernel-mode', 'mode', 'kernel'),
                ('native-broken-origin', 'initial_node', warm['returned_node'])]:
                changed = dict(native); changed[key] = value
                try: validate_chain((warm_raw, timeout_raw, json.dumps(changed)), changed)
                except RuntimeError: passed(name)
                else: raise AssertionError(name)

    # Exercise actual main/transaction/exception ordering and shared retention
    # helper. Only the device transport and lock are modeled; no USB discovery.
    modes = ('success-new-admission', 'legacy-signature-refusal', 'prelaunch-transport-loss', 'launch-transport-loss', 'prelaunch-interruption')
    if args.warm_return: modes += ('success-warm-admission',)
    modes += ('controlled-outcome-match', 'controlled-outcome-unlisted', 'controlled-progress-match',
              'controlled-progress-unlisted', 'returned-status-failure', 'returned-quarantine-failure')
    for mode in modes:
        if args.case and mode != args.case:
            continue
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td); image = tmp/'image.bin'; image.write_bytes(b'bounded-image')
            run_node, run_identity = (warm_node, warm_identity) if mode == 'success-warm-admission' else (node, identity)
            held = [False]; requests = []; quarantine = []; memory = {}; active = [run_identity]
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
                            size=int(parts[0]); Path(parts[1]).write_bytes(memory.get(address,b'\xa5'*size)[:size]); parts=parts[2:]
                        else: raise AssertionError(op)
                return SimpleNamespace(returncode=0,stdout=output,stderr='')
            def modules(name, path):
                return access if name == 'access' else helper
            argv=['--fel',str(tool),'--image',str(image),'--evidence',str(tmp/'run')]
            if mode != 'legacy-signature-refusal':
                argv += ['--ddr-evidence',str(paths[3]),'--platform-survey',str(paths[4])]
            if mode == 'success-warm-admission':
                argv[-1] = str(args.warm_survey)
                argv += ['--warm-return', str(args.warm_return)]
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
                    if mode == 'returned-quarantine-failure':
                        # A returned instance that could not be quarantined keeps the port lock.
                        assert not quarantine and requests[-1][0] == 'read'
                    else:
                        assert (mode.endswith('transport-loss') or mode == 'prelaunch-interruption') and len(quarantine)==1
                        assert requests[-1][0] == ('reset64' if mode.startswith('launch-') else 'version')
                except SystemExit as stop:
                    if mode == 'legacy-signature-refusal':
                        assert not quarantine
                        assert not any(r[0] in ('write','reset64') for r in requests)
                    else:
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
                    assert mode in ('success-new-admission', 'success-warm-admission', 'controlled-outcome-match',
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
                    if mode == 'success-warm-admission':
                        assert (tmp/'run/warm-return-1.json').read_bytes() == warm_raw
                        assert saved['admission']['warm_returns'] == [
                            dict(file='warm-return-1.json', sha256=hashlib.sha256(warm_raw).hexdigest())]
                        assert (tmp/'run/ddr-admission.json').read_bytes() == raw[0]
                        assert (tmp/'run/platform-survey.json').read_bytes() == warm_survey_raw
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
