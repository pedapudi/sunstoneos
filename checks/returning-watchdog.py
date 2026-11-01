#!/usr/bin/env python3
"""Check the host-armed watchdog and RTC breadcrumbs of returning images.

  returning-watchdog.py --out DIR [--image ELF] [--baseline ELF]
                        [--clang C --lld L --launcher-prepare PREPARE.PY]

Cases without --image use a modeled FEL: the shared transport arms WDT0 just
before exe, classifies the returned instance before any DRAM request, writes
the reset report and keeps the fresh ROM instance unquarantined. Host cases
check which failures quarantine the instance: a host exception after a
confirmed normal return does not; a failed or refused FEL request, an exception
before the confirmation and a return without RETURN_ROM do. The modeled
quarantine writes its records under the case's temporary directory. With --image,
the linked return image runs in the instruction model: an EL1 breadcrumb SVC,
the EL3 first-fault record and the disarm on the normal ROM return. No USB
node, queue or FEL executable is opened.

The OS-request launcher is built by an application's OS-image preparation
module, which provides build_launcher(out, session_bytes, clang, lld,
watchdog_mode). The launcher case loads that module from --launcher-prepare
with SUNSTONEOS_DIR set to this tree.
"""
import argparse
import contextlib
import importlib.util
import io
import itertools
import json
import os
from pathlib import Path
import re
import shutil
import tempfile
import types
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
EPOCH = 0x1234_5678_9abc_def1


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


crumbs = load('checked_crumbs', 'tools/a733-crumbs.py')
harness = load('checked_harness', 'checks/harness.py')
transport = load('checked_returning', 'tools/a733-returning-host.py')
base = load('checked_returning_base', 'tools/check-a733-smp.py')
rows = []


def passed(name, **extra):
    rows.append({'name': name, 'passed': True, **extra})


def refused(action, text):
    try:
        action()
    except (ValueError, AssertionError, RuntimeError) as error:
        assert text in str(error), (text, str(error))
        return str(error)
    raise AssertionError('accepted: ' + text)


def header_cases():
    defines, stages = crumbs.header()
    offsets = {name.lower(): defines[name] for name in ('TAG', 'LAST', 'TIME', 'FAULT', 'ESR', 'ELR', 'FAR')}
    assert offsets == crumbs.WORDS and defines['RTC'] == crumbs.RTC
    assert 0x108 not in crumbs.WORDS.values(), 'the FEL flag word is never a breadcrumb'
    assert len(set(stages)) == len(stages) and all(0 < s < 0x10000 for s in stages)
    assert defines['WATCHDOG_MODE'] == 0xb1 and crumbs.interval(0xb1) == 16 and crumbs.interval(0x41) == 4
    assert stages[crumbs.RETURN_ROM] == 'RETURN_ROM' and defines['SVC'] == 0x5c0d
    assert harness.CRUMB_SVC == 0xd4000001 | defines['SVC'] << 5
    arm = crumbs.arm_arguments(0xb1)
    assert '0x7090108' not in arm and arm[:-12] == sum((['writel', hex(crumbs.RTC + o), '0x0']
                                                        for o in crumbs.WORDS.values()), [])
    assert arm[-12:] == ['writel', '0x2050018', '0x16aa0002', 'writel', '0x2050010', '0x16aa0001',
                         'writel', '0x2050014', '0x16aa00b1', 'writel', '0x205000c', '0x14af']
    passed('header-decoder-layout-and-arm-order')
    words = dict.fromkeys(crumbs.WORDS, 0)
    words.update(tag=0x5c0d0000 | EPOCH & 0xffff, last=0x0303 << 16, time=(5 << 29) | 1000,
                 fault=0xfa000000 | 3 << 20 | 1 << 16 | 8 << 8 | 4, esr=0x96000045, elr=0x41012340, far=0x60)
    decoded = crumbs.decode(words, EPOCH)
    assert decoded['last']['name'] == 'RECOGNITION_START' and decoded['last']['core'] == 5
    assert decoded['fault']['esr'] == '0x96000045' and 'EL1 fault on core 3' in decoded['summary']
    words['tag'] ^= 1
    assert not crumbs.decode(words, EPOCH)['session']
    passed('decode-stage-fault-and-foreign-session')


def transport_case(variant, root):
    old, new = Path('/dev/bus/usb/003/056'), Path('/dev/bus/usb/003/057')
    port = Path('/sys/bus/usb/devices/3-3')
    records = root / 'recovery'
    state = {'locked': False, 'guard': False, 'node': old, 'launched': False, 'held': 0, 'quarantined': 0,
             'calls': [], 'after_return': [], 'mode': 0, 'rtc': {}}
    class Access:
        @contextlib.contextmanager
        def board_lock(self):
            state['locked'] = True
            try: yield old
            finally: state['locked'] = False
        def require_session(self, node): assert state['locked']
        def require_rom(self, node): assert state['locked']
        def identity(self, node): return [1, 57 if node == new else 56, 3, 4, 5]
        def target(self, node): return port
        def session_node(self, config): return state['node']
        def quarantine_instance(self, node, reason, observed=None):
            # Same record as tools/usb-access.py, kept in this case's directory.
            state['quarantined'] += 1
            records.mkdir(exist_ok=True)
            key = port.name + '-' + '-'.join(str(int(x)) for x in observed or self.identity(node))
            (records / f'recovery-required-{key}.json').write_text(json.dumps({'identity': observed, 'reason': reason}))
    class Guard:
        def check_health(self): pass
        @contextlib.contextmanager
        def reenumeration(self, *values):
            state['guard'] = True
            try: yield
            finally: state['guard'] = False
    def process(command, **kwargs):
        arguments, out = command[3:], []
        state['calls'].append(arguments)
        if state['launched']:
            state['after_return'].append(arguments)
        i = 0
        while i < len(arguments):
            op = arguments[i]
            if op == 'version':
                out.append('soc=00001903(A733)'); i += 1
            elif op == 'writel':
                address, value = int(arguments[i + 1], 0), int(arguments[i + 2], 0); i += 3
                if address == crumbs.WDT_MODE:
                    state['mode'] = value & 0xffff if variant != 'disarm-ignored' or not state['launched'] else state['mode']
                elif crumbs.RTC <= address < crumbs.RTC + 0x200:
                    state['rtc'][address - crumbs.RTC] = value
            elif op == 'readl':
                address = int(arguments[i + 1], 0); i += 2
                value = {crumbs.WDT_MODE: state['mode'], crumbs.WDT_CONFIG: 1,
                         crumbs.WDT_OUTPUT: 2 if variant != 'arm-readback' else 0}.get(address)
                if value is None:
                    value = state['rtc'].get(address - crumbs.RTC, 0) if address >= crumbs.RTC else 0
                out.append(hex(value))
            elif op == 'exe':
                assert state['guard'] and state['mode'] == (0 if variant == 'no-watchdog' else 0xb1), 'exe inside the guard, armed as asked'
                i += 2
                state['launched'] = True; state['node'] = new
                rtc = state['rtc']
                rtc[0x100] = 0x5c0d0000 | EPOCH & 0xffff if variant != 'fired-before-entry' else 0
                if variant in ('fired', 'disarm-ignored'):
                    rtc[0x104] = 0x0410 << 16 | 37; rtc[0x10c] = 6 << 29 | 77
                    if variant == 'fired':
                        state['mode'] = 0x01  # the image's EL3 stop shortened WDT0
                elif variant == 'disarmed-without-rom':
                    state['mode'] = 0; rtc[0x104] = 0x0421 << 16
                elif variant == 'rom-from-other-session':
                    state['mode'] = 0; rtc[0x104] = 0x0503 << 16; rtc[0x100] ^= 1
                elif variant != 'fired-before-entry':
                    # The image's disarm write clears the mode before RETURN_ROM.
                    state['mode'] = 0; rtc[0x104] = 0x0503 << 16
            elif op == 'read':
                Path(arguments[i + 3]).write_bytes(bytes(int(arguments[i + 2]))); i += 4
            else:
                raise AssertionError(op)
        failing = variant == 'fel-error-after-return' and arguments[:1] == ['read'] and state['launched']
        return types.SimpleNamespace(returncode=1 if failing else 0, stdout='\n'.join(out), stderr='')
    def admit(*values): assert state['locked']
    def validate(path): return {'image_sha256': 'f' * 64, 'epoch': EPOCH}
    def execute(transaction, launch, ready, p, record, save):
        ready(); record['dirty'] = True; save()
        assert int(transaction('readl', '0x02050014'), 16) & 1 == 0
        if variant == 'host-error-before-launch':
            raise KeyError('modeled host error before launch')
        launch()
        transaction('version'); transaction('read', '0x42000000', 128, str(root / 'report.bin'))
        if variant == 'host-error-after-return':
            # The 3-3 run of 2026-10-31: outputs read, then a missing host file.
            (root / 'return-review.json').read_text()
        if variant == 'fel-refused-after-return':
            transaction('exe', '0x52000')
        record.update(complete=True); save()
        return 'normal'
    def hold(): state['held'] += 1
    fel = root / 'fel'; fel.write_bytes(b'modeled, never executed')
    args = types.SimpleNamespace(bundle=root, fel=fel, evidence=root / 'evidence', root_owned_claim='MODEL ONLY')
    options = {'return_timeout': 90}
    if variant != 'no-watchdog':
        options['watchdog'] = {'mode': 0xb1, 'session_seconds': 0 if variant == 'invalid-session' else 60}
    clock = itertools.count(0, 0.5)
    decode = crumbs.decode
    if variant == 'host-error-during-classification':
        def decode(*values): raise KeyError('modeled host error before the return is confirmed')
    stderr = io.StringIO()
    with contextlib.redirect_stderr(stderr), patch.object(transport.crumbs, 'decode', decode), \
            patch.object(base, 'digest', return_value=base.FEL_HASH), patch.object(base, 'hold_ownership', hold), \
            patch.object(transport.subprocess, 'run', process), patch.object(transport.os, 'access', return_value=True), \
            patch.object(transport.time, 'monotonic', lambda: next(clock)), patch.object(transport.time, 'sleep', lambda s: None):
        try:
            result = transport.run(args, base=base, profile='model', validate_bundle=validate, admit=admit,
                                   execute=execute, description='model', access=Access(), guard=Guard(), **options)
            error = None
        except (ValueError, OSError, KeyError, transport.WatchdogReset) as failure:
            result, error = None, failure
    record = json.loads((args.evidence / 'session.json').read_text()) if (args.evidence / 'session.json').exists() else {}
    state['records'] = sorted(records.glob('recovery-required-*.json')) if records.exists() else []
    state['stderr'] = stderr.getvalue()
    return state, record, result, error, args.evidence


def quarantined(state, record):
    """One quarantine record for the returned or original instance, also stated in the run record."""
    assert state['quarantined'] == 1 and len(state['records']) == 1, state
    written = json.loads(state['records'][0].read_text())
    assert record['instance_quarantined'] == written['reason'] and not record.get('host_failed_after_normal_return')
    assert 'HOST FAILED AFTER NORMAL RETURN' not in state['stderr']
    return written


def transport_cases():
    with tempfile.TemporaryDirectory(prefix='returning-watchdog-') as temporary:
        for variant in ('normal', 'fired', 'fired-before-entry', 'no-watchdog', 'invalid-session', 'arm-readback',
                        'disarm-ignored', 'disarmed-without-rom', 'rom-from-other-session',
                        'host-error-after-return', 'fel-error-after-return', 'fel-refused-after-return',
                        'host-error-before-launch', 'host-error-during-classification'):
            root = Path(temporary) / variant; root.mkdir()
            state, record, result, error, evidence = transport_case(variant, root)
            if variant == 'normal':
                assert record['return_timeout_seconds'] == 86, 'armed: session, interval and 10 s'
                assert result == 'normal' and error is None and not state['held'] and not state['quarantined']
                assert not state['records'] and 'instance_quarantined' not in record
                n = record['normal_return']
                assert n['wdt0_disarmed'] and n['first_request_succeeded'] and n['return_rom'] == {
                    'session': True, 'stage': crumbs.RETURN_ROM, 'value': 0}
                assert n['returned_identity'] == record['returned_identity'] != record['initial_identity']
                w = record['watchdog']
                assert w['armed'] and w['disarmed'] and not w['fired'] and w['mode_after_return'] == 0
                assert w['breadcrumbs']['last']['name'] == 'RETURN_ROM'
                # The first request on the returned instance is the classification.
                assert state['after_return'][0] == crumbs.return_arguments()
            elif variant in ('fired', 'fired-before-entry'):
                assert isinstance(error, transport.WatchdogReset) and not state['held'] and not state['quarantined']
                assert record['watchdog_reset'] and record['dram_lost'] and record['rom_usable'] and not record['complete']
                assert record['failure'].startswith('WDT0 reset the board')
                report = json.loads((evidence / 'watchdog-report.json').read_text())
                assert report['watchdog_fired'] and state['mode'] == 0
                # Classification and the launcher-report read only: no DRAM request.
                assert len(state['after_return']) == 2 and state['after_return'][1][:2] == ['read', '0x52d00']
                assert report['stop_shortened'] == (variant == 'fired')
                assert report['reset_interval_seconds'] == (0.5 if variant == 'fired' else 16)
                if variant == 'fired':
                    assert report['breadcrumbs']['last'] == {'stage': 0x410, 'name': 'GPU_SUBMIT', 'value': 37,
                        'core': 6, 'seconds_since_power_on': round(77 * 1024 / 24e6, 4)}
                else:
                    assert not report['breadcrumbs']['session']
            elif variant == 'no-watchdog':
                assert record['return_timeout_seconds'] == 90
                assert result == 'normal' and not any('writel' in c for c in state['calls'])
            elif variant == 'host-error-after-return':
                # Failed with evidence and the usual hold, but no quarantine.
                assert isinstance(error, FileNotFoundError) and result is None, error
                assert not state['quarantined'] and not state['records'] and state['held'] == 1
                assert record['host_failed_after_normal_return'] and record['instance_quarantined'] is False
                assert not record['complete'] and 'return-review.json' in record['failure']
                assert record['normal_return']['first_request_succeeded'] and not record.get('fel_request_raised')
                assert 'HOST FAILED AFTER NORMAL RETURN: FileNotFoundError' in state['stderr']
                assert 'the board instance remains usable' in state['stderr']
            elif variant in ('fel-error-after-return', 'fel-refused-after-return'):
                text = {'fel-error-after-return': 'FEL request failed',
                        'fel-refused-after-return': 'guarded launch callback'}[variant]
                assert text in str(error) and record['normal_return'] and record['fel_request_raised']
                assert quarantined(state, record)['identity'] == record['returned_identity'] and state['held'] == 1
            elif variant in ('host-error-before-launch', 'host-error-during-classification'):
                assert isinstance(error, KeyError) and 'normal_return' not in record and state['held'] == 1
                identity = record['initial_identity'] if variant == 'host-error-before-launch' else record['returned_identity']
                assert quarantined(state, record)['identity'] == identity
            elif variant == 'invalid-session':
                assert 'Watchdog mode or session length differs' in str(error) and not state['calls']
            else:
                text = {'arm-readback': 'readback after arming', 'disarm-ignored': 'did not disarm',
                        'disarmed-without-rom': 'without this session', 'rom-from-other-session': 'without this session'}[variant]
                assert text in str(error) and state['held'] == 1 and state['quarantined'] == 1, (variant, error, state)
                assert 'normal_return' not in record and quarantined(state, record)
            passed('transport-' + variant, fel_requests=len(state['calls']))


def launcher_case(out, clang, lld, preparation):
    os.environ['SUNSTONEOS_DIR'] = str(ROOT)
    prepare = load('checked_os_prepare', Path(preparation).resolve())
    for mode, name in ((None, 'disabled'), (0xb1, 'armed')):
        target = out / ('launcher-' + name); target.mkdir()
        prepare.build_launcher(target, 904, clang, lld, mode)
        text = (target / 'launch.c').read_text() + (target / 'board-launch-base.c').read_text()
        armed = '(rd(0x02050014)&0xffff)==0xb1 && (rd(0x02050010)&0xffff)==1 && (rd(0x02050018)&0xffff)==2 &&'
        assert (armed in text) == (mode is not None)
        assert ('!(rd(0x02050014)&1) && !(rd(0x02050010)&0x100) &&' in text) == (mode is None)
        passed('launcher-' + name, bytes=(target / 'launch.bin').stat().st_size)


def native_cases(image_path, baseline):
    host = load('checked_os_host_marker', 'apps/os/host.py')
    marker = host.watchdog_marker(image_path)
    assert marker == {'mode': 0xb1, 'session_seconds': 60}, marker
    if baseline:
        assert host.watchdog_marker(baseline) is None
    passed('image-watchdog-marker')
    prior = load('checked_cpu_model', 'checks/cpu-runtime-qualification.py')
    model = lambda: prior.Model(prior.fault.Image(Path(image_path)))
    rtc = lambda m, offset: m.u32(crumbs.RTC + offset)
    m = model()
    for core in (2, 7):
        m.call(core, 'os_cpu_return_thermal_done')
        assert m.stop == 'return' and rtc(m, 0x104) == 0x0221 << 16 and rtc(m, 0x10c) >> 29 == core
    assert rtc(m, 0x110) == 0, 'a breadcrumb SVC is not a fault'
    passed('el1-svc-breadcrumb-and-eret')
    def el3_ready(m):
        # The fault record stores only after the EL3 identity-map checks pass:
        # the cached SCTLR/TCR/MAIR/TTBR state and every core's adopted record.
        l1 = m.s['a733_cached_el3_l1']
        for c in range(8):
            m.reg[c].update(scr_el3=0x531, sctlr_el3=0x1005, tcr_el3=0x80823019, mair_el3=0x44ff00, ttbr0_el3=l1)
            record = m.s['a733_cached_records'] + c * 128
            m.uc.mem_write(record, prior.struct.pack('<3Q', prior.EPOCH, c, 3))
            m.put64(record + 64, 0x1005)
            m.put64(record + 72, l1)
    m = model()
    m.core = 3
    el3_ready(m)
    for name, value in (('esr_el1', 0x96000045), ('elr_el1', 0x41012340), ('far_el1', 0x13579bdf)):
        m.reg[3][m.key(name)] = value
    m.call(3, 'a733_cached_cold_fault')
    fault = rtc(m, 0x110)
    assert fault == 0xfa000000 | 3 << 20 | 1 << 16 | 8 << 8 | 4, hex(fault)
    assert (rtc(m, 0x114), rtc(m, 0x118), rtc(m, 0x11c)) == (0x96000045, 0x41012340, 0x13579bdf)
    m.reg[4][m.key('esr_el1')] = 0x2000000
    m.call(4, 'a733_cached_cold_fault')
    assert rtc(m, 0x110) == fault and rtc(m, 0x114) == 0x96000045, 'the first fault is kept'
    passed('el3-first-fault-record', stop=m.stop)
    m = model()
    m.reg[3][m.key('esr_el1')] = 0x96000045
    m.call(3, 'a733_cached_cold_fault')
    assert m.stop == 'park' and rtc(m, 0x110) == 0, 'no RTC store before the EL3 map checks pass'
    passed('el3-fault-record-waits-for-map-checks')
    m = model()
    m.call(0, 'a733_cached_smc', (0, 0))
    assert rtc(m, 0x110) == 0, 'the success SMC records no fault'
    passed('success-smc-records-nothing')
    m = model()
    m.put32(0x02050014, 0xb1)
    m.call(0, 'a733_qualification_rom')
    assert m.stop == 'rom'
    writes = [(a, v) for _, a, size, v in m.writes if a == 0x02050014]
    assert writes == [(0x02050014, 0x16aa0000)], writes
    assert rtc(m, 0x104) == 0x0503 << 16
    passed('rom-return-disarms-before-rmr')
    # A disarm that does not take: record the mode read back and park.
    m = model()
    m.put32(0x02050014, 0xb1)
    rom = m.s['a733_qualification_rom']
    def still_enabled(uc, address, size, unused):
        if int.from_bytes(uc.mem_read(address, 4), 'little') == 0xb9401401:  # ldr w1, [x0, #0x14]
            m.put32(0x02050014, 0xb1)
    m.uc.hook_add(prior.UC_HOOK_CODE, still_enabled, begin=rom, end=rom + 0x40)
    m.call(0, 'a733_qualification_rom')
    assert m.stop == 'park' and rtc(m, 0x104) == 0x0503 << 16 | 0xb1
    assert not any(a == 0x08001004 for _, a, _, v in m.writes), 'no RVBAR write before parking'
    passed('rom-return-parks-while-enabled')
    # The EL3 stop: record EL3_STOP, then shorten only the host-armed WDT0.
    class Stop(prior.Model):
        def code(self, uc, a, size, unused):
            if a != self.s['a733_smp_stop']:
                super().code(uc, a, size, unused)
    for mode, shortened in ((0xb1, True), (0, False), (0x41, False)):
        m = Stop(prior.fault.Image(Path(image_path)))
        m.put32(0x02050014, mode)
        m.call(0, 'a733_smp_stop', (3, 0x736))
        writes = [(a, v) for _, a, size, v in m.writes if 0x02050000 <= a < 0x02051000]
        assert m.stop == 'park' and rtc(m, 0x104) == 0x0111 << 16 | 3 << 12 | 0x736, hex(rtc(m, 0x104))
        assert writes == ([(0x02050014, 0x16aa0001), (0x0205000c, 0x14af)] if shortened else []), (mode, writes)
    passed('el3-stop-shortens-armed-watchdog')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--image', type=Path)
    parser.add_argument('--baseline', type=Path)
    parser.add_argument('--clang', type=Path)
    parser.add_argument('--lld', type=Path)
    parser.add_argument('--launcher-prepare', type=Path,
                        help="Application OS-image preparation module providing build_launcher")
    args = parser.parse_args()
    if (args.clang or args.lld or args.launcher_prepare) and not (args.clang and args.lld and args.launcher_prepare):
        parser.error('the launcher case needs --clang, --lld and --launcher-prepare together')
    args.out.mkdir(parents=True, exist_ok=False)
    header_cases()
    transport_cases()
    if args.clang and args.lld:
        launcher_case(args.out, args.clang, args.lld, args.launcher_prepare)
    if args.image:
        native_cases(args.image, args.baseline)
    (args.out / 'results.json').write_text(json.dumps({'physical_access': False, 'cases': rows}, indent=1) + '\n')
    print('PASS returning watchdog:', len(rows), 'cases')


if __name__ == '__main__':
    main()
