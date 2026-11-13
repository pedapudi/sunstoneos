#!/usr/bin/env python3
"""Execute resident completion through the linked supervisor, exit hook and vectors.

Service timestamps, the admitted session, PMIC replies and Type-C cleanup are
modeled inputs. USB0 startup, application success, supervision, failure recording
and EL3 stop code execute.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import struct

from elftools.elf.elffile import ELFFile
from harness import Image

ROOT = Path(__file__).resolve().parents[1]
GATE_RUNS = [['{elf:resident-stop}', '--out', '{output}']]
CHECK_INPUTS = ['apps/os/application.c', 'apps/os/main.patch', 'apps/os/services.c',
                'apps/os/boot.patch', 'apps/os/vectors.patch', 'apps/os/sealed-lifetime.h',
                'apps/os/usb0.c', 'apps/board/usb0_service.h', 'platform/a733/native_usb.c',
                'include/sunstone_usb0_stop.h', 'tools/a733-crumbs.py']
spec = importlib.util.spec_from_file_location('resident_trace', ROOT / 'checks/el3-stop-trace.py')
trace = importlib.util.module_from_spec(spec)
spec.loader.exec_module(trace)
prior = trace.prior
spec = importlib.util.spec_from_file_location('resident_crumbs', ROOT / 'tools/a733-crumbs.py')
crumbs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(crumbs)


class Model(trace.Trace):
    def __init__(self, image, layout):
        super().__init__(image)
        self.layout = layout
        self.lifetime(2, None, 2)
        self.el3_ready(True)
        self.reg[0]['currentel'] = 4
        board = layout.address('board', 'os-image/main.c')
        bf, sf = layout.layouts['board_application'], layout.layouts['a733_board_session']
        self.put64(board + bf['live_until'], ((1 << 64) - 1) // 24)
        self.put64(board + bf['session'] + sf['deadline_us'], ((1 << 64) - 1) // 24)
        self.put32(board + bf['session'] + sf['workload'], 2)
        self.session = self.s['os_sealed']
        self.put32(self.session + layout.layouts['os_session']['board'] + sf['workload'], 2)
        self.put32(self.session + layout.layouts['os_session']['granted'], 0)
        self.put32(layout.address('granted_word', 'apps/os/grants.c'), 0x80000000)
        seen = layout.address('seen', 'apps/os/services.c')
        with image.path.open('rb') as stream:
            count = next(s['st_size'] // 8 for s in ELFFile(stream).get_section_by_name('.symtab').iter_symbols()
                         if s.name == 'seen' and s['st_value'] == seen)
        self.uc.mem_write(seen, struct.pack('<%dQ' % count, *([1] * count)))
        self.finished = layout.address('finished', 'apps/os/application.c')
        self.completion = layout.address('completion_stop')
        self.put32(self.finished, 1)
        self.put64(self.s['ss_result_record'] + 40, 1)
        self.put32(0x02050014, 0x41)
        self.hooks = {self.s['os_session_admitted']: lambda: self.session,
                      self.s['a733_typec_revoke']: lambda: 1,
                      # The supervisor's precise observation deadline (apps/os/supervisor-wait.c) is
                      # outside this model; each case decides on its first pass.
                      self.s['os_supervisor_wait_init']: lambda: 1}

    def code(self, uc, address, size, unused):
        if address == self.s['os_supervisor_wait_next']:
            # A pass that does not stop renewal waits for its next observation.
            self.stop = 'observation-wait'
            uc.emu_stop()
            return
        if address in self.hooks:
            uc.reg_write(prior.fault.REGS[0], self.hooks[address]())
            uc.reg_write(prior.UC_ARM64_REG_PC, uc.reg_read(prior.UC_ARM64_REG_LR))
            return
        instruction = self.image.insn(uc, address).mnemonic
        if instruction == 'svc':
            super().code(uc, address, size, unused)
            mode = getattr(self, 'svc_mode', 5)
            self.set('spsr_el1', mode)
            uc.reg_write(prior.UC_ARM64_REG_PC,
                         self.s['a733_cached_el1_vectors'] + (4 if mode == 5 else 0) * 128)
            return
        if instruction == 'smc':
            self.reg[self.core]['currentel'] = 12
        super().code(uc, address, size, unused)


class USBModel(Model):
    """Execute the owner with a modeled PMIC result, before USB MMIO starts.

    The two replies deliberately fail different predicates of native_usb_step.
    They establish what the retained record distinguishes, not which physical
    predicate failed in entry 1794468954726206112-8d88da24.
    """

    def __init__(self, image, layout, result):
        super().__init__(image, layout)
        nf = layout.layouts['a733_native_usb']
        provider = layout.address('providers') + layout.layouts['board_provider']['shared']
        self.put64(provider + nf['live'], self.s['board_live'])
        self.put32(provider + nf['pmic_ready'], 1)
        self.put32(provider + nf['usb_step'], 3)
        self.put32(self.finished, 0)
        self.put32(self.session + layout.layouts['os_session']['granted'], 1 << 6)
        self.put32(layout.address('granted_word', 'apps/os/grants.c'), 0x80000040)
        self.reg[1]['currentel'] = 4
        self.svc_mode = 4  # The retained physical owner runs at EL1t, using SP_EL0.
        self.hooks[self.s['a733_pmic_result']] = lambda: result

    def code(self, uc, address, size, unused):
        if address == self.s['vTaskSuspend']:
            self.stop = 'usb-suspended'
            uc.emu_stop()
            return
        super().code(uc, address, size, unused)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    layout = Image(args.elf, {'board_application', 'a733_board_session', 'os_session',
                              'board_provider', 'a733_native_usb'})
    image = prior.fault.Image(args.elf)
    rows = []
    for status in (1, 2):
        m = Model(image, layout)
        m.put64(m.s['ss_result_record'] + 40, status)
        assert m.call(0, 'os_application_succeeded') == (status == 1)
        m.put32(m.finished, 0)
        assert m.call(0, 'os_application_succeeded') == 0
        rows.append('application-result-%d' % status)
    for kind in ('completion', 'board-failure-before', 'board-failure-after', 'fault'):
        m = Model(image, layout)
        if kind == 'board-failure-before':
            m.call(0, 'board_fail_at', (0x5123, 9, 0x41001234))
        if kind == 'fault':
            m.put64(m.s['a733_fault_pending'], 1)
        if kind == 'board-failure-after':
            assert m.call(0, 'board_resident_renew', (0, 4 * 24000000)) == 0
            assert m.uc.mem_read(m.completion, 1) == b'\1'
            m.call(0, 'board_fail_at', (0x5123, 9, 0x41001234))
        m.s['resident_supervisor'] = layout.address('supervise', 'os-image/main.c')
        m.call(0, 'resident_supervisor')
        assert m.stop == 'park', (kind, m.stop)
        last, fault = m.u32(0x07090104), m.u32(0x07090110)
        if kind == 'completion':
            assert last == 0x01115000 and not fault, (hex(last), hex(fault))
        else:
            assert last != 0x01115000 or fault, (kind, hex(last), hex(fault))
        assert not any(0x02050000 <= a < 0x02051000 for _, a, _, _ in m.writes)
        rows.append(kind)
    build = args.elf.resolve().parent
    generated = (build / 'os-image/main.c').read_text()
    assert 'if(!i||(i==7&&(os_returning()||BOARD_RESIDENT())))vTaskCoreAffinitySet(board.handles[i],1);' in generated
    rows.append('generated-supervisor-core-zero')
    for seconds in (13.8148, 17, 67):
        m = Model(image, layout)
        m.time = int(seconds * 24000000)
        assert m.call(1, 'board_live') == 1
        rows.append('resident-live-at-%gs' % seconds)
    usb_records = []
    for cause, result in (('pmic-invalid', 5), ('snapshot-invalid', 2)):
        m = USBModel(image, layout, result)
        m.time = int(13.8148 * 24000000)
        assert m.call(1, 'board_usb0_task') == 0
        assert m.stop == 'usb-suspended'
        assert m.u64(m.s['a733_fault_pending']) == 0
        # The reason and underlying provider result survive the common stop.
        record = {key: m.u32(address) for key, address in
                  (('fault', 0x07090110), ('esr', 0x07090114),
                   ('elr', 0x07090118), ('far', 0x0709011c))}
        assert record['fault'] == 0xfa1b0400 and record['far'] == result
        assert record['esr'] & 0xffff == 0xb007 and record['esr'] >> 16 & 0x7fff == 3
        m.call(0, 'resident_supervisor')
        assert m.stop == 'park' and m.u32(0x07090104) == 0x01115fff
        usb_records.append({'cause': cause, 'record': record, 'stop': '0x01115fff'})
        rows.append(cause)
    assert usb_records[0]['record'] != usb_records[1]['record']
    reason_records = []
    for entry, arguments, reason, result in (
            ('a733_usb_assert', (), 4, 0), ('a733_usb_poll_enable', (0,), 2, 0),
            ('a733_usb_prepare', (), 1, 0xffffffff),
            ('board_usb0_provider_fail', (0,), 23, 0)):
        m = USBModel(image, layout, 2)
        if entry == 'a733_usb_poll_enable':
            m.put32(layout.address('board', 'os-image/main.c') + layout.layouts['board_application']['fatal'], 1)
        if entry == 'a733_usb_prepare':
            m.hooks[m.s['a733_native_usb_health']] = lambda: 0xffffffff
        m.call(1, entry, arguments)
        assert m.stop == 'usb-suspended'
        site = m.u32(0x07090114)
        assert site & 0xffff == 0xb000 | reason and m.u32(0x0709011c) == result
        record = {key: m.u32(0x07090000 + offset) for key, offset in crumbs.WORDS.items()}
        decoded = crumbs.board_failure(record, crumbs.header()[0], None)
        assert decoded['usb0_guard'] == crumbs.USB0_REASONS[reason]
        assert decoded['provider_result'] == (result if result < 0x80000000 else result - (1 << 32))
        reason_records.append(site & 0xfff)
        rows.append('guard-refusal-' + entry)
    assert len(set(reason_records)) == 4
    m = USBModel(image, layout, 2)
    m.put32(m.finished, 1)
    assert m.call(0, 'board_resident_renew', (0, 4 * 24000000)) == 0
    assert m.uc.mem_read(m.completion, 1) == b'\1'
    m.call(0, 'a733_storage_maintenance_stop', (5,))
    assert m.call(1, 'a733_usb_poll_enable', (0,)) == 0 and m.stop == 'usb-suspended'
    assert m.u64(m.s['a733_fault_pending']) == 0
    m.call(0, 'resident_supervisor')
    assert m.stop == 'park' and m.u32(0x07090104) == 0x01115000 and m.u32(0x07090110)
    rows.append('completion-close-usb-race-has-detail-zero')
    (args.out / 'results.json').write_text(json.dumps(
        {'physical_access': False, 'cases': rows, 'usb_provider_records': usb_records,
         'forced_guard_codes': reason_records}, indent=2) + '\n')
    print('PASS %d linked resident cases; no device access' % len(rows))


if __name__ == '__main__':
    main()
