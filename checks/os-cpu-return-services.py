#!/usr/bin/env python3
"""Execute linked close coordination with modeled RTOS and completed CPU DVFS.

The thermal driver's STOPPED result and completed clock transition are API
fixtures. The actual thermal owner masks its SPI and publishes completion;
the registered owner callbacks, PMIC idle predicate and supervisor notification
order execute. RTOS wake-ups are modeled; this is not physical qualification.
In an image with the host-armed watchdog, each live supervisor step restarts
WDT0 and a step that fails its liveness check does not.
"""
import argparse
import hashlib
import importlib.util
from pathlib import Path
from unicorn.arm64_const import UC_ARM64_REG_PC, UC_ARM64_REG_LR, UC_ARM64_REG_SP
from harness import Image, save

ROOT = Path(__file__).resolve().parents[1]
GATE_RUNS = [['{elf:ret-gpu}', '--out', '{output}']]
spec = importlib.util.spec_from_file_location('return_services_prior', ROOT / 'checks/cpu-runtime-qualification.py')
prior = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prior)
spec = importlib.util.spec_from_file_location('return_services_terminal', ROOT / 'checks/os-cpu-return.py')
terminal = importlib.util.module_from_spec(spec)
spec.loader.exec_module(terminal)


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
        self.thermal_context = None
        self.thermal_running = False
        self.mutex_held = False
        self.hooks = {}
        for name in ('vTaskEnterCritical', 'vTaskExitCritical', 'os_cpu_clock_step'):
            self.hooks[layout.address(name)] = lambda: 1
        self.hooks[layout.address('xQueueGetMutexHolder')] = lambda: int(self.mutex_held)
        self.hooks[layout.address('xTaskGetCurrentTaskHandle')] = lambda: 0x3000
        self.hooks[layout.address('a733_thermal_stop')] = self.thermal_stop
        self.hooks[layout.address('xTaskGenericNotify')] = self.notify
        self.hooks[layout.address('xTaskCreateStaticAffinitySet')] = self.create
        self.hooks[layout.address('vTaskDelay')] = self.delay
        self.board = layout.address('board', 'os-image/main.c')
        self.shared = layout.address('providers') + layout.layouts['board_provider']['shared']
        self.pmic = self.shared + layout.layouts['a733_native_usb']['pmic']
        self.thermal = layout.address('thermal', 'apps/os/thermal.c')
        self.flags = {name: layout.address(name, 'cpu-return.c')
                      for name in ('closing', 'ready')}
        self.quiesce = {name: layout.address(name + '_quiesce') for name in ('clock', 'thermal', 'pmic', 'terminal')}
        self.grants = terminal.service_grants(image)
        if self.grants & terminal.GRANTS['NPU']:
            self.quiesce['npu'] = layout.address('npu_quiesce')
        self.put32(layout.address('granted_word', 'apps/os/grants.c'), 0x80000000 | self.grants)
        self.put32(layout.address('return_done', 'apps/os/cpu.c'), 1)
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
        self.stop = 'yield'
        self.uc.emu_stop()
        return 0

    def q(self, name, field):
        return self.u32(self.quiesce[name] + self.layout.layouts['os_quiesce'][field])

    def register(self):
        # Native constructors register the PMIC and terminals; the modeled
        # completed clock and constructed thermal owner register their actual
        # descriptors before any return request.
        assert self.call(0, 'os_cpu_return_start') == 1
        names = ['thermal']
        if self.grants & terminal.GRANTS['CPU_CLOCK']:
            names.append('clock')
        if self.grants & terminal.GRANTS['NPU']:
            names.append('npu')
        for name in names:
            assert self.call(0, 'os_quiesce_register', (self.quiesce[name],)) == 1

    def thermal_pass(self):
        # Resume the actual owner after its modeled scheduler delay. Its stack
        # stays separate from calls made by the supervisor and PMIC task.
        self.core = 0
        self.stop = None
        if self.thermal_context is None:
            self.uc.reg_write(UC_ARM64_REG_SP, 0x41f80000)
            self.uc.reg_write(UC_ARM64_REG_LR, prior.fault.SENTINEL)
            self.uc.reg_write(UC_ARM64_REG_PC, self.layout.address('os_thermal_task'))
            self.uc.reg_write(prior.fault.REGS[0], 0)
        else:
            self.uc.context_restore(self.thermal_context)
        for _ in range(200000 // 64):
            self.thermal_running = True
            self.uc.emu_start(self.uc.reg_read(UC_ARM64_REG_PC), 0, count=64)
            self.thermal_running = False
            self.apply_side_effects()
            if self.stop:
                self.thermal_context = self.uc.context_save()
                return self.stop
        raise AssertionError(('thermal owner did not yield', hex(self.uc.reg_read(UC_ARM64_REG_PC))))

    def close_thermal(self):
        assert self.thermal_pass() == 'yield'  # masks the SPI in one bounded pass
        assert self.thermal_pass() == 'suspended'
        assert self.q('thermal', 'result') == 1

    def thermal_stop(self):
        # SPI disable must precede converter/local-source shutdown.
        assert any(address == 0x03400188 and value == 1 << 7 for _, address, _, value in self.writes)
        assert self.q('thermal', 'result') == 0 and not self.q('pmic', 'requested')
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
             'a733_pmic', 'a733_pmic_slot', 'a733_platform_locks', 'os_thermal_service', 'os_quiesce'}
    layout = Image(args.elf, names)
    image = prior.fault.Image(args.elf)
    rows = []
    armed = 'os_cpu_return_watchdog' in image.symbols
    restarts = lambda m: sum(1 for _, a, _, v in m.writes if a == 0x0205000c and v == 0x14af)
    model = Model(image, layout)
    model.register()
    assert len(model.handles) == 8
    model.call(0, 'os_cpu_return_supervise')
    assert model.stop == 'return' and restarts(model) == armed
    model.put32(model.flags['closing'], 1)
    model.call(0, 'os_cpu_return_supervise')
    assert model.events == [('notify', 0x3000)] and not model.q('pmic', 'requested')
    assert restarts(model) == 2 * armed
    model.events.clear()
    model.close_thermal()
    assert model.events == [('thermal-stop', 7), ('suspend', 0)]
    model.events.clear()
    model.call(0, 'os_cpu_return_supervise')
    assert model.events == [('notify', 0x1000)] and not model.u32(model.flags['ready'])
    model.events.clear()
    model.call(0, 'os_cpu_return_pmic')
    assert model.stop == 'suspended' and model.q('pmic', 'result') == 1
    model.events.clear()
    model.call(0, 'os_cpu_return_supervise')
    assert model.events == [('notify', h) for h in model.handles[1:] + model.handles[:1]] + [('suspend', 0)]
    assert model.u32(model.flags['ready']) == 1 and restarts(model) == 4 * armed
    rows.append({'name': 'thermal-pmic-and-cpu0-last-order', 'passed': True, 'events': model.events,
                 'watchdog_restarts': restarts(model)})
    for case in ('pmic-retained', 'pmic-slot-owned', 'mutex-held', 'usb-started', 'thermal-stop-failed',
                 'pmic-deadline', 'ths-rwp-deadline', 'session-failed'):
        model = Model(image, layout)
        model.register()
        model.put32(model.flags['closing'], 1)
        model.call(0, 'os_cpu_return_supervise')
        model.events.clear()
        if case in ('thermal-stop-failed', 'ths-rwp-deadline'):
            if case == 'thermal-stop-failed':
                model.stop_result = 5
            else:
                model.put32(0x03400000, 1 << 31)
            assert model.thermal_pass() == 'yield'
            assert model.thermal_pass() == 'yield'
            assert not model.q('pmic', 'requested') and not model.u32(model.flags['ready'])
            if case == 'thermal-stop-failed':
                assert model.q('thermal', 'result') == 2
                assert not model.u32(model.board + layout.layouts['board_application']['fatal'])
            else:
                assert model.q('thermal', 'result') == 0
            # The supervisor owns the shared timeout even when an owner's
            # hardware operation remains pending or returned FAULT.
            model.time += 24 * 3100000
            model.call(0, 'os_cpu_return_supervise')
        else:
            model.close_thermal()
            model.call(0, 'os_cpu_return_supervise')
            model.events.clear()
            if case == 'pmic-retained':
                model.put32(model.pmic + layout.layouts['a733_pmic']['retained'], 1)
            elif case == 'pmic-slot-owned':
                slot = model.pmic + layout.layouts['a733_pmic']['slots']
                model.put32(slot + layout.layouts['a733_pmic_slot']['state'], 1)
            elif case == 'mutex-held':
                model.mutex_held = True
            elif case == 'usb-started':
                model.call(0, 'os_cpu_return_pmic')
                model.put32(model.shared + layout.layouts['a733_native_usb']['usb_step'], 1)
            elif case == 'pmic-deadline':
                model.time = (60000000 - 10000) * 24
            elif case == 'session-failed':
                model.put32(model.board + layout.layouts['board_application']['fatal'], 1)
            before = restarts(model)
            model.call(0, 'os_cpu_return_supervise' if case in ('usb-started', 'pmic-deadline', 'session-failed')
                       else 'os_cpu_return_pmic')
            if case in ('pmic-deadline', 'session-failed'):
                assert restarts(model) == before, 'no WDT0 restart once the session is not live'
            if case in ('pmic-retained', 'pmic-slot-owned', 'mutex-held'):
                assert model.stop == 'suspended' and model.q('pmic', 'result') == 2
                assert not model.u32(model.board + layout.layouts['board_application']['fatal'])
                model.call(0, 'os_cpu_return_supervise')
                assert not model.u32(model.flags['ready'])
                model.time += 24 * 3100000
                model.call(0, 'os_cpu_return_supervise')
        assert model.stop == 'failure', (case, model.events)
        assert not model.u32(model.flags['ready'])
        assert not any(event[0] == 'notify' and event[1] in model.handles for event in model.events)
        rows.append({'name': case, 'passed': True, 'events': model.events})
    save(args.out / 'results.json', {
        'physical_access': False,
        'image_sha256': hashlib.sha256(args.elf.read_bytes()).hexdigest(),
        'checker_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        'model_boundary': 'Clock completion, thermal driver result and RTOS wake-ups are inputs; '
                          'registration, owner callbacks, thermal stop passes, PMIC idle checks '
                          'and supervisor ordering run from the ELF.',
        'cases': rows,
    })
    print('PASS linked CPU return service close:', len(rows), 'cases')


if __name__ == '__main__':
    main()
