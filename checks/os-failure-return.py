#!/usr/bin/env python3
"""Execute the linked OS failure return in each failure context with modeled peripherals.

An application or OS failure ends the run with a normal return to the boot ROM
and the first-failure record, instead of a watchdog reset. The real code runs
from the supplied ELF: the failure sites, the breadcrumb SVC, the claim of the
return, the result record, the supervisor's escalation, the secondary-core wait
before the scheduler and the EL1/EL3 terminal with its cleanup and restoration.
The fixture registers the image's owner descriptors through the native API.
It supplies each owner's completion reply only after the supervisor requests
that owner, plus task notifications and scheduler state words. Registration,
callbacks, class ordering, fault retention and terminals run from the ELF.
No FreeRTOS task switch happens in this model.

The model enters the vectors directly with the exception state a core would
hold (ESR_EL1, FAR_EL1, ELR_EL1, SPSR_EL1, ICC_PMR_EL1); it does not generate
the exception, the translation faults or the MMU permissions.

Kernel entry points that this model stubs: xTaskGetSchedulerState, xTaskGenericNotify
and vTaskSuspend.

The context cases:
  startup      ss_fail on core 0 before the scheduler; cores 1-7 wait in the
               start sequence (a733_cached_el1) and join the terminal.
  running      board_fail_at after the scheduler started: the claim, the
               record, and then the supervisor driving the stop sequence until
               it notifies all eight terminals, which return to the ROM.
  running-repeat, running-device, escalation, device-first
               Later application failures leave the first record; a device
               failure through an unconverted board_fail_at caller and an
               unfinished return end at the watchdog; already-closed admission
               keeps the watchdog ending.
  registered-pending, registered-faults, registered-fault-behind-pending
               Requested owners wait without releasing later classes. Owner
               faults leave admission open, preserve separate RAM records and
               the retained secondary RTC record, and allow later cleanup.
               Unproved retirement withholds terminals until the 3 s bound.
  panic, panic-masked, unexpected-interrupt
               Software-detected faults: a core that can still schedule
               returns; masked or in a critical section it keeps the watchdog.
  el0-data-abort, el0-undefined, el0-instruction-abort, el0-secondary
               A synchronous exception from an application task at EL0 on a core
               that can still schedule enters the EL1 vector (lower-EL slot),
               which abandons the faulting context, claims a reserved stack and
               returns to EL0 in os_cpu_return_el0_fault. That runs the
               ss_fail claim (record group application, detail 0xe00 plus the
               ESR class, value the faulting address, caller ELR), suspends the
               task, and the supervisor and terminals return to the ROM.
  el1-exception, el0-masked, el0-pmr, el0-other-class, el0-device-first,
  el0-no-stack
               Negative controls that must keep the watchdog ending: an
               exception from EL1; an EL0 exception with interrupts or the
               priority mask closed; an EL0 exception of an unlisted class; one
               after a device failure closed admission; one past the last
               reserved stack.
--baseline-elf runs the startup and running failures on an image without the
failure return (today's code); they must end at the watchdog, so this check
fails there for the cases that expect a ROM return.
"""
import argparse
import hashlib
import sys
import importlib.util
import json
import struct
from pathlib import Path
from elftools.elf.elffile import ELFFile
import unicorn
from unicorn.arm64_const import UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_LR
from harness import Image as Layouts, save

ROOT = Path(__file__).resolve().parents[1]
# The case groups. The gate runs them as five steps side by side, balanced by their measured host time on 2026-11-13 (one run of
# every group took 342 s: el0 91 s, negative 68 s, registered 35 s, startup 31 s, the rest 2 to 22 s each); the five runs cover
# every group exactly once, which the assertion below enforces.
CASE_GROUPS = ('npu-report', 'startup', 'running', 'registered', 'repeat-device', 'escalation', 'device-first', 'panic',
               'unexpected-interrupt', 'el0', 'el0-second', 'negative', 'baseline')
GATE_RUNS = [['{elf:ret-npu-load}', '--out', '{output}', '--case', 'el0'],
             ['{elf:ret-npu-load}', '--out', '{output}', '--case', 'negative'],
             ['{elf:ret-npu-load}', '--out', '{output}', '--case', 'registered', '--case', 'startup'],
             ['{elf:ret-npu-load}', '--out', '{output}', '--case', 'npu-report', '--case', 'panic', '--case', 'running',
              '--case', 'baseline'],
             ['{elf:ret-npu-load}', '--out', '{output}', '--case', 'repeat-device', '--case', 'escalation', '--case', 'device-first',
              '--case', 'unexpected-interrupt', '--case', 'el0-second']]
_GATE_GROUPS = [run[i + 1] for run in GATE_RUNS for i, word in enumerate(run) if word == '--case']
assert sorted(_GATE_GROUPS) == sorted(CASE_GROUPS), 'the gate runs must cover every case group exactly once'
sys.path.insert(0, str(ROOT / 'tools'))
import board_images


def load(name, relative):
    spec = importlib.util.spec_from_file_location(name, ROOT / relative)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


terminal = load('failure_return_terminal_model', 'checks/os-cpu-return.py')
prior = terminal.prior
records = load('failure_return_records', 'apps/os/host.py')
crumbs = load('failure_return_crumbs', 'tools/a733-crumbs.py')
defines, _ = crumbs.header()
RTC = 0x07090000
GICD = 0x03400000
APPLICATION = defines['FAIL_APPLICATION']
PMIC = defines['FAIL_PMIC']
RETURN = 0x80000000
GRANTS = {name: 1 << int(bit) for name, bit, _ in records.manifest_tool.os_service_rows('OS_GRANT_ROWS')}


class Failure(terminal.Model):
    """The terminal model with the FreeRTOS entry points the failure paths reach stubbed."""

    def __init__(self, image, names):
        super().__init__(image)
        self.names = names
        self.notified = []
        self.suspended = set()
        self.panics = []
        self.quiesce_callbacks = {}
        self.callback_calls = []
        self.scheduler_state = 1   # taskSCHEDULER_NOT_STARTED; the kernel's own state words are not modeled
        self.el0_watch = False
        self.el0_entry = names.get('os_cpu_return_el0_fault')
        self.el0_seen = None
        self.stubs = {names[n]: n for n in ('xTaskGenericNotify', 'vTaskSuspend', 'xTaskGetSchedulerState') if n in names}

    def code(self, uc, a, size, unused):
        stub = self.stubs.get(a)
        if a in self.quiesce_callbacks:
            self.callback_calls.append(self.quiesce_callbacks[a])
        if self.el0_watch and a == self.el0_entry:
            # The vector returned to EL0 in the failure function: capture what it was given.
            self.el0_watch = False
            self.el0_seen = dict(core=self.core, args=[self.gpr('x%d' % i) for i in range(3)], sp_el0=self.get('sp_el0'),
                                 spsr=self.get('spsr_el1'), tpidr=self.get('tpidr_el1'))
            self.stop = 'el0'
            uc.emu_stop()
            return
        if stub == 'xTaskGenericNotify':
            self.notified.append((self.core, self.gpr('x0')))
            uc.reg_write(prior.fault.REGS[0], 1)
            uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_LR))
            return
        if stub == 'xTaskGetSchedulerState':
            uc.reg_write(prior.fault.REGS[0], self.scheduler_state)
            uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_LR))
            return
        if stub == 'vTaskSuspend':
            self.suspended.add(self.core)
            self.stop = 'park'
            self.parked.add(self.core)
            uc.emu_stop()
            return
        if a == self.names.get('platform_panic'):
            self.panics.append((self.core, self.gpr('x1')))
        super().code(uc, a, size, unused)


def symbols(path):
    """Every symbol by name, with its addresses; static objects included."""
    with Path(path).open('rb') as stream:
        table = {}
        for s in ELFFile(stream).get_section_by_name('.symtab').iter_symbols():
            if s.name:
                table.setdefault(s.name, []).append(int(s['st_value']))
    return table


class Scenario:
    def __init__(self, elf):
        self.elf = elf
        self.image = prior.fault.Image(elf)
        self.syms = symbols(elf)
        self.layouts = Layouts(elf, {'board_application', 'a733_board_session', 'a733_cached_runtime', 'os_quiesce'}).layouts
        self.registry = {}
        self.model = Failure(self.image, {**{k: v[0] for k, v in self.syms.items()}, **self.image.symbols})
        self.contexts = self.model.initialize()
        m = self.model
        self.board = self.image.symbols['board']
        self.has_return = 'os_cpu_return_failure' in self.syms
        # Nothing has been reported ready: the failure return must produce it.
        m.put32(self.local('ready'), 0)
        for c in range(8):
            m.put64(self.local('arrived') + c * 8, 0)
            base = self.image.symbols['a733_cached_runtime'] + c * 128
            m.put64(base + self.layouts['a733_cached_runtime']['epoch'], prior.EPOCH)
            m.put64(base + self.layouts['a733_cached_runtime']['startup_deadline'], prior.DEADLINE)
            m.put64(base + self.layouts['a733_cached_runtime']['diagnostic_deadline'], prior.DEADLINE)
        m.put64(m.report + 32, m.time + 10 ** 9)
        # A sealed session far from its deadline, as after admission.
        s = self.board + self.layouts['board_application']['session']
        m.put64(s + self.layouts['a733_board_session']['epoch'], prior.EPOCH)
        m.put64(s + self.layouts['a733_board_session']['generation'], 7)
        m.put64(s + self.layouts['a733_board_session']['deadline_us'], 10 ** 9)
        m.put64(self.board + self.layouts['board_application']['live_until'], 10 ** 9)
        # Capacity of the result record, as the manifest sets it.
        self.result = self.local('ss_result_record')
        self.capacity = struct.unpack('<Q', m.uc.mem_read(self.local('ss_result_capacity'), 8))[0]

    def local(self, name):
        found = self.syms.get(name, [])
        assert len(found) == 1, (name, found)
        return found[0]

    def flag(self, name):
        return self.model.u32(self.local(name))

    def fatal(self):
        return self.model.u32(self.board + self.layouts['board_application']['fatal'])

    def register_owners(self):
        """Register the image's descriptors through the native API before failure.

        Device/clock completion is supplied as owner evidence, as in the
        terminal fixture. The list links and the ordering are native code.
        """
        if self.registry or 'os_quiesce_register' not in self.syms:
            return
        manifest = records.image_manifest(self.elf)['manifest']
        names = ['thermal_quiesce', 'pmic_quiesce', 'terminal_quiesce']
        grants = 0
        if manifest and manifest.get('clock_use', 0) & 1:
            names.insert(0, 'clock_quiesce')
            grants |= GRANTS['CPU_CLOCK']
        if 'ss_application_npu' in self.syms:
            names.insert(0, 'npu_quiesce')
            grants |= GRANTS['NPU']
        # A sealed admitted grant cache, supplied by the session boundary.
        self.model.put32(self.local('granted_word'), 0x80000000 | grants)
        for name in names:
            address = self.local(name)
            self.start(0, self.local('os_quiesce_register'), (address,))
            assert self.run_one(0) == 'return' and self.model.gpr('x0') == 1, name
            self.registry[name] = address
            step = self.model.u64(address + self.layouts['os_quiesce']['step'])
            self.model.quiesce_callbacks[step] = name
        owner_head = Layouts(self.elf, layout_names=()).address('owners', '/apps/os/quiesce.c')
        assert self.model.u64(owner_head) == self.registry[names[-1]]

    def complete_owner(self, name):
        if name not in self.registry:
            return
        # Supply the owner's publication as well as its registry result. The
        # completed PMIC callback observes pmic_done before notifying its task.
        if name == 'pmic_quiesce':
            self.model.put32(self.local('pmic_done'), 1)
        self.start(0, self.local('os_quiesce_complete'), (self.registry[name],))
        assert self.run_one(0) == 'return'

    def requested(self, name):
        return self.model.u32(self.registry[name] + self.layouts['os_quiesce']['requested'])

    def fault_owner(self, name, site, value):
        self.start(3, self.local('os_quiesce_fault'), (self.registry[name], site, value, 0x41070200))
        assert self.run_one(3) == 'return' and self.model.gpr('x0') == 1

    def owner_record(self, name):
        q, fields = self.registry[name], self.layouts['os_quiesce']
        return {field: self.model.u32(q + fields[field]) for field in ('result', 'fault_state', 'site', 'value', 'visited')}

    def scheduler(self, running=True, core=6):
        m = self.model
        if running:
            self.register_owners()
        m.scheduler_state = 2 if running else 1
        m.put32(self.local('ucPrimaryCoreInitDoneFlag'), 1 if running else 0)

    def start(self, core, pc, args=()):
        m = self.model
        m.core = core
        m.uc.context_restore(self.contexts[core])
        for i, value in enumerate(args):
            m.uc.reg_write(prior.fault.REGS[i], value)
        m.uc.reg_write(UC_ARM64_REG_LR, prior.fault.SENTINEL)
        m.uc.reg_write(UC_ARM64_REG_PC, pc)
        self.contexts[core] = m.uc.context_save()

    def run_one(self, core, budget=2000000):
        """Run one core until it returns to the sentinel, parks or leaves the model."""
        m = self.model
        m.core = core
        m.stop = None
        m.uc.context_restore(self.contexts[core])
        executed = 0
        while executed < budget:
            m.uc.emu_start(m.uc.reg_read(UC_ARM64_REG_PC), 0, count=256)
            executed += 256
            m.apply_side_effects()
            if m.stop:
                break
        else:
            raise AssertionError(('core did not stop', core, hex(m.uc.reg_read(UC_ARM64_REG_PC))))
        outcome = m.stop
        self.contexts[core] = m.uc.context_save()
        return outcome

    def rtc(self):
        m = self.model
        return {name: m.u32(RTC + offset) for name, offset in
                (('last', 0x104), ('fault', 0x110), ('esr', 0x114), ('elr', 0x118), ('far', 0x11c))}

    def record(self):
        raw = bytes(self.model.uc.mem_read(self.result, 56 + self.capacity))
        return records.decode_failure(raw, prior.EPOCH, 7, self.capacity)

    def terminals(self, budget=4000000):
        """Run the eight per-core terminals from the point the tasks reach."""
        # The supervisor and the failing task suspended; every core now runs its terminal task.
        self.model.parked.clear()
        for c in range(8):
            self.start(c, self.image.symbols['a733_qualification_worker_done'], (c,))
        return self.model.execute(self.contexts, budget=budget)


def watchdog_ending(scenario, core, outcome):
    """The core reached the EL3 fatal path, whose stop leaves the watchdog to reset the board."""
    pc = scenario.model.uc.reg_read(UC_ARM64_REG_PC) if outcome == 'park' else None
    handler = scenario.image.symbols['a733_cached_el3_fault']
    return outcome == 'native-retained' or (pc is not None and handler <= pc < handler + 0x400)


def board_failure(scenario):
    rtc = scenario.rtc()
    assert rtc['fault'] >> 24 == 0xfa and (rtc['fault'] >> 16) & 15 == defines['FAIL_LEVEL'], hex(rtc['fault'])
    return crumbs.board_failure(dict(fault=rtc['fault'], esr=rtc['esr'], elr=rtc['elr'], far=rtc['far']), defines, None)


def returned_to_rom(scenario):
    """The return reached the ROM entry with the watchdog disarmed and recorded."""
    rtc = scenario.rtc()
    assert rtc['last'] >> 16 == 0x0503 and rtc['last'] & 0xffff == 0, hex(rtc['last'])


def never_ran_npu(s):
    """An NPU image whose service never ran: undo the terminal fixture's finished, active and stopped service state."""
    m = s.model
    npu = s.local('npu')
    layout = Layouts(s.elf, {'os_npu_application', 'npu_service_signals'})
    nf = layout.layouts['os_npu_application']
    signals = npu + nf['signals']
    for address in (npu + nf['stopped'], npu + nf['active'], signals + layout.layouts['npu_service_signals']['irq_stopped'],
                    signals + layout.layouts['npu_service_signals']['runtime_parked']):
        m.put32(address, 0)
    report = s.local('os_npu_published_report')
    m.uc.mem_write(report, bytes(104))


def case_startup(elf, npu_never_ran=False):
    s = Scenario(elf)
    m = s.model
    s.scheduler(False)
    if npu_never_ran:
        never_ran_npu(s)
    for name in ('probe_arrived', 'probe_finished'):
        for c in range(8):
            m.put32(s.local(name) + 4 * c, 1)
    m.put32(s.local('probe_release'), 1)
    m.put32(s.local('probe_count'), 256)
    s.start(0, s.image.symbols['ss_fail'], (0xb01,))
    for c in range(1, 8):
        s.start(c, s.image.symbols['a733_cached_el1'],
                (c, prior.EPOCH, prior.DEADLINE, prior.DEADLINE, 0x03460000 + c * 0x20000))
    try:
        outcome = m.execute(s.contexts, budget=4000000)
    except AssertionError as error:
        # Cores that never reach a terminal spin until the watchdog resets the board.
        assert error.args[0][0] == 'execution budget', error
        outcome = 'no-return'
    return s, outcome


def case_startup_refusal(elf, group, line, value, caller):
    """A start hook refuses: board_fail_note names it, then main panics (line 11)
    on core 0 before the scheduler. The panic must claim the failure return."""
    s = Scenario(elf)
    m = s.model
    s.scheduler(False)
    for name in ('probe_arrived', 'probe_finished'):
        for c in range(8):
            m.put32(s.local(name) + 4 * c, 1)
    m.put32(s.local('probe_release'), 1)
    m.put32(s.local('probe_count'), 256)
    site = group << 12 | line
    s.start(0, s.image.symbols['board_fail_note'], (site, value, caller))
    assert s.run_one(0) == 'return', 'the refusal note returns to board main'
    file = s.local('ss_result_capacity')   # any readable address names the file
    s.start(0, s.image.symbols['platform_panic'], (file, 11))
    for c in range(1, 8):
        s.start(c, s.image.symbols['a733_cached_el1'],
                (c, prior.EPOCH, prior.DEADLINE, prior.DEADLINE, 0x03460000 + c * 0x20000))
    outcome = m.execute(s.contexts, budget=4000000)
    return s, outcome


def case_running_claim(elf, site=None, group=APPLICATION, value=0x4641, preset_fatal=False):
    s = Scenario(elf)
    s.scheduler(True)
    m = s.model
    if preset_fatal:
        m.put32(s.board + s.layouts['board_application']['fatal'], 1)
    site = site if site is not None else RETURN | group << 12 | 0xb7f
    s.start(6, s.image.symbols['board_fail_at'], (site, value, 0x41070000))
    outcome = s.run_one(6)
    return s, outcome, site


def drive_supervisor(s, rounds=8, complete=True):
    """Run registered callbacks, supplying only each requested owner's reply."""
    m = s.model
    handles = s.syms.get('terminal_handle')
    for c in range(8):
        address = handles[0] + c * 8 if handles else s.local('terminal_handle.%d' % c)
        m.put64(address, 0x41f00000 + c)
    for _ in range(rounds):
        m.notified.clear()
        s.start(0, s.image.symbols['os_cpu_return_supervise'])
        outcome = s.run_one(0)
        if s.flag('ready') or outcome not in ('return', 'park'):
            break
        if complete:
            for name in ('clock_quiesce', 'thermal_quiesce', 'pmic_quiesce'):
                if name in s.registry and s.requested(name):
                    s.complete_owner(name)
    return outcome


def registered_cases(elf):
    rows = []
    s, outcome, _ = case_running_claim(elf)
    assert outcome == 'return' and s.fatal() == 0
    waiting = 'clock_quiesce' if 'clock_quiesce' in s.registry else 'thermal_quiesce'
    assert drive_supervisor(s, rounds=1, complete=False) == 'return'
    assert s.requested(waiting) and not s.requested('pmic_quiesce') and not s.flag('ready')
    s.model.time += 24 * 100000
    assert drive_supervisor(s) == 'park' and s.flag('ready') == 1
    assert s.terminals() == 'rom'
    rows.append(dict(name='registered-pending', outcome='rom', owner=waiting, wait_us=100000))

    s, outcome, _ = case_running_claim(elf)
    first = s.record()[1]
    owner = 'clock_quiesce' if 'clock_quiesce' in s.registry else 'thermal_quiesce'
    other = 'pmic_quiesce'
    site = RETURN | defines['FAIL_CPU_CLOCK' if owner == 'clock_quiesce' else 'FAIL_THERMAL'] << 12 | 0x731
    second_site = RETURN | defines['FAIL_PMIC'] << 12 | 0x732
    s.fault_owner(owner, site, 0x11)
    # Publish the first secondary before another owner faults. The registry
    # reports asynchronous peers when it next visits the supervisor.
    assert drive_supervisor(s, rounds=1, complete=False) == 'return'
    s.fault_owner(other, second_site, 0x22)
    assert s.fatal() == 0 and s.record()[1] == first
    # A late DONE publication must not erase an interrupt or owner fault.
    s.complete_owner(owner)
    assert s.owner_record(owner)['result'] == 2
    assert drive_supervisor(s, complete=False) == 'return' and not s.flag('ready')
    assert all(s.requested(n) for n in (owner, other, 'pmic_quiesce'))
    assert 'pmic_quiesce' in s.model.callback_calls, 'a prior owner fault must not hide later cleanup'
    for n, expected_site, value in ((owner, site, 0x11), (other, second_site, 0x22)):
        record = s.owner_record(n)
        assert record == dict(result=2, fault_state=2, site=expected_site, value=value, visited=1), (n, record)
    rtc = s.rtc()
    retained_site = quiesce_site(s, owner, site)
    assert (rtc['fault'] >> 16) & 15 == defines['FAIL_LEVEL_SECOND'] and rtc['esr'] == retained_site, rtc
    assert s.record()[1] == first and not s.fatal()
    s.model.time += 24 * 3100000
    s.start(0, s.image.symbols['os_cpu_return_supervise'])
    assert watchdog_ending(s, 0, s.run_one(0)) and not s.flag('ready')
    rows.append(dict(name='registered-faults', outcome='watchdog', first_secondary=retained_site,
                     owner_records={name: s.owner_record(name) for name in (owner, other)}))
    if 'clock_quiesce' in s.registry:
        s, _, _ = case_running_claim(elf)
        site = RETURN | defines['FAIL_THERMAL'] << 12 | 0x733
        s.fault_owner('thermal_quiesce', site, 0x33)
        assert drive_supervisor(s, rounds=1, complete=False) == 'return'
        assert s.requested('clock_quiesce') and not s.requested('pmic_quiesce')
        assert s.owner_record('thermal_quiesce')['visited'] == 1
        assert s.rtc()['esr'] == quiesce_site(s, 'thermal_quiesce', site)
        assert not s.fatal() and not s.flag('ready')
        s.model.time += 24 * 3100000
        s.start(0, s.image.symbols['os_cpu_return_supervise'])
        assert watchdog_ending(s, 0, s.run_one(0)) and not s.flag('ready')
        rows.append(dict(name='registered-fault-behind-pending', outcome='watchdog', owner='thermal'))
    return rows


def quiesce_site(s, owner, original):
    """Retained owner identity from the declared crumb format and linked enum."""
    identifier = s.model.u32(s.registry[owner] + s.layouts['os_quiesce']['owner'])
    return (original & 0x80000fff) | defines['FAIL_QUIESCE'] << 12 | identifier << 16 | ((original >> 12) & 15) << 24


VECTOR_LOWER_A64_SYNC = 8   # slot in the EL1 vector table: lower EL using AArch64, synchronous
VECTOR_CURRENT_SPX_SYNC = 4


def esr_value(klass, iss=0):
    return klass << 26 | 1 << 25 | iss


def raise_exception(s, core, slot, klass, iss=0, far=0, elr=0x41234560, spsr=0, pmr=0xff):
    """Enter the EL1 vector for a synchronous exception, with the state a core holds on entry.

    Runs until the vector returns to EL0 in the failure function (outcome 'el0'), or ends at
    another outcome (a park in the EL3 stop, a return)."""
    m = s.model
    m.core = core
    m.set('esr_el1', esr_value(klass, iss))
    m.set('far_el1', far)
    m.set('elr_el1', elr)
    m.set('spsr_el1', spsr)
    m.set('icc_pmr_el1', pmr)
    m.reg[core]['daif'] = 0x3c0   # the exception masks DAIF
    m.el0_watch = True
    m.el0_seen = None
    s.start(core, s.image.symbols['a733_cached_el1_vectors'] + slot * 128)
    m.uc.context_restore(s.contexts[core])
    m.uc.reg_write(prior.fault.REGS[16], 0x1234567890abcdef)   # the faulting task's x16, kept in TPIDR_EL1
    s.contexts[core] = m.uc.context_save()
    return s.run_one(core)


def finish_el0_fault(s, core):
    """Continue the failure function at EL0 on the stack that the vector gave the task."""
    m = s.model
    m.el0_watch = False
    m.uc.context_restore(s.contexts[core])
    m.uc.reg_write(UC_ARM64_REG_SP, m.el0_seen['sp_el0'])
    s.contexts[core] = m.uc.context_save()
    return s.run_one(core)


def check_el0_entry(s, core, klass, iss, far, elr):
    seen = s.model.el0_seen
    stacks = s.local('os_cpu_return_el0_stacks')
    assert seen and seen['core'] == core, seen
    assert seen['args'] == [esr_value(klass, iss), far, elr], [hex(a) for a in seen['args']]
    assert seen['spsr'] == 0, 'the task returns to EL0t with interrupts open'
    assert seen['tpidr'] == core, 'TPIDR_EL1 holds the core index again'
    assert stacks < seen['sp_el0'] <= stacks + 8 * 2048 and (seen['sp_el0'] - stacks) % 2048 == 0, hex(seen['sp_el0'])
    return (seen['sp_el0'] - stacks) // 2048 - 1


def case_el0_return(elf, name, core, klass, iss, far, elr, expect_value, pmr=0xff):
    """An EL0 exception ends the run at the ROM, with the application record."""
    s = Scenario(elf)
    s.scheduler(True)
    m = s.model
    assert raise_exception(s, core, VECTOR_LOWER_A64_SYNC, klass, iss, far, elr, pmr=pmr) == 'el0'
    slot = check_el0_entry(s, core, klass, iss, far, elr)
    assert slot == 0 and s.flag('os_cpu_return_el0_slots') == 1
    assert finish_el0_fault(s, core) == 'park' and core in m.suspended, 'the faulting task suspends itself'
    assert s.flag('closing') == s.flag('failing') == 1 and not s.flag('ready') and not s.flag('prefail')
    assert s.fatal() == 0, 'admission stays open for the stop paths'
    site = RETURN | (iss & 0x7fff) << 16 | APPLICATION << 12 | 0xe00 | klass
    record, payload = s.record()
    assert record['context'] == 'fault' and record['site'] == site and record['value'] == expect_value and \
        record['caller'] == elr and record['core'] == core, record
    rtc = s.rtc()
    assert rtc['esr'] == site and rtc['elr'] == elr and rtc['far'] == expect_value, {k: hex(v) for k, v in rtc.items()}
    failure = board_failure(s)
    assert failure['group'] == 'application' and failure['site'] == 0xe00 | klass, failure
    assert drive_supervisor(s) == 'park' and s.flag('ready') == 1
    assert sorted(c for c, _ in m.notified) == [0] * 8, m.notified
    outcome = s.terminals()
    assert outcome == 'rom', outcome
    returned_to_rom(s)
    assert s.record()[0] == record and not m.postclean
    return s, dict(name=name, outcome=outcome, record=record, breadcrumb=failure, instructions=m.count)


def case_el0_stop(elf, name, slot=VECTOR_LOWER_A64_SYNC, core=6, klass=0x24, iss=0x46, spsr=0, pmr=0xff,
                  preset_fatal=False, used_slots=0):
    """An exception that must keep the watchdog ending: the core reaches the EL3 stop, no claim."""
    s = Scenario(elf)
    s.scheduler(True)
    m = s.model
    if preset_fatal:
        m.put32(s.board + s.layouts['board_application']['fatal'], 1)
    if used_slots:
        m.put32(s.local('os_cpu_return_el0_slots'), used_slots)
    outcome = raise_exception(s, core, slot, klass, iss, 0x7a000000, 0x41234560, spsr, pmr)
    if preset_fatal:
        # The vector accepts the fault; the claim is refused, admission stays closed, and the supervisor stops.
        assert outcome == 'el0' and finish_el0_fault(s, core) == 'park'
        assert s.fatal() == 1 and not s.flag('closing') and not s.flag('failing')
    else:
        assert watchdog_ending(s, core, outcome), outcome
        assert not s.flag('closing') and not s.flag('failing') and m.el0_seen is None
        assert s.flag('os_cpu_return_el0_slots') == used_slots
    return s, dict(name=name, outcome='watchdog')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('elf', type=Path)
    parser.add_argument('--baseline-elf', type=Path, help='an image without the failure return, for the negative control')
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--case', action='append', choices=CASE_GROUPS,
                        help='run only this group; repeat to resume affected or unexecuted groups')
    args = parser.parse_args()
    selected = lambda name: not args.case or name in args.case
    args.out.mkdir(parents=True, exist_ok=False)
    args.elf_symbols = {n: next(iter(v - {0})) for n, v in Layouts(args.elf, layout_names=()).symbols.items() if n in ('os_npu_start', 'os_application_prepare')}
    rows = []

    # The runner admits a failure return only for an image that links it.
    assert 'os_cpu_return_failure' in board_images.symbols(args.elf)
    if args.baseline_elf:
        assert 'os_cpu_return_failure' not in board_images.symbols(args.baseline_elf)

    if selected('npu-report'):
        # An NPU image that fails before its service ran publishes the never-started report.
        if 'os_npu_published_report' in symbols(args.elf):
            s, outcome = case_startup(args.elf, npu_never_ran=True)
            assert outcome == 'rom', outcome
            returned_to_rom(s)
            raw = bytes(s.model.uc.mem_read(s.local('os_npu_published_report'), 104))
            words = struct.unpack_from('<4I2Q10I', raw)
            # magic, version, bytes, finished=2 (never started), the failure record's epoch and generation,
            # then stopped=1 and nothing else.
            assert words[:4] == (0x5255504e, 1, 104, 2) and words[4] == prior.EPOCH and words[5] == 7, words
            assert words[6:] == (0, 0, 0, 0, 0, 0, 1, 0, 0, 0) and not any(raw[72:]), words
            rows.append(dict(name='startup-npu-never-ran', outcome=outcome, report=list(words)))
            # A service that already ran (the fixture's finished report) is never replaced.
            s, outcome = case_startup(args.elf)
            assert outcome == 'rom' and s.model.u32(s.local('os_npu_published_report') + 12) == 1
            rows.append(dict(name='startup-npu-ran', outcome=outcome))

            if records.image_manifest(args.elf)['manifest'] is None:
                save(args.out / 'results.json', {'physical_access': False, 'cases': rows,
                     'image_sha256': hashlib.sha256(args.elf.read_bytes()).hexdigest(),
                     'model_boundary': 'An image without an application manifest holds no result record; only its NPU stop report is checked.'})
                print('PASS linked OS failure return (NPU report):', len(rows), 'cases')
                return

    if selected('startup'):
        # Before the scheduler: ROM return, record, all eight cores in the terminal.
        s, outcome = case_startup(args.elf)
        assert outcome == 'rom', outcome
        returned_to_rom(s)
        failure = board_failure(s)
        assert failure['group'] == 'application' and failure['site'] == 0xb01 and failure['value'] == hex(0xb01), failure
        record, payload = s.record()
        assert record['context'] == 'startup' and record['site'] == (RETURN | APPLICATION << 12 | 0xb01) and \
            record['value'] == 0xb01 and record['core'] == 0, record
        assert s.flag('closing') == s.flag('failing') == s.flag('prefail') == s.flag('ready') == 1
        assert s.fatal() == 0
        assert not s.model.postclean, s.model.postclean[:4]
        assert all(s.model.u64(s.model.report + 688 + c * 1024 + 24) == prior.EPOCH for c in range(8)), 'every core restored'
        rows.append(dict(name='startup', outcome=outcome, record=record, breadcrumb=failure, instructions=s.model.count))
        (args.out / 'startup-return-report.bin').write_bytes(bytes(s.model.uc.mem_read(s.model.report, 8880)))
        (args.out / 'startup-failure-record.bin').write_bytes(payload)

        # A refusing start hook: the note, then the board main panic, before the scheduler.
        npu_start = args.elf_symbols['os_npu_start']
        for name, group, line, value, caller in (
                ('startup-refusal-prepare', 1, 37, 0xffff, args.elf_symbols['os_application_prepare']),
                ('startup-refusal-npu', 10, 434, 0, npu_start)):
            s, outcome = case_startup_refusal(args.elf, group, line, value, caller)
            assert outcome == 'rom', (name, outcome)
            returned_to_rom(s)
            assert s.flag('closing') == s.flag('failing') == s.flag('prefail') == s.flag('ready') == 1, name
            assert s.fatal() == 0, 'no watchdog path'
            assert not s.model.postclean, s.model.postclean[:4]
            assert all(s.model.u64(s.model.report + 688 + c * 1024 + 24) == prior.EPOCH for c in range(8)), 'every core restored'
            record, payload = s.record()
            assert record['core'] == 0 and record['context'] in ('startup', 'fault'), record
            rows.append(dict(name=name, outcome=outcome, record=record, breadcrumb=board_failure(s)))

    if selected('running'):
        # After the scheduler: the claim, then the supervisor's stop sequence, then the terminals.
        s, outcome, site = case_running_claim(args.elf)
        assert outcome == 'return', outcome
        assert s.flag('closing') == s.flag('failing') == 1 and not s.flag('ready') and not s.flag('prefail')
        assert s.fatal() == 0, 'admission stays open for the stop paths'
        record, payload = s.record()
        assert record['context'] == 'running' and record['site'] == site and record['value'] == 0x4641 and \
            record['caller'] == 0x41070000, record
        assert drive_supervisor(s) == 'park'   # the supervisor suspends after notifying the terminals
        assert s.flag('ready') == 1 and sorted(c for c, _ in s.model.notified) == [0] * 8, s.model.notified
        assert len({h for _, h in s.model.notified}) == 8, 'one terminal task per core'
        outcome = s.terminals()
        assert outcome == 'rom', outcome
        returned_to_rom(s)
        failure = board_failure(s)
        assert failure['group'] == 'application' and failure['site'] == 0xb7f, failure
        assert s.record()[0] == record and not s.model.postclean
        rows.append(dict(name='running', outcome=outcome, record=record, breadcrumb=failure, instructions=s.model.count))

    if selected('registered'):
        rows.extend(registered_cases(args.elf))

    if selected('repeat-device'):
        # A later application failure leaves the first record and the open admission.
        s, outcome, site = case_running_claim(args.elf)
        first = s.record()[1]
        s.start(2, s.image.symbols['board_fail_at'], (RETURN | APPLICATION << 12 | 0xb01, 5, 0x41070100))
        assert s.run_one(2) == 'return' and s.fatal() == 0 and s.record()[1] == first
        rows.append(dict(name='running-repeat', outcome='first-record-kept'))

        # A device failure during the return is not recoverable: admission closes and the
        # supervisor ends at the watchdog path instead of notifying the terminals.
        s.start(3, s.image.symbols['board_fail_at'], (PMIC << 12 | 0x123, 9, 0x41070200))
        assert s.run_one(3) == 'return' and s.fatal() == 1
        assert watchdog_ending(s, 0, drive_supervisor(s, 2)) and not s.flag('ready') and not s.model.notified
        rows.append(dict(name='running-device', outcome='watchdog'))

    if selected('escalation'):
        # An unfinished return ends at the watchdog after the bounded interval.
        s, outcome, site = case_running_claim(args.elf)
        m = s.model
        m.time += 24 * 4_000_000   # four seconds pass with the stop sequence unfinished
        s.start(0, s.image.symbols['os_cpu_return_supervise'])
        assert watchdog_ending(s, 0, s.run_one(0)) and s.fatal() == 1 and not s.flag('ready')
        rows.append(dict(name='escalation', outcome='watchdog'))

    if selected('device-first'):
        # A device failure that already closed admission keeps the watchdog ending.
        s, outcome, site = case_running_claim(args.elf, preset_fatal=True)
        assert outcome == 'return' and not s.flag('closing') and not s.flag('failing') and s.fatal() == 1
        rows.append(dict(name='device-first', outcome='watchdog'))

    if selected('panic'):
        # Software-detected faults on a core that can still schedule return; others do not.
        for name, masked in (('panic', False), ('panic-masked', True)):
            s = Scenario(args.elf)
            s.scheduler(True)
            m = s.model
            m.reg[6]['daif'] = 0x80 if masked else 0
            m.reg[6]['icc_pmr_el1'] = 0xff
            file = s.local('ss_result_capacity')   # any readable address names the file
            s.start(6, s.image.symbols['platform_panic'], (file, 1234))
            outcome = s.run_one(6)
            if masked:
                assert watchdog_ending(s, 6, outcome) and not s.flag('closing'), outcome
            else:
                assert outcome == 'park' and s.flag('failing') == 1 and 6 not in s.model.suspended
                record, payload = s.record()
                assert record['context'] == 'fault' and record['site'] == RETURN | 0x1000 | 1234 and \
                    record['value'] == file, record
                failure = board_failure(s)
                assert failure['group'] == 'board' and failure['site'] == 1234, failure
            rows.append(dict(name=name, outcome=outcome))

    if selected('unexpected-interrupt'):
        s = Scenario(args.elf)
        s.scheduler(True)
        m = s.model
        m.put64(s.local('diagnostic_deadline'), prior.DEADLINE)
        s.start(6, s.image.symbols['vApplicationIRQHandler'], (300,))
        outcome = s.run_one(6)
        assert outcome == 'return' and s.flag('failing') == 1 and m.u32(GICD + 0x180 + 4 * 9) == 1 << (300 % 32)
        failure = board_failure(s)
        assert failure['group'] == 'board' and failure['site'] == 0x800 | 300 and failure['value'] == hex(300), failure
        rows.append(dict(name='unexpected-interrupt', outcome=outcome))

    if selected('el0'):
        # Exceptions from an application task at EL0: the task is abandoned, the board returns.
        ELR = 0x41070040
        for name, core, klass, iss, far, value in (
                ('el0-data-abort', 6, 0x24, 0x46, 0x7a000000, 0x7a000000),      # store to an unmapped address
                ('el0-instruction-abort', 6, 0x20, 0x06, 0x7a000100, 0x7a000100),
                ('el0-undefined', 2, 0x00, 0x00, 0xdead0000, 0),                # FAR is unknown for this class
                ('el0-alignment', 3, 0x24, 0x61, 0x41f00003, 0x41f00003),       # data abort, alignment fault status
                ('el0-pc-alignment', 5, 0x22, 0, 0x41070042, 0x41070042),
                ('el0-sp-alignment', 1, 0x26, 0, 0, 0),
                ('el0-secondary', 7, 0x24, 0x46, 0x7a000008, 0x7a000008)):
            # The controller reads the unmasked priority mask back as 0xf8 when it has five priority bits.
            s, row = case_el0_return(args.elf, name, core, klass, iss, far, ELR, value, pmr=0xf8 if core == 6 else 0xff)
            rows.append(row)
            if name == 'el0-data-abort':
                (args.out / 'el0-data-abort-failure-record.bin').write_bytes(s.record()[1])
    if selected('el0-second'):
        # A second EL0 fault on another core: its own stack, the first record kept.
        ELR = 0x41070040
        s, row = case_el0_return(args.elf, 'el0-first', 6, 0x24, 0x46, 0x7a000000, ELR, 0x7a000000)
        first = s.record()[1]
        assert raise_exception(s, 4, VECTOR_LOWER_A64_SYNC, 0x00, 0, 0, 0x41070100) == 'el0'
        assert check_el0_entry(s, 4, 0x00, 0, 0, 0x41070100) == 1, 'a second stack'
        assert finish_el0_fault(s, 4) == 'park' and 4 in s.model.suspended and s.fatal() == 0 and s.record()[1] == first
        rows.append(dict(name='el0-second-fault', outcome='first-record-kept'))

    if selected('negative'):
        # Negative controls: the watchdog ending is kept.
        for name, kwargs in (
                ('el1-exception', dict(slot=VECTOR_CURRENT_SPX_SYNC, spsr=5, klass=0x25)),    # data abort taken from EL1h
                ('el1-svc-class-fault', dict(slot=VECTOR_CURRENT_SPX_SYNC, spsr=5, klass=0x21)),
                ('el0-masked', dict(spsr=0x80)),                  # interrupts masked in the faulting task
                ('el0-pmr', dict(pmr=0x98)),                      # in a kernel critical section (API mask)
                ('el0-aarch32', dict(spsr=0x10)),
                ('el0-other-class', dict(klass=0x07)),            # an FP/SIMD access trap
                ('el0-no-stack', dict(used_slots=8))):
            s, row = case_el0_stop(args.elf, name, **kwargs)
            rows.append(row)
        # A device failure closed admission first: the exception still ends the task, the claim is refused.
        s, row = case_el0_stop(args.elf, 'el0-device-first', preset_fatal=True)
        rows.append(row)

    if selected('baseline'):
        if args.baseline_elf:
            # Today's code: the same failures close admission and start no return.
            base, outcome = case_startup(args.baseline_elf)
            assert outcome in ('no-return', 'retained') and base.rtc()['last'] >> 16 != 0x0503, outcome
            rows.append(dict(name='baseline-startup', outcome=outcome))
            base, outcome, site = case_running_claim(args.baseline_elf)
            assert base.fatal() == 1 and not base.flag('closing')
            rows.append(dict(name='baseline-running', outcome='admission closed; no return requested'))
            # Without the failure return an EL0 exception takes the EL3 stop.
            base = Scenario(args.baseline_elf)
            base.scheduler(True)
            assert watchdog_ending(base, 6, raise_exception(base, 6, VECTOR_LOWER_A64_SYNC, 0x24, 0x46, 0x7a000000))
            rows.append(dict(name='baseline-el0-exception', outcome='watchdog'))
    save(args.out / 'results.json', {'physical_access': False, 'cases': rows,
         'image_sha256': hashlib.sha256(args.elf.read_bytes()).hexdigest(),
         'baseline_sha256': hashlib.sha256(args.baseline_elf.read_bytes()).hexdigest() if args.baseline_elf else None,
         'checker_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
         'selected_groups': args.case,
         'model_boundary': 'Owner completion replies, task wake-ups and scheduler words are supplied; registration, callbacks, ordering and terminals run from the ELF.'})
    print('PASS linked OS failure return:', len(rows), 'cases')


if __name__ == '__main__':
    main()
