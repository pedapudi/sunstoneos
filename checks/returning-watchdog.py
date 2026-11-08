#!/usr/bin/env python3
"""Check the host-armed watchdog and RTC breadcrumbs of returning images.

  returning-watchdog.py --out DIR [--image ELF] [--baseline ELF] [--clang C --lld L]

Without --image, check the breadcrumb layout and decoder. Transport, quarantine
and watchdog classification use the fake-board cases in checks/board-runner.py.
The optional image checks execute the actual native watchdog and fatal paths.
"""
import argparse
import contextlib
import importlib.util
import io
import itertools
import json
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
    assert defines['FAIL_SVC'] == 0x5c0e and defines['FAIL_LEVEL'] == crumbs.FAIL_LEVEL
    assert defines['FAIL_RETURN'] == 1 << 31
    groups = {k: v for k, v in defines.items() if k.startswith('FAIL_') and k not in ('FAIL_SVC', 'FAIL_LEVEL', 'FAIL_LEVEL_SECOND', 'FAIL_RETURN')}
    assert len(set(groups.values())) == len(groups) and all(0 < v < 16 for v in groups.values())
    words = dict.fromkeys(crumbs.WORDS, 0)
    words.update(tag=0x5c0d0000 | EPOCH & 0xffff, last=crumbs.EL3_STOP << 16 | 15 << 12 | 0x736,
                 fault=0xfa0b0000 | 6 << 20, esr=1 << 31 | 3 << 12 | 60 << 16 | 0x736,
                 elr=0x4101a1c4, far=1 << 28 | 1 << 31 | 2900)
    decoded = crumbs.decode(words, EPOCH)
    f = decoded['board_failure']
    assert decoded['last']['status'] == '15 or more' and decoded['last']['detail'] == '0x736'
    assert f['group'] == 'cpu_return' and f['site'] == 0x736 and f['deadline_seconds'] == 60
    assert f['exception_level'] == 'EL0' and f['core'] == 6 and f['caller_kind'] == 'return address'
    assert f['refusal'] == ['board.fatal already set', 'within 10 ms of the deadline'] and f['counter_ms'] == 2900
    assert 'fault' not in decoded and 'first board failure on core 6 at EL0: cpu_return' in decoded['summary']
    words.update(fault=0xfa0b0000 | 1 << 20 | 5 << 8, esr=2 << 12 | 238, far=0x30002)
    f = crumbs.decode(words, EPOCH)['board_failure']
    assert (f['group'], f['site'], f['exception_level'], f['caller_kind']) == ('thermal', 238, 'EL1', 'statement')
    # An entry-guard rail refusal in the REQUEST phase: the full refusal word
    # gives both rails, DCDC5 1000 mV and DCDC3 1050 mV, in 10 mV units.
    assert defines['CPU_GUARD'] == 1 << 30 and defines['CPU_AUX'] == 1 << 29
    words.update(esr=1 << 30 | 1 << 29 | (2 << 8) << 16 | 5 << 12 | 192,
                 far=(100 << 10 | 105) << 12 | crumbs.GUARD_RAIL_LINE)
    f = crumbs.decode(words, EPOCH)['board_failure']
    assert (f['group'], f['site'], f['phase'], f['cluster'], f['guard_line']) == ('cpu_clock', 192, 2, 0, crumbs.GUARD_RAIL_LINE)
    assert (f['dcdc5_mv'], f['dcdc3_mv']) == (1000, 1050), f
    words.update(esr=1 << 29 | (3 << 8 | 1) << 16 | 5 << 12 | 200, far=4)
    f = crumbs.decode(words, EPOCH)['board_failure']
    assert (f['phase'], f['cluster'], f['status']) == (3, 1, '0x4') and 'guard_line' not in f, f
    # An image without the layout flag packed phase and cluster into the value.
    words.update(esr=5 << 12 | 192, far=2 << 24 | 0x901c)
    f = crumbs.decode(words, EPOCH)['board_failure']
    assert (f['phase'], f['cluster'], f['status'], f['status_bits']) == (2, 0, '0x901c', 16), f
    passed('decode-board-failure-and-stop-status')








def launcher_case(out, clang, lld):
    prepare = load('checked_os_prepare', 'apps/os/prepare.py')
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
    assert rtc(m, 0x10c) & 0x1fffffff and rtc(m, 0x10c) >> 29 == 0, 'RETURN_ROM records its time and core'
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
    m = Stop(prior.fault.Image(Path(image_path)))
    m.call(0, 'a733_smp_stop', (54, 0x1736))
    assert m.stop == 'park' and rtc(m, 0x104) == 0x0111 << 16 | 15 << 12 | 0x736, hex(rtc(m, 0x104))
    passed('el3-stop-clamps-status')
    board_failure_cases(image_path, prior, model, rtc)


def board_failure_cases(image_path, prior, model, rtc):
    """The first board_fail names its site, value, caller, core and level."""
    class El1(prior.Model):
        # An SVC from EL1h: SPSR.M = 5 and the current-EL SPx sync vector.
        def code(self, uc, a, size, unused):
            if self.image.insn(uc, a).mnemonic == 'svc':
                super().code(uc, a, size, unused)
                self.set('spsr_el1', 5)
                uc.reg_write(prior.UC_ARM64_REG_PC, self.s['a733_cached_el1_vectors'] + 4 * 128)
                return
            super().code(uc, a, size, unused)
    sentinel = prior.fault.SENTINEL & 0xffffffff
    from elftools.elf.elffile import ELFFile
    with open(image_path, 'rb') as f:
        symbols = {x.name: x['st_value'] for x in ELFFile(f).get_section_by_name('.symtab').iter_symbols()
                   if x.name in ('fail_recorded', 'bounded', 'ss_fail')}
    m = model()
    m.call(2, 'board_fail', (0x1234,))
    site = rtc(m, 0x114)
    assert m.stop == 'return' and rtc(m, 0x110) == 0xfa0b0000 | 2 << 20, hex(rtc(m, 0x110))
    assert site >> 31 and site >> 12 & 0x7ffff == 1 and site & 0xfff, hex(site)
    assert (rtc(m, 0x118), rtc(m, 0x11c)) == (sentinel, 0x1234)
    first = [rtc(m, o) for o in (0x110, 0x114, 0x118, 0x11c)]
    m.call(5, 'board_fail_at', (0x5123, 7, 0x41001234))
    assert m.stop == 'return' and [rtc(m, o) for o in (0x110, 0x114, 0x118, 0x11c)] == first, 'first writer wins'
    # Even with the RAM claim lost, the vector keeps an earlier record.
    claim = symbols['fail_recorded']
    assert m.u32(claim) == 1
    m.put32(claim, 0)
    m.call(5, 'board_fail_at', (0x5123, 7, 0x41001234))
    assert [rtc(m, o) for o in (0x110, 0x114, 0x118, 0x11c)] == first, 'the vector keeps a set FAULT'
    passed('board-fail-first-record-el0', site=hex(site))
    m = El1(prior.fault.Image(Path(image_path)))
    m.call(1, 'board_fail_at', (0x5123, 9, 0x41001234))
    assert m.stop == 'return' and rtc(m, 0x110) == 0xfa0b0000 | 1 << 20 | 5 << 8, hex(rtc(m, 0x110))
    assert (rtc(m, 0x114), rtc(m, 0x118), rtc(m, 0x11c)) == (0x5123, 0x41001234, 9)
    passed('board-fail-record-el1')
    # An application's ss_fail(detail): the decoder names the group, the
    # detail and the symbolized caller from the image.
    m = model()
    m.call(4, 'ss_fail', (0x123,))
    site = rtc(m, 0x114)
    # ss_fail does not return: after the record, its task suspends and the core parks.
    assert m.stop == 'park' and site == 1 << 31 | 4 << 12 | 0x123 and rtc(m, 0x118) == sentinel, (m.stop, hex(site))
    words = {name: rtc(m, offset) for name, offset in crumbs.WORDS.items()}
    words['tag'] = 0x5c0d0000 | EPOCH & 0xffff
    # The model's caller is a sentinel; a return address inside the image
    # shows the symbolization.
    words['elr'] = symbols['ss_fail'] + 4
    decoded = crumbs.decode(words, EPOCH, image_path)
    f = decoded['board_failure']
    assert (f['group'], f['site_kind'], f['site'], f['value'], f['core'], f['exception_level']) == (
        'application', 'detail', 0x123, '0x123', 4, 'EL0'), f
    assert f['caller_source'].startswith('ss_fail') and 'apps/os/application.c' in f['caller_source'], f
    assert 'first board failure on core 4 at EL0: application detail 0x123' in decoded['summary'], decoded['summary']
    passed('application-failure-site', summary=decoded['summary'])
    m = model()
    m.put32(crumbs.RTC + 0x110, 0xfa010000 | 3 << 20 | 8 << 8 | 4)
    m.call(2, 'board_fail', (0,))
    assert rtc(m, 0x110) == 0xfa010000 | 3 << 20 | 8 << 8 | 4 and rtc(m, 0x114) == 0, 'an earlier fault is kept'
    passed('board-fail-keeps-earlier-fault')
    # bounded() with no session window: the record says why board_live refused.
    m = model()
    m.s['bounded'] = symbols['bounded']
    m.time = 24 * 3_000_000
    m.call(0, 'bounded', budget=2000000)
    site, value = rtc(m, 0x114), rtc(m, 0x11c)
    assert rtc(m, 0x110) == 0xfa0b0000 and site == 1 << 31 | 3 << 12 | 0x736, hex(site)
    assert value >> 28 == 0b0110 and value & 0x0fffffff == 3000, hex(value)
    passed('bounded-records-refusal-bits', value=hex(value))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--image', type=Path)
    parser.add_argument('--baseline', type=Path)
    parser.add_argument('--clang', type=Path)
    parser.add_argument('--lld', type=Path)
    args = parser.parse_args()
    if bool(args.clang) != bool(args.lld):
        parser.error('the launcher case needs --clang and --lld together')
    args.out.mkdir(parents=True, exist_ok=False)
    header_cases()
    if args.clang and args.lld:
        launcher_case(args.out, args.clang, args.lld)
    if args.image:
        native_cases(args.image, args.baseline)
    (args.out / 'results.json').write_text(json.dumps({'physical_access': False, 'cases': rows}, indent=1) + '\n')
    print('PASS returning watchdog:', len(rows), 'cases')


if __name__ == '__main__':
    main()
