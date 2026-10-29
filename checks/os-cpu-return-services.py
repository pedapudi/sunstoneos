#!/usr/bin/env python3
"""Execute linked close coordination with modeled RTOS and completed CPU DVFS.

The thermal driver's STOPPED result and completed clock transition are API
fixtures. The actual thermal owner masks its SPI and publishes completion;
the actual PMIC idle predicate and supervisor notification order execute.
"""
import argparse
import importlib.util
from pathlib import Path
import struct
from unicorn.arm64_const import UC_ARM64_REG_PC, UC_ARM64_REG_LR
from harness import Image, save

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('return_services_prior', ROOT / 'checks/cpu-runtime-qualification.py')
prior = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prior)


class Model(prior.Model):
    def __init__(self, image, layout):
        super().__init__(image)
        self.layout = layout
        for c in range(8):
            self.core = c
            self.reg[c].update(currentel=4, scr_el3=0x531, daif=0)
            self.set('tpidr_el1', c)
        self.core = 0
        self.events = []
        self.handles = []
        self.stop_result = 7
        self.mutex_held = False
        self.hooks = {}
        for name in ('vTaskEnterCritical', 'vTaskExitCritical', 'os_cpu_clock_step'):
            self.hooks[layout.address(name)] = lambda: 1
        self.hooks[layout.address('os_cpu_return_clock_done')] = lambda: 1
        self.hooks[layout.address('xQueueGetMutexHolder')] = lambda: int(self.mutex_held)
        self.hooks[layout.address('xTaskGetCurrentTaskHandle')] = lambda: 0x3000
        self.hooks[layout.address('a733_thermal_stop')] = self.thermal_stop
        self.hooks[layout.address('xTaskGenericNotify')] = self.notify
        self.hooks[layout.address('xTaskCreateStaticAffinitySet')] = self.create
        self.hooks[layout.address('vTaskDelay')] = self.delay
        self.board = layout.address('board')
        self.shared = layout.address('providers') + layout.layouts['board_provider']['shared']
        self.pmic = self.shared + layout.layouts['a733_native_usb']['pmic']
        self.thermal = layout.address('thermal', 'return-thermal.c')
        self.flags = {name: layout.address(name, 'cpu-return.c')
                      for name in ('closing', 'thermal_done', 'pmic_done', 'pmic_stop', 'ready')}
        board_fields = layout.layouts['board_application']
        session_fields = layout.layouts['a733_board_session']
        session = self.board + board_fields['session']
        self.put64(session + session_fields['deadline_us'], 60000000)
        self.put64(session + session_fields['epoch'], 4660)
        self.put64(session + session_fields['generation'], 9)
        self.put32(session + session_fields['workload'], 2)
        self.put64(self.board + board_fields['live_until'], 60000000)
        self.put64(self.board + board_fields['handles'], 0x1000)
        self.put32(self.pmic + layout.layouts['a733_pmic']['active'], 8)
        tf = layout.layouts['os_thermal_service']
        self.put64(self.thermal + tf['handle'], 0x3000)
        self.put32(self.thermal + tf['active'], 1)
        self.put64(self.thermal + tf['next_read'], 100000000)
        self.put64(self.thermal + tf['irq'], 0x03400000)
        self.put32(self.thermal + tf['irq'] + 8, 71)

    def create(self):
        core = self.uc.reg_read(prior.fault.REGS[3])
        assert core == len(self.handles)
        assert self.uc.reg_read(prior.fault.REGS[7]) == 1 << core
        self.handles.append(0x2000 + core)
        return self.handles[-1]

    def notify(self):
        self.events.append(('notify', self.uc.reg_read(prior.fault.REGS[0])))
        return 1

    def delay(self):
        self.time += 2400
        return 0

    def thermal_stop(self):
        # SPI disable must precede converter/local-source shutdown.
        assert any(address == 0x03400188 and value == 1 << 7 for _, address, _, value in self.writes)
        assert self.u32(self.flags['thermal_done']) == self.u32(self.flags['pmic_stop']) == 0
        self.events.append(('thermal-stop', self.stop_result))
        return self.stop_result

    def code(self, uc, address, size, unused):
        if address == self.layout.address('platform_exit'):
            self.stop = 'failure'
            self.events.append(('failure', self.uc.reg_read(prior.fault.REGS[0])))
            uc.emu_stop()
        elif address == self.layout.address('vTaskSuspend'):
            self.stop = 'suspended'
            self.events.append(('suspend', self.core))
            uc.emu_stop()
        elif address in self.hooks:
            uc.reg_write(prior.fault.REGS[0], self.hooks[address]())
            uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_LR))
        elif self.image.insn(uc, address).mnemonic == 'eret':
            # Unlike the terminal-only model, coordination resumes after the
            # real port's SVC109 core query. Its ELR is modeled with the other
            # architectural registers, rather than Unicorn's unrelated bank.
            uc.reg_write(UC_ARM64_REG_PC, self.get('elr_el1'))
        else:
            super().code(uc, address, size, unused)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    names = {'board_application', 'a733_board_session', 'board_provider', 'a733_native_usb',
             'a733_pmic', 'a733_pmic_slot', 'a733_platform_locks', 'os_thermal_service'}
    layout = Image(args.elf, names)
    image = prior.fault.Image(args.elf)
    rows = []
    model = Model(image, layout)
    assert model.call(0, 'os_cpu_return_start') == 1 and len(model.handles) == 8
    model.put32(model.flags['closing'], 1)
    model.call(0, 'os_cpu_return_supervise')
    assert not model.events and not model.u32(model.flags['pmic_stop'])
    model.call(0, 'os_thermal_task')
    assert model.stop == 'suspended' and model.events == [('thermal-stop', 7), ('suspend', 0)]
    assert model.u32(model.flags['thermal_done']) == 1 and not model.u32(model.flags['pmic_stop'])
    model.events.clear()
    model.call(0, 'os_cpu_return_supervise')
    assert model.events == [('notify', 0x1000)] and not model.u32(model.flags['ready'])
    model.events.clear()
    model.call(0, 'os_cpu_return_pmic')
    assert model.stop == 'suspended' and model.u32(model.flags['pmic_done']) == 1
    model.events.clear()
    model.call(0, 'os_cpu_return_supervise')
    assert model.events == [('notify', h) for h in model.handles[1:] + model.handles[:1]] + [('suspend', 0)]
    assert model.u32(model.flags['ready']) == 1
    rows.append({'name': 'thermal-pmic-and-cpu0-last-order', 'passed': True, 'events': model.events})
    for case in ('pmic-retained', 'pmic-slot-owned', 'mutex-held', 'usb-started', 'thermal-stop-failed',
                 'pmic-deadline', 'ths-rwp-deadline'):
        model = Model(image, layout)
        model.put32(model.flags['closing'], 1)
        if case in ('thermal-stop-failed', 'ths-rwp-deadline'):
            if case == 'thermal-stop-failed':
                model.stop_result = 5
            else:
                model.time = (60000000 - 10050) * 24
                model.put32(0x03400000, 1 << 31)
            model.call(0, 'os_thermal_task')
            assert not model.u32(model.flags['thermal_done']) and not model.u32(model.flags['pmic_stop'])
        else:
            model.put32(model.flags['thermal_done'], 1)
            model.put32(model.flags['pmic_stop'], 1)
            if case == 'pmic-retained':
                model.put32(model.pmic + layout.layouts['a733_pmic']['retained'], 1)
            elif case == 'pmic-slot-owned':
                slot = model.pmic + layout.layouts['a733_pmic']['slots']
                model.put32(slot + layout.layouts['a733_pmic_slot']['state'], 1)
            elif case == 'mutex-held':
                model.mutex_held = True
            elif case == 'usb-started':
                model.put32(model.flags['pmic_done'], 1)
                model.put32(model.shared + layout.layouts['a733_native_usb']['usb_step'], 1)
            elif case == 'pmic-deadline':
                model.time = (60000000 - 10000) * 24
            model.call(0, 'os_cpu_return_supervise' if case in ('usb-started', 'pmic-deadline') else 'os_cpu_return_pmic')
        assert model.stop == 'failure', (case, model.events)
        assert not model.u32(model.flags['ready'])
        assert not any(event[0] == 'notify' for event in model.events)
        rows.append({'name': case, 'passed': True, 'events': model.events})
    save(args.out / 'results.json', {'physical_access': False, 'cases': rows})
    print('PASS linked CPU return service close:', len(rows), 'cases')


if __name__ == '__main__':
    main()
