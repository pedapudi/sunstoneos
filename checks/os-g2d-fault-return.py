#!/usr/bin/env python3
"""Execute the linked return of a faulted G2D service (apps/os/g2d.c) through the production quiesce registry.

The image's own os_quiesce_register, os_quiesce_step and G2D quiesce step run under AArch64 emulation, with the G2D
owner task run between supervisor passes as its task would be. The CCU, PCK1 and G2D registers are modeled as
plain registers, and register access passes the image's own Device-mapping check. The distributor calls that mask
INTID 83 (gicv3_spi_mask, gicv3_spi_masked) report the line masked. Time advances 10 ms per
supervisor pass; the supervisor's shared bound is 3 s (OS_FAILURE_RETURN_US, apps/os/cpu-return.c), after which the
image stops at site 0x742 and the watchdog reset ends the session.

Cases, each starting from a service that failed during its open (phase FAULT):
- fault-after-engine-open: the fault came after the engine's bus clock and reset opened (step G2D_STEP_TOP). The
  owner runs the vendor-order close, which closes 0x20's update and interrupt bits, gates the module and bus clocks
  and asserts both resets; the registry reaches DONE within the bound with no fault note, and the fault record keeps
  the first failure.
- fault-before-bus-gate: the fault came before the bus clock opened (step G2D_STEP_BUS_GATE, G2D reset deasserted).
  The close touches no G2D register, and the registry reaches DONE.
- close-step-fails: the CCU bus-gate word does not hold its write. The close fails, the quiesce step reports FAULT
  with site 0x931 once, with value step << 24, and the registry never reaches DONE: the session fails at the bound.
- run-after-failed-close: as close-step-fails, then the application posts a run. The owner refuses it (result -1)
  with nothing recorded: the fault step and value, which ss_g2d_fault returns and the reported 0x931 note carries
  as step << 24 | value, stay those of the first fault. An image whose owner records a refused run on a FAULT service
  (bringup at 903da3dfa) rewrites them to G2D_STEP_RUN and the phase.
- quiesce-among-close-stores: as fault-after-engine-open, but the quiesce step that runs during the close is placed
  at every point among the close's stores to the words the step reads (phase, close_started, reported and quiesced),
  with a pause after each of its first three loads of them so that its loads can straddle one or more stores
  (harness.TwoCores). No placement may report 0x931; every one must reach DONE with the service CLOSED. An image whose
  quiesce step decides from phase and close_started (bringup at 566574ee3) fails this case.
Two cases run the owner task without a break at its deadline waits, with each task notification index's pending
count modeled: a give adds one to the owner's count at its index, a deadline wait returns at once (notified) when its
index has a count and otherwise at its deadline (expired), and takes its index's count on its way out, and the
owner's block with no timeout returns when index 0 has a count and otherwise holds the owner until a give. The input
arrives while the close masks INTID 83. Master 10 is attached, and the close's detach releases the IOMMU's clock
lease, which finds the clock registry busy once and waits 50 us in the owner's task through the deadline runtime
(clock_lease, apps/os/iommu.c), so the close pauses in a deadline wait with the input's notification pending:
- request-during-close: as fault-after-engine-open, with a run posted by the application while the quiesce's close
  runs (its post tested quiesce_wanted before the quiesce set it). The owner must answer the run (refused, result -1,
  the service being CLOSED). An owner whose deadline waits take index 0 consumes the post's notification in the
  pause, then blocks with the run posted and the application's run stranded (bringup at 5d3de101a).
- quiesce-during-close: the application's close of the FAULT service runs first, and the quiesce arrives during it.
  The owner answers the close and the registry reaches DONE. The owner tests quiesce_wanted after every request, and
  the supervisor's quiesce step notifies the owner again on every PENDING pass, so a quiesce notification taken by a
  deadline wait does not strand the quiesce; the case guards that order.
- hints-reach-deadline-waits: the power row's wake and INTID 83 (with nothing armed) each give the owner the index
  its deadline waits take, which is not index 0; an owner whose hints stayed on index 0 would sleep each operation's
  completion wait and each row wait to its deadline.
- deadline-waits-on-index-0: request-during-close with the owner's deadline index forced to 0 (the registration hook
  answers as for sunstone_deadline_register). The case must fail, which shows that request-during-close detects an
  owner whose deadline waits take its inputs' index.
An image built before the close of a faulted service (the stage-2 board run 1794711214889069759-0a9ae5d4, the
stage-1 image) fails the first two cases: its registry reports FAULT on every pass until the bound.

Usage: os-g2d-fault-return.py --image ELF --output DIR"""
import argparse
import hashlib
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn.arm64_const import (UC_ARM64_REG_PC, UC_ARM64_REG_LR, UC_ARM64_REG_SP, UC_ARM64_REG_X0,
                                 UC_ARM64_REG_X1, UC_ARM64_REG_X2)
from harness import machine as harness_machine, flat_members, Image, hook_addresses, save, skip_crumbs, TwoCores

GATE_RUNS = [['--image', '{elf:ret-g2d}', '--output', '{output}']]
UNIT = '/apps/os/g2d.c'
CCU, PCK1, G2D = 0x02002000, 0x07061000, 0x05440000
STACKS, SENTINEL, NOTE = 0x60000000, 0x60100000, 0x60100100
BOUND_US, PASS_US = 3_000_000, 10_000
QUIESCE = {'PENDING': 0, 'DONE': 1, 'FAULT': 2}
FAULT_PHASE, CLOSED_PHASE = 4, 0
SITE_G2D = 0x931
FAKE_TASK = 0x60200000                                                    # the owner's handle; gives to it are counted


class Registers:
    """Plain 32-bit registers per page, with optional words that keep a fixed value."""
    def __init__(self, base, words, stuck=None):
        self.base, self.words, self.stuck, self.writes, self.reads = base, dict(words), stuck or {}, [], []

    def read(self, uc, offset, size, data):
        self.reads.append(offset)
        return self.words.get(offset, 0)

    def write(self, uc, offset, size, value, data):
        self.writes.append((offset, value))
        self.words[offset] = self.stuck.get(offset, value)


def run_case(image, parts, layout, steps, name, fault_step, ccu_words, stuck=None, schedule=None, run_after=False,
             inject=None, mutant=False, hints=False):
    uc = harness_machine()
    low = min(a for a, _, _ in parts) & ~0xfff
    high = (max(a + n for a, _, n in parts) + 0xfff) & ~0xfff
    uc.mem_map(low, high - low)
    for address, data, _ in parts:
        uc.mem_write(address, data)
    uc.mem_map(STACKS, 0x100000)
    uc.mem_map(SENTINEL, 0x1000)
    uc.mem_write(NOTE, (0xd65f03c0).to_bytes(4, 'little'))             # ret
    skip_crumbs(uc, image)
    ccu = Registers(CCU, ccu_words, stuck)
    pck = Registers(PCK1, {0x8: 8})
    g2d_regs = Registers(G2D, {0x20: 0x50})
    for regs in (ccu, pck, g2d_regs):
        uc.mmio_map(regs.base, 0x1000, regs.read, None, regs.write, None)
    state = {'time': 1000, 'requested': 0, 'notes': [], 'slice_done': False, 'pending': [0, 0, 0],
             'deadline_index': None, 'blocked': False, 'waits': []}
    continuous = inject is not None
    g2d = image.address('g2d', UNIT)

    def put(address, value, n=4): uc.mem_write(address, int(value).to_bytes(n, 'little'))
    def get(address, n=4): return int.from_bytes(uc.mem_read(address, n), 'little')
    def field(name): return g2d + layout[name]

    board = image.address('board')
    put(board + image.layouts['board_application']['session'] + image.layouts['a733_board_session']['deadline_us'],
        60_000_000, 8)
    put(field('dev.phase'), FAULT_PHASE); put(field('dev.fault_step'), fault_step); put(field('dev.fault_value'), 0)
    put(image.address('owner', UNIT), FAKE_TASK, 8)

    def ret(m, value=None):
        if value is not None:
            m.reg_write(UC_ARM64_REG_X0, value & 0xffffffffffffffff)
        m.reg_write(UC_ARM64_REG_PC, m.reg_read(UC_ARM64_REG_LR))

    def now(m):
        state['time'] += 1
        ret(m, state['time'])

    def counter_now(m):
        state['time'] += 1
        put(m.reg_read(UC_ARM64_REG_X1), state['time'], 8)
        ret(m, 1)

    def translate(m):
        address, out = m.reg_read(UC_ARM64_REG_X0), m.reg_read(UC_ARM64_REG_X2)
        uc.mem_write(out, address.to_bytes(8, 'little') + bytes(8))      # physical, Device-nGnRnE, shareability 0
        ret(m, 1)

    def deadline_wait(until):
        # The deadline wait: notified at once by a count at its index, otherwise expired at its deadline; it takes its
        # index's count on its way out.
        index = state['deadline_index']
        notified = state['pending'][index] > 0
        if not notified:
            state['time'] = max(state['time'], until)
        state['waits'].append({'index': index, 'notified': notified, 'pending': list(state['pending'])})
        state['pending'][index] = 0
        return 0 if notified else 1

    def wait(m):
        if continuous:
            until = m.reg_read(UC_ARM64_REG_X0)
            if until - state['time'] > BOUND_US and not state['pending'][state['deadline_index']]:
                # An idle wait past the supervisor's bound (an owner that wakes on a timer between requests): the
                # owner sleeps through the case.
                state['blocked'] = True
                state['slice_done'] = True
                m.emu_stop()
                return
            ret(m, deadline_wait(until))
            return
        state['slice_done'] = True
        m.emu_stop()

    def detach(m):
        # The last detach's clock-lease release: the registry busy once, then a 50 us deadline wait, then released.
        deadline_wait(state['time'] + 50)
        ret(m, 1)

    def block(m):
        if not continuous:
            wait(m)
            return
        index = m.reg_read(UC_ARM64_REG_X0)
        if state['pending'][index]:
            count, state['pending'][index] = state['pending'][index], 0
            ret(m, count)
            return
        state['blocked'] = True
        state['slice_done'] = True
        m.emu_stop()

    def give(m):
        if m.reg_read(UC_ARM64_REG_X0) == FAKE_TASK:
            state['pending'][m.reg_read(UC_ARM64_REG_X1)] += 1
            state.setdefault('gives', []).append(m.reg_read(UC_ARM64_REG_X1))
        ret(m, 1)

    def register(m, index):
        if state['deadline_index'] is None:
            state['deadline_index'] = index
        ret(m, 1)

    def mask(m):
        if inject and not state.get('injected'):
            # The input arrives during the close, with its notification, as ss_g2d_run's post and the quiesce step give.
            if inject == 'request':
                put(field('op'), steps['RUN_OP']); put(field('request'), steps['POSTED'])
            else:
                put(field('dev.quiesce_wanted'), 1)
            state['pending'][0] += 1
            state['injected'] = True
        ret(m, 1)


    def note(m):
        state['notes'].append((m.reg_read(UC_ARM64_REG_X0), m.reg_read(UC_ARM64_REG_X1)))
        ret(m)

    hooks = {
        'sunstone_deadline_register': lambda m: register(m, 0),
        'sunstone_deadline_now_us': now,
        'sunstone_deadline_wait_until': wait,
        'ulTaskGenericNotifyTake': block,  # the owner task's block with no timeout
        'xTaskGenericNotify': give,
        'vTaskGenericNotifyGiveFromISR': give,
        'a733_counter_clock_now': counter_now,
        'ss_counter_ticks': lambda m: ret(m, state['time'] * 24),
        'aarch64_device_translate': translate,
        'a733_platform_lock_enter': lambda m: ret(m, 1),
        'a733_platform_lock_leave': lambda m: ret(m),
        'board_live': lambda m: ret(m, 1),
        'os_cpu_return_requested': lambda m: ret(m, state['requested']),
        'os_cpu_return_failing': lambda m: ret(m, 0),
        'os_application_wake_task': lambda m: ret(m),
        'gicv3_spi_mask': mask,
        'gicv3_spi_masked': lambda m: ret(m, 1),
    }
    if continuous:
        put(field('attached'), 1)
        hooks['os_iommu_detach'] = detach
    if image.symbols.get('sunstone_deadline_register_index'):
        hooks['sunstone_deadline_register_index'] = lambda m: register(m, 0 if mutant else m.reg_read(UC_ARM64_REG_X0))
    for symbol, callback in hooks.items():
        hook_addresses(uc, {image.address(symbol)}, lambda m, a, s, u, c=callback: c(m))
    hook_addresses(uc, {NOTE}, lambda m, a, s, u: note(m))
    hook_addresses(uc, {SENTINEL}, lambda m, a, s, u: m.emu_stop())

    def enter(function, *args, stack=STACKS + 0x80000):
        for register, value in zip((UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2), args):
            uc.reg_write(register, value)
        uc.reg_write(UC_ARM64_REG_SP, stack)
        uc.reg_write(UC_ARM64_REG_LR, SENTINEL)
        uc.reg_write(UC_ARM64_REG_PC, function)

    def call(function, *args, stack=STACKS + 0x80000):
        enter(function, *args, stack=stack)
        uc.emu_start(function, SENTINEL, count=5_000_000)
        return uc.reg_read(UC_ARM64_REG_X0)

    quiesce = image.address('g2d_quiesce', UNIT)
    assert call(image.address('os_quiesce_register'), quiesce) == 1, (name, 'registration refused')
    state['requested'] = 1
    task = image.address('os_g2d_task', UNIT)
    if hints:
        # The row's wake and INTID 83 with nothing armed: the notification index each gives the owner.
        call(image.address('wake_owner', UNIT), 0)
        call(image.address('os_g2d_irq'))
        return {'gives': state.get('gives', [])}

    def owner_pass():
        # A blocked owner runs again only after a give at index 0; it resumes at its loop's top, which a return from
        # the block reaches.
        if state['blocked']:
            if not state['pending'][0]:
                state['slice_done'] = True
                return
            state['pending'][0], state['blocked'] = 0, False
        call(task, 0, stack=STACKS + 0x40000)

    if inject == 'quiesce':
        # The application's close of the FAULT service, served before any quiesce.
        put(field('op'), steps['CLOSE_OP']); put(field('request'), steps['POSTED'])
        state['slice_done'] = False
        owner_pass()
    start, results, events = state['time'], [], None
    while state['time'] - start < BOUND_US:
        result = call(image.address('os_quiesce_step'), NOTE) & 0xffffffff
        results.append(result)
        if result == QUIESCE['DONE']:
            break
        state['slice_done'] = False
        if schedule and events is None:
            # The owner's close, with one more quiesce step on the supervisor's core placed among its stores.
            shared = [(field('dev.' + n), 4) for n in ('phase', 'close_started', 'reported', 'quiesced')]
            cores = TwoCores(uc, shared, SENTINEL)
            enter(task, 0, stack=STACKS + 0x40000)
            writer = uc.context_save()
            enter(image.address('os_quiesce_step'), NOTE)
            reader = uc.context_save()
            events = cores.run(writer, reader, schedule, lambda: state['slice_done'],
                               lambda: uc.reg_read(UC_ARM64_REG_PC) == SENTINEL)
            results.append(uc.reg_read(UC_ARM64_REG_X0) & 0xffffffff)
            cores.close()
            if results[-1] == QUIESCE['DONE']:
                break
        else:
            owner_pass()
        assert state['slice_done'], (name, 'the owner task did not reach its wait')
        state['time'] += PASS_US
    run = None
    if run_after:
        # The application's run, posted after the failed close, then one more supervisor pass.
        before = (get(field('dev.fault_step')), get(field('dev.fault_value')), get(field('dev.phase')))
        put(field('op'), steps['RUN_OP']); put(field('request'), steps['POSTED'])
        state['slice_done'] = False
        call(task, 0, stack=STACKS + 0x40000)
        assert state['slice_done'] and get(field('request')) == steps['DONE'], (name, 'the run was not answered')
        result = int.from_bytes(uc.mem_read(field('result'), 4), 'little', signed=True)
        results.append(call(image.address('os_quiesce_step'), NOTE) & 0xffffffff)
        run = {'result': result, 'before': before,
               'after': (get(field('dev.fault_step')), get(field('dev.fault_value')), get(field('dev.phase')))}
    posted = None
    if inject:
        posted = {'injected': bool(state.get('injected')), 'request': get(field('request')),
                  'op': get(field('op')), 'result': int.from_bytes(uc.mem_read(field('result'), 4), 'little', signed=True),
                  'deadline_index': state['deadline_index'], 'blocked': state['blocked'],
                  'waits_with_input_pending': sum(1 for w in state['waits'] if w['pending'][0])}
    return {
        'posted': posted,
        'run': run,
        'case': name, 'fault_step': fault_step, 'passes': len(results), 'last_result': results[-1],
        'done_within_bound': results[-1] == QUIESCE['DONE'], 'elapsed_us': state['time'] - start,
        'schedule': schedule, 'events': events, 'any_fault': QUIESCE['FAULT'] in results,
        'phase': get(field('dev.phase')), 'recorded_fault_step': get(field('dev.fault_step')),
        'notes': [(hex(s), hex(v)) for s, v in state['notes']],
        'ccu': {hex(k): hex(v) for k, v in sorted(ccu.words.items())},
        'g2d_accesses': len(g2d_regs.reads) + len(g2d_regs.writes), 'g2d_20': hex(g2d_regs.words.get(0x20, 0)),
    }


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    image = Image(a.image, {'board_application', 'a733_board_session'})
    layout, steps = None, {}
    with a.image.open('rb') as stream:
        elf = ELFFile(stream)
        parts = [(int(s['p_vaddr']), bytes(s.data()), int(s['p_memsz']))
                 for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']
        for cu in elf.get_dwarf_info().iter_CUs():
            if not cu.get_top_DIE().attributes['DW_AT_name'].value.decode().endswith(UNIT):
                continue
            for die in cu.iter_DIEs():
                if die.tag == 'DW_TAG_variable' and die.attributes.get('DW_AT_name') and \
                        die.attributes['DW_AT_name'].value == b'g2d' and 'DW_AT_type' in die.attributes:
                    layout = flat_members(die.get_DIE_from_attribute('DW_AT_type'))
                if die.tag == 'DW_TAG_enumerator':
                    steps[die.attributes['DW_AT_name'].value.decode()] = die.attributes['DW_AT_const_value'].value
    assert layout and steps['OS_DEVICE_FAULT'] == FAULT_PHASE and steps['OS_DEVICE_CLOSED'] == CLOSED_PHASE
    opened = {0xa40: 0x80000000, 0xa44: 0x00010001, 0xa74: 0x00010000}
    cases = [
        ('fault-after-engine-open', steps['G2D_STEP_TOP'], opened, None),
        ('fault-before-bus-gate', steps['G2D_STEP_BUS_GATE'], {0xa40: 0, 0xa44: 0x00010000, 0xa74: 0x00010000}, None),
        ('close-step-fails', steps['G2D_STEP_TOP'], opened, {0xa44: 0x00010001}),
        ('run-after-failed-close', steps['G2D_STEP_TOP'], opened, {0xa44: 0x00010001}),
    ]
    rows, failures = [], []
    injected = {'request-during-close': ('request', False), 'quiesce-during-close': ('quiesce', False),
                'deadline-waits-on-index-0': ('request', True)}
    for name in injected:
        cases.append((name, steps['G2D_STEP_TOP'], opened, None))
    for name, step, words, stuck in cases:
        inject, mutant = injected.get(name, (None, False))
        row = run_case(image, parts, layout, steps, name, step, words, stuck, run_after=name == 'run-after-failed-close',
                       inject=inject, mutant=mutant)
        problems = []
        if inject:
            p = row['posted']
            want_op = steps['RUN_OP'] if inject == 'request' else steps['CLOSE_OP']
            want_result = -1 if inject == 'request' else 1
            if not p['injected'] or not p['waits_with_input_pending']:
                problems.append('the input did not arrive during a deadline wait of the close')
            elif p['request'] != steps['DONE'] or p['op'] != want_op or p['result'] != want_result:
                problems.append(f"the owner left the {inject} unanswered: request {p['request']}, op {p['op']}, "
                                f"result {p['result']} (expected DONE, {want_op}, {want_result}); deadline index "
                                f"{p['deadline_index']}, owner blocked {p['blocked']}")
            if mutant:
                # The mutant must fail; its row passes when it does.
                problems = [] if problems else ['an owner whose deadline waits take index 0 was not detected']
        if name == 'run-after-failed-close':
            run = row['run']
            if run['result'] != -1:
                problems.append(f"a run on a FAULT service must be refused: result {run['result']}")
            if run['after'] != run['before'] or run['before'] != (step, 0, FAULT_PHASE):
                problems.append(f"the refused run must leave the fault and the phase unchanged: {run['before']} -> {run['after']}")
        if name in ('close-step-fails', 'run-after-failed-close'):
            if row['done_within_bound'] or row['last_result'] != QUIESCE['FAULT']:
                problems.append('a failed close step must leave the service FAULT')
            if len(row['notes']) != 1 or int(row['notes'][0][0], 16) & 0xfff != SITE_G2D:
                problems.append('one fault note with site 0x931 expected')
            elif int(row['notes'][0][1], 16) != step << 24:
                problems.append(f"the note's value must be the first fault, {step << 24:#x}: {row['notes'][0][1]}")
        else:
            if not row['done_within_bound']:
                problems.append(f'the registry did not reach DONE within {BOUND_US} us: the return stops at 0x742')
            if row['notes']:
                problems.append('a closed service must not publish a fault note')
            if row['phase'] != CLOSED_PHASE or row['recorded_fault_step'] != step:
                problems.append('the service must be CLOSED with its first fault kept')
            ccu = {int(k, 16): int(v, 16) for k, v in row['ccu'].items()}
            if ccu.get(0xa40, 0) & 0x80000000 or ccu.get(0xa44, 0) & 0x10001 or ccu.get(0xa74, 0) & 0x10000:
                problems.append('the module and bus gates must be closed and both resets asserted')
            if name == 'fault-before-bus-gate' and row['g2d_accesses']:
                problems.append('no G2D register may be touched before its bus clock and reset open')
            if name == 'fault-after-engine-open' and int(row['g2d_20'], 16) & 0x50:
                problems.append('0x20 update and interrupt bits must be cleared')
        row['passed'] = not problems
        row['problems'] = problems
        if problems:
            failures.append(name)
        rows.append(row)
    # The producers that end the owner's deadline waits give the index those waits take.
    index = next(r['posted']['deadline_index'] for r in rows if r['case'] == 'request-during-close')
    gives = run_case(image, parts, layout, steps, 'hints-reach-deadline-waits', steps['G2D_STEP_TOP'], opened, None,
                     hints=True)['gives']
    ok = index not in (None, 0) and gives == [index, index]
    rows.append({'case': 'hints-reach-deadline-waits', 'deadline_index': index, 'gives': gives, 'passed': ok,
                 'problems': [] if ok else [f'the row wake and INTID 83 must give the deadline index {index}, not '
                                            f'index 0: gave {gives}']})
    if not ok:
        failures.append('hints-reach-deadline-waits')
    # The quiesce step on the supervisor's core placed at every point among the close's stores to the words it reads,
    # with its loads straddling one or more of them: no placement may report the fault of a close that succeeds.
    base = ('fault-after-engine-open', steps['G2D_STEP_TOP'], opened, None)
    writer_events = run_case(image, parts, layout, steps, *base, schedule=(0, 0, 0))['events']['writer']
    sweep = [(w, k, b) for w in range(writer_events + 1) for k in range(4) for b in range(w, writer_events + 1)]
    races = []
    for schedule in sweep:
        row = run_case(image, parts, layout, steps, *base, schedule=schedule)
        if row['any_fault'] or row['notes'] or not row['done_within_bound'] or row['phase'] != CLOSED_PHASE:
            races.append({'schedule': schedule, 'events': row['events'], 'notes': row['notes'], 'phase': row['phase']})
    rows.append({'case': 'quiesce-among-close-stores', 'writer_events': writer_events, 'schedules': len(sweep),
                 'reported': races[:8], 'passed': not races and writer_events >= 2,
                 'problems': [f'{len(races)} of {len(sweep)} placements reported 0x931 during a close that succeeds']
                 if races else [] if writer_events >= 2 else ['the close must store at least two shared words']})
    if not rows[-1]['passed']:
        failures.append('quiesce-among-close-stores')
    save(a.output / 'result.json', {
        'passed': not failures, 'physical_access': False, 'image': str(a.image),
        'image_sha256': hashlib.sha256(a.image.read_bytes()).hexdigest(), 'cases': rows,
        'sources_sha256': {n: hashlib.sha256((Path(__file__).resolve().parents[1] / n).read_bytes()).hexdigest()
                           for n in ('apps/os/g2d.c', 'apps/os/quiesce.c', 'checks/os-g2d-fault-return.py')}})
    for row in rows:
        detail = f"{row['schedules']} placements over {row['writer_events']} close stores" if 'schedules' in row else \
            f"gives {row['gives']}" if 'gives' in row else f"{row['passes']} passes, {row['elapsed_us']} us"
        print('PASS' if row['passed'] else 'FAIL', row['case'], detail, '; '.join(row['problems']))
    if failures:
        raise SystemExit(f'{len(failures)} of {len(rows)} cases failed')
    print('PASS faulted G2D service: closes and returns within the bound; a failed close step stays FAULT')


if __name__ == '__main__':
    main()
