#!/usr/bin/env python3
"""Execute the linked return of a faulted RTC service (apps/os/rtc.c) through the production quiesce registry.

The image's own owner task (os_rtc_task, with the RTC driver drivers/rtc/a733_rtc.c as linked), os_quiesce_register
and os_quiesce_step run under AArch64 emulation. The RTC page (0x07090000) and the distributor words of INTID 228 are
modeled as registers; the clock lease, the register lock and the distributor mask calls are hooked. Each case opens
the service through the owner task (SS_RTC_KEEP_RC, so no crystal selection), makes one request that faults, and then
runs supervisor passes, 10 ms apart, with the owner task between them. The shared bound is 3 s
(OS_FAILURE_RETURN_US, apps/os/cpu-return.c); a registry that is still not DONE then stops the image at site 0x742 and
the watchdog reset ends the session.

Cases (docs/designs/failure-return-audit.md, "A faulted device service closes before it reports"):
- open-line-pending: INTID 228 reads pending at the open (step 1); the driver was never opened. The close masks the
  line and releases the clock lease: DONE, no fault note, the service CLOSED with step 1 kept.
- read-incoherent: the calendar changes on every read, so a calendar read fails (step 4) and the driver is RETAINED
  after reads only. The close settles it: DONE.
- armed-line-refused: the alarm is armed and the distributor refuses to rearm the line (step 7); the driver is ARMED.
  The close cancels the alarm through a733_rtc_finish: alarm enable and interrupt enable read 0 and the alarm words
  are restored; DONE.
- arm-write-fails: the alarm day word does not hold its write, so the arm fails after writing (step 6) and the driver
  is RETAINED. The service stays FAULT and reports site 0x921 once; the registry never reaches DONE.
- close-step-fails: as open-line-pending, but the distributor never reports the line masked during the close
  (step 8). The service stays FAULT with site 0x921.
- reopen-write-retained: a calendar read fails (step 4, settleable), the close returns the service to CLOSED, the
  service opens again, and an alarm arm fails after its write (step 6) with the driver RETAINED. The second open began
  a new fault lifetime, so the close is judged on step 6, not on the earlier lifetime's step 4: the service stays
  FAULT and reports site 0x921 with step 6. An open that keeps the earlier lifetime's fault closes it and returns DONE.
- set-refused: a calendar set with day 0 is refused by the driver before any access. The service stays OPEN, with no
  fault recorded, and the quiesce's close returns DONE.
- quiesce-among-close-stores: as open-line-pending, but the quiesce step that runs during the close is placed at
  every point among the close's stores to the words the step reads (phase, close_started, reported and quiesced), with
  a pause after each of its first three loads of them so that its loads can straddle one or more stores
  (harness.TwoCores). No placement may report 0x921; every one must reach DONE with the service CLOSED.
An image built before the RTC close of a faulted service fails the first three cases and set-refused. An image whose
quiesce step decides from phase and close_started fails quiesce-among-close-stores.

Usage: os-rtc-fault-return.py --image ELF --output DIR"""
import argparse
import hashlib
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn.arm64_const import UC_ARM64_REG_PC, UC_ARM64_REG_LR, UC_ARM64_REG_SP, UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2
from harness import machine as harness_machine, flat_members, Image, hook_addresses, save, skip_crumbs, TwoCores

GATE_RUNS = [['--image', '{elf:ret-rtc}', '--output', '{output}']]
UNIT = '/apps/os/rtc.c'
RTC, GICD = 0x07090000, 0x03400000
STACKS, SENTINEL, NOTE = 0x60000000, 0x60100000, 0x60100100
BOUND_US, PASS_US = 3_000_000, 10_000
QUIESCE = {'PENDING': 0, 'DONE': 1, 'FAULT': 2}
SITE_RTC = 0x921
KEEP_RC = 1
INTID = 228
# The RTC as the board's runs find it: the normal RC source (control and status 0, calibration bit 0 clear, the RC
# oscillator enabled at 0x160), calendar 2026-11-12 04:00:00, the alarm disabled and its words zero.
RTC_WORDS = {0x00: 0, 0x04: 0, 0x08: 0x1f, 0x0c: 0, 0x160: 1, 0x10: 20770, 0x14: 4 << 16,
             0x20: 0, 0x24: 0, 0x28: 0, 0x2c: 0, 0x30: 0, 0x50: 0}


class Registers:
    """Plain 32-bit registers of one page, with words that ignore writes and a calendar that may tick per read."""
    def __init__(self, base, words, stuck=(), ticking=False):
        self.base, self.words, self.stuck, self.ticking, self.writes = base, dict(words), set(stuck), ticking, []

    def read(self, uc, offset, size, data):
        v = self.words.get(offset, 0)
        if self.ticking and offset == 0x14:
            self.words[0x14] = (v + 1) & 0x1f3f3f if (v & 63) < 59 else v & ~63
        return v

    def write(self, uc, offset, size, value, data):
        self.writes.append((offset, value))
        if offset not in self.stuck:
            self.words[offset] = value


def run_case(image, parts, layout, enums, name, request, rtc_words, stuck=(), tick_after_open=False, pending=False,
             close_mask_fails=False, schedule=None, sequence=()):
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
    rtc_regs = Registers(RTC, rtc_words, stuck)
    word = INTID // 32 * 4
    gicd = Registers(GICD, {0x200 + word: (1 << INTID % 32) if pending else 0})
    for regs in (rtc_regs, gicd):
        uc.mmio_map(regs.base, 0x1000, regs.read, None, regs.write, None)
    state = {'time': 1000, 'requested': 0, 'notes': [], 'slice_done': False, 'closing': False, 'released': 0}
    rtc = image.address('rtc', UNIT)

    def put(address, value, n=4): uc.mem_write(address, int(value).to_bytes(n, 'little'))
    def get(address, n=4): return int.from_bytes(uc.mem_read(address, n), 'little')
    def field(name): return rtc + layout['rtc'][name]
    driver = field('rtc')

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

    def wait(m):
        target = m.reg_read(UC_ARM64_REG_X0)
        if target - state['time'] < 1_000_000:
            state['time'] = max(state['time'], target)        # a short pause inside an operation
            ret(m, 1)
            return
        state['slice_done'] = True                            # the owner's idle wait ends the slice
        m.emu_stop()

    def note(m):
        state['notes'].append((m.reg_read(UC_ARM64_REG_X0), m.reg_read(UC_ARM64_REG_X1)))
        ret(m)

    def masked(m):
        ret(m, 0 if state['closing'] and close_mask_fails else 1)

    def release(m):
        state['released'] += 1
        ret(m, enums['A733_CLOCK_OK'])

    hooks = {
        'sunstone_deadline_register': lambda m: ret(m, 1),
        'sunstone_deadline_now_us': now,
        'sunstone_deadline_wait_until': wait,
        'a733_counter_clock_now': counter_now,
        'ss_counter_ticks': lambda m: ret(m, state['time'] * 24),
        'a733_platform_lock_enter': lambda m: ret(m, 1),
        'a733_platform_lock_leave': lambda m: ret(m),
        'a733_clock_acquire': lambda m: ret(m, enums['A733_CLOCK_OK']),
        'a733_clock_release': release,
        'gicv3_spi_mask': lambda m: ret(m, 1),
        'gicv3_spi_masked': masked,
        'gicv3_spi_rearm': lambda m: ret(m, -1),
        'os_cpu_return_requested': lambda m: ret(m, state['requested']),
        'os_cpu_return_failing': lambda m: ret(m, 0),
        'os_application_wake_task': lambda m: ret(m),
        'xTaskGenericNotify': lambda m: ret(m, 1),
    }
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

    task = image.address('os_rtc_task', UNIT)

    def owner_slice(what):
        state['slice_done'] = False
        call(task, 0, stack=STACKS + 0x40000)
        assert state['slice_done'], (name, what, 'the owner task did not reach its wait')

    def serve(op, a=0, b=0):
        put(field('op'), enums[op]); put(field('a'), a); put(field('b'), b); put(field('request'), enums['POSTED'])
        owner_slice(op)
        assert get(field('request')) == enums['DONE'], (name, op, 'not served')
        result = int.from_bytes(uc.mem_read(field('result'), 4), 'little', signed=True)
        put(field('request'), 0)
        return result

    assert call(image.address('os_quiesce_register'), image.address('rtc_quiesce', UNIT)) == 1, (name, 'registration')
    results = {'open': serve('OPEN_OP', KEEP_RC)}
    rtc_regs.ticking = tick_after_open
    if request:
        op, a, b = request
        results[op] = serve(op, a, b)
    # Requests made in order, each after its register change: (op, a, b, ticking, stuck words).
    results['sequence'] = []
    for op, a, b, ticking, words in sequence:
        rtc_regs.ticking, rtc_regs.stuck = ticking, set(words)
        results['sequence'].append((op, serve(op, a, b), get(field('dev.fault_step')), get(field('dev.phase'))))
    fault_after_request = get(field('dev.fault_step'))
    phase_after_request = get(field('dev.phase'))
    driver_after_request = get(driver + layout['a733_rtc']['state'])
    state['requested'] = 1
    state['closing'] = True
    start, passes, events = state['time'], [], None
    while state['time'] - start < BOUND_US:
        result = call(image.address('os_quiesce_step'), NOTE) & 0xffffffff
        passes.append(result)
        if result == QUIESCE['DONE']:
            break
        if schedule and events is None:
            # The owner's close, with one more quiesce step on the supervisor's core placed among its stores.
            shared = [(field('dev.' + n), 4) for n in ('phase', 'close_started', 'reported', 'quiesced')]
            cores = TwoCores(uc, shared, SENTINEL)
            state['slice_done'] = False
            enter(task, 0, stack=STACKS + 0x40000)
            writer = uc.context_save()
            enter(image.address('os_quiesce_step'), NOTE)
            reader = uc.context_save()
            events = cores.run(writer, reader, schedule, lambda: state['slice_done'],
                               lambda: uc.reg_read(UC_ARM64_REG_PC) == SENTINEL)
            passes.append(uc.reg_read(UC_ARM64_REG_X0) & 0xffffffff)
            cores.close()
            assert state['slice_done'], (name, schedule, 'the owner task did not reach its wait')
            if passes[-1] == QUIESCE['DONE']:
                break
        else:
            owner_slice('quiesce')
        state['time'] += PASS_US
    return {
        'case': name, 'results': results, 'fault_after_request': fault_after_request,
        'phase_after_request': phase_after_request, 'driver_after_request': driver_after_request,
        'alarm_enabled': any(o == 0x28 and v & 1 for o, v in rtc_regs.writes), 'passes': len(passes), 'last_result': passes[-1],
        'schedule': schedule, 'events': events, 'any_fault': QUIESCE['FAULT'] in passes,
        'done_within_bound': passes[-1] == QUIESCE['DONE'], 'elapsed_us': state['time'] - start,
        'phase': get(field('dev.phase')), 'fault': get(field('dev.fault_step')),
        'driver_state': get(driver + layout['a733_rtc']['state']),
        'notes': [(hex(s), hex(v)) for s, v in state['notes']], 'clock_releases': state['released'],
        'rtc': {hex(k): hex(v) for k, v in sorted(rtc_regs.words.items())},
    }


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    image = Image(a.image, {'a733_rtc'})
    layout, enums = {'a733_rtc': image.layouts['a733_rtc']}, {}
    with a.image.open('rb') as stream:
        elf = ELFFile(stream)
        parts = [(int(s['p_vaddr']), bytes(s.data()), int(s['p_memsz']))
                 for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']
        for cu in elf.get_dwarf_info().iter_CUs():
            unit = cu.get_top_DIE().attributes['DW_AT_name'].value.decode()
            for die in cu.iter_DIEs():
                if unit.endswith(UNIT) and die.tag == 'DW_TAG_variable' and die.attributes.get('DW_AT_name') and \
                        die.attributes['DW_AT_name'].value == b'rtc' and 'DW_AT_type' in die.attributes:
                    layout['rtc'] = flat_members(die.get_DIE_from_attribute('DW_AT_type'))
                if die.tag == 'DW_TAG_enumerator' and (unit.endswith(UNIT) or 'A733_CLOCK_OK' == die.attributes['DW_AT_name'].value.decode()
                                                       or die.attributes['DW_AT_name'].value.decode().startswith('A733_RTC_')):
                    enums.setdefault(die.attributes['DW_AT_name'].value.decode(), die.attributes['DW_AT_const_value'].value)
    assert 'rtc' in layout and enums['OS_DEVICE_FAULT'] == 4 and enums['OS_DEVICE_CLOSED'] == 0 and enums['OS_DEVICE_OPEN'] == 2, (sorted(enums))
    cases = [
        ('open-line-pending', None, {'pending': True}),
        ('read-incoherent', ('READ_OP', 0, 0), {'tick_after_open': True}),
        ('armed-line-refused', ('ARM_OP', 3, 0), {}),
        ('arm-write-fails', ('ARM_OP', 3, 0), {'stuck': (0x20,)}),
        ('close-step-fails', None, {'pending': True, 'close_mask_fails': True}),
        ('set-refused', ('SET_OP', 0, 4 << 16), {}),
        ('reopen-write-retained', None, {'sequence': (('READ_OP', 0, 0, True, ()), ('CLOSE_OP', 0, 0, False, ()),
                                                      ('OPEN_OP', KEEP_RC, 0, False, ()), ('ARM_OP', 3, 0, False, (0x20,)))}),
    ]
    rows, failures = [], []
    for name, request, options in cases:
        row = run_case(image, parts, layout, enums, name, request, dict(RTC_WORDS), **options)
        problems = []
        closes = name not in ('arm-write-fails', 'close-step-fails', 'reopen-write-retained')
        want = {'read-incoherent': 'A733_RTC_RETAINED', 'armed-line-refused': 'A733_RTC_ARMED',
                'arm-write-fails': 'A733_RTC_RETAINED', 'reopen-write-retained': 'A733_RTC_RETAINED'}.get(name)
        if name == 'reopen-write-retained':
            got = [(op, r, f) for op, r, f, _ in row['results']['sequence']]
            if got != [('READ_OP', -1, 4), ('CLOSE_OP', 1, 4), ('OPEN_OP', 1, 0), ('ARM_OP', -1, 6)]:
                problems.append(f'the read must fault at step 4, the close and reopen succeed with the fault cleared, '
                                f'and the arm fault at step 6: {got}')
        if want and row['driver_after_request'] != enums[want]:
            problems.append(f"the driver must be {want} after the request, not {row['driver_after_request']}")
        if name == 'armed-line-refused' and not row['alarm_enabled']:
            problems.append('the arm must have enabled the alarm')
        if name == 'set-refused':
            if row['results'].get('SET_OP') != -1 or row['phase_after_request'] != enums['OS_DEVICE_OPEN'] or row['fault_after_request']:
                problems.append('an invalid calendar must be refused with the service open and no fault recorded')
        elif row['phase_after_request'] != enums['OS_DEVICE_FAULT']:
            problems.append(f"the request must leave the service FAULT, not phase {row['phase_after_request']}")
        if closes:
            if not row['done_within_bound']:
                problems.append(f'the registry did not reach DONE within {BOUND_US} us: the return stops at 0x742')
            if row['notes']:
                problems.append('a closed service must not publish a fault note')
            if row['phase'] != enums['OS_DEVICE_CLOSED'] or row['fault'] != row['fault_after_request']:
                problems.append('the service must be CLOSED with its first fault kept')
            if row['clock_releases'] != 1:
                problems.append('the clock lease must be released once')
            rtc = {int(k, 16): int(v, 16) for k, v in row['rtc'].items()}
            if (rtc[0x28] | rtc[0x2c]) & 1 or rtc[0x20] != RTC_WORDS[0x20] or rtc[0x24] != RTC_WORDS[0x24]:
                problems.append('the alarm must be disabled and its words restored')
        else:
            if row['done_within_bound'] or row['last_result'] != QUIESCE['FAULT']:
                problems.append('the service must stay FAULT')
            if len(row['notes']) != 1 or int(row['notes'][0][0], 16) & 0xfff != SITE_RTC:
                problems.append('one fault note with site 0x921 expected')
            elif int(row['notes'][0][1], 16) != row['fault'] << 24:
                problems.append(f"the note's value must be the fault step << 24, {row['fault'] << 24:#x}: {row['notes'][0][1]}")
        row['passed'] = not problems
        row['problems'] = problems
        if problems:
            failures.append(name)
        rows.append(row)
    # The quiesce step on the supervisor's core placed at every point among the close's stores to the words it reads,
    # with its loads straddling one or more of them: no placement may report the fault of a close that succeeds.
    base = ('open-line-pending', None, {'pending': True})
    writer_events = run_case(image, parts, layout, enums, *base[:2], dict(RTC_WORDS), **base[2],
                             schedule=(0, 0, 0))['events']['writer']
    sweep = [(w, k, b) for w in range(writer_events + 1) for k in range(4) for b in range(w, writer_events + 1)]
    races = []
    for schedule in sweep:
        row = run_case(image, parts, layout, enums, *base[:2], dict(RTC_WORDS), **base[2], schedule=schedule)
        if row['any_fault'] or row['notes'] or not row['done_within_bound'] or row['phase'] != enums['OS_DEVICE_CLOSED']:
            races.append({'schedule': schedule, 'events': row['events'], 'notes': row['notes'], 'phase': row['phase']})
    rows.append({'case': 'quiesce-among-close-stores', 'writer_events': writer_events, 'schedules': len(sweep),
                 'reported': races[:8], 'passed': not races and writer_events >= 2,
                 'problems': [f'{len(races)} of {len(sweep)} placements reported 0x921 during a close that succeeds']
                 if races else [] if writer_events >= 2 else ['the close must store at least two shared words']})
    if not rows[-1]['passed']:
        failures.append('quiesce-among-close-stores')
    save(a.output / 'result.json', {
        'passed': not failures, 'physical_access': False, 'image': str(a.image),
        'image_sha256': hashlib.sha256(a.image.read_bytes()).hexdigest(), 'cases': rows,
        'sources_sha256': {n: hashlib.sha256((Path(__file__).resolve().parents[1] / n).read_bytes()).hexdigest()
                           for n in ('apps/os/rtc.c', 'drivers/rtc/a733_rtc.c', 'apps/os/quiesce.c', 'checks/os-rtc-fault-return.py')}})
    for row in rows:
        detail = f"{row['schedules']} placements over {row['writer_events']} close stores" if 'schedules' in row else \
            f"fault {row['fault']}, {row['passes']} passes, {row['elapsed_us']} us"
        print('PASS' if row['passed'] else 'FAIL', row['case'], detail, '; '.join(row['problems']))
    if failures:
        raise SystemExit(f'{len(failures)} of {len(rows)} cases failed')
    print('PASS faulted RTC service: closes and returns within the bound when its close can settle the driver; '
          'a write the driver retained or a failed close step stays FAULT')


if __name__ == '__main__':
    main()
