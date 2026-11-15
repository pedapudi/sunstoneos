#!/usr/bin/env python3
"""Execute the linked return of a faulted video decoder service (apps/os/video_decode.c) through the production quiesce
registry.

The image's own os_quiesce_register, os_quiesce_step, decoder quiesce step, owner task and decoder platform binding run
under AArch64 emulation, with the owner task run between supervisor passes as its task would be. The binding is
initialized by its own a733_video_decoder_init from a configuration at the vendor point; the decoder is then left as
the stage-1 board run 1794712072331160946-bd447f26 left it, refused in hold B (cold step 4) with PLL_VE0 running and
open, the module gate open, and reset and bus gate released. The CCU, RTC, R_CCU, GIC, PCK and decoder pages are
plain registers, and every page the binding uses passes the image's own Device-mapping check, made once at the
decoder's init: each case translates each of the ten pages once, there, however many registers it accesses. The clock and PCK lease
registries answer that the decoder's leases are held. Time advances 10 ms per supervisor pass; the supervisor's
shared bound is 3 s (OS_FAILURE_RETURN_US, apps/os/cpu-return.c), after which the image stops at site 0x742 and the
watchdog reset ends the session.

Cases, each starting from a service that failed in hold B (phase FAULT, step VD_STEP_COLD):
- quiesce-closes: the quiesce asks the owner task, which runs the service's one close in the vendor order: module
  gate, bus gate, reset, then PLL_VE0's output gate, lock enable, enable and LDO. The registry reaches DONE within the
  bound with no fault note, the service is CLOSED with the first fault kept and the record marked fault-closed, and no
  decoder register is touched.
- application-closes: the application's close request (as apps/vdec-check/main.c makes after a failed open) runs the
  same close and is answered -1; the registry then reaches DONE.
- close-step-fails: the CCU bus-gate word does not hold its write. The close fails, the quiesce step reports FAULT
  with site 0x941 once per pass, and the registry never reaches DONE: the session fails at the bound.
- uncertain-access: the binding's failure was an access with an unknown effect. The close touches nothing and the
  registry reports FAULT (site 0x941).
- quiesce-among-close-stores: as quiesce-closes, but the quiesce step that runs during the close is placed at every
  point among the close's stores to the words the step reads (phase, close_started, reported and quiesced), with a
  pause after each of its first three loads of them so that its loads can straddle one or more stores
  (harness.TwoCores). No placement may report 0x941; every one must reach DONE with the service CLOSED.
One more case starts from a CLOSED service whose open the application has requested, with the CCU at the port 3-3 FEL
words and the published nonsecure boot ROM mapped:
- row-wait: the open runs its read-only entry hold and requests domain 2, which the owner never answers. Before
  supply verification ends, the service waits on the supply budget (A733_POWER_SUPPLY_SETTLE_US +
  A733_POWER_SUPPLY_BUDGET_US + one tick, anchored at the PMIC startup deadline or now); once DCDC2 is verified,
  on the PCK600 bound plus two ticks. Past that bound the open is refused with step VD_STEP_ROW_TIMEOUT: the service
  ends CLOSED with no register written and no close, and the binding drops the open's deadline so that a reopen runs.
- row-wait-deadline: the same open, but the call deadline (VD_CALL_US) passes during the supply wait, so the binding
  refuses the open at its deadline with the row still pending. The service records the same step,
  VD_STEP_ROW_TIMEOUT, and ends the same way.
- record-layout: every member offset of the board record (struct vdec_check_record, with the binding's cold record
  and the owner's transition record embedded) in apps/vdec-check/host.py equals the image's DWARF, so the host
  decodes the bytes the image returns.
The image of the hold-B run (c1a4e4fc...) fails the first two cases: its quiesce step reports FAULT on every pass
until the bound. The image of the stage-1 rerun (7d63776e...) fails quiesce-among-close-stores: its quiesce step
decides from phase and close_started.

Usage: os-vdec-fault-return.py --image ELF --output DIR"""
import argparse
import hashlib
import importlib.util
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn.arm64_const import (UC_ARM64_REG_PC, UC_ARM64_REG_LR, UC_ARM64_REG_SP, UC_ARM64_REG_X0,
                                 UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3)
from harness import machine as harness_machine, Image, hook_addresses, save, skip_crumbs, TwoCores

GATE_RUNS = [['--image', '{elf:ret-vdec}', '--output', '{output}']]
UNIT = '/apps/os/video_decode.c'
CCU, RTC, R_CCU, GIC, PCK, VE = 0x02002000, 0x07090000, 0x07010000, 0x03400000, 0x07062000, 0x01c0e000
STACKS, SENTINEL, NOTE, SCRATCH = 0x60000000, 0x60100000, 0x60100100, 0x60100200
RET, PMIC_NOW, SESSION = 0x60100600, 0x60100700, 0x60100800
ROM, ROM_BASE = Path.home() / '.cache/sunstoneos/sources/brom/ns-brom.bin', 0x17000
ROM_SHA256 = '95be69c916846a8259578565f3193a6b9ab0625294d5374f3dc6c2f667fc6169'
# The port 3-3 FEL words (checks/a733-video-cold.c): decoder in reset and gated, PLL_VE0 disabled.
FEL = {0x220: 0x41305a00, 0x22c: 0x30000000, 0x580: 0x80000000, 0x588: 0x80000000, 0x5c0: 0x110103ff,
       0x5e0: 0xf1055803, 0x5e4: 0x4, 0x1b10: 0x00020000, 0x1918: 0x00030003, 0x1998: 0x00030000}
SETTLE_US, BUDGET_US, PCK_BOUND_US, TICK_US = 5160, 100000, 10000, 1000
CALL_US = 500000  # VD_CALL_US, the open's deadline
DCDC2_VERIFIED = 1 << 2
BOUND_US, PASS_US = 3_000_000, 10_000
# The pages a733_video_unit_native_map checks, once per boot from the decoder's init, each with one write
# translation (drivers/video/a733_video_unit.c, pages[]). A page walk per access would translate once per register
# access instead.
MAPPED_PAGES = 10
QUIESCE = {'PENDING': 0, 'DONE': 1, 'FAULT': 2}
SITE_DECODER = 0x941
VD_OWNER = 0x5645440000000001
OSC_PCK, STABLE, COLD_NS_ROM = 1 | 1 << 3, 1, 1
PLL_INHERITED = 1 << 24
# The board's state after hold B refused (run bd447f26): PLL_VE0 running the vendor profile with its output open,
# the module gate open at mux 1, reset and bus gate released, the MBUS clock gate opened with its key.
HOLD_B = {0x220: 0xf9204d00, 0xa88: 0x81000000, 0xa8c: 0x00040004, 0x5e4: 0x00040306, 0x5c0: 0x1101037f,
          0x5e0: 0xf1045803, 0x1b10: 0x00020000, 0x1918: 0x00030003, 0x1998: 0x00030000}
LAYOUTS = {'board_application', 'a733_board_session', 'a733_video_decoder', 'a733_video_unit',
           'a733_video_platform_config', 'a733_clock_token', 'a733_gpu_resource_lease', 'a733_power_domain_row',
           'vdec_check_record', 'board_provider', 'a733_native_usb', 'a733_rail_leases', 'a733_power_domains',
           'a733_pmic', 'a733_pmic_config', 'a733_rail_token', 'a733_video_cold_record', 'a733_power_transition',
           'video_decode_check_result'}


class Registers:
    """Plain 32-bit registers on one page, with optional words that keep a fixed value."""
    def __init__(self, base, size, words, stuck=None):
        self.base, self.size, self.words, self.stuck, self.writes, self.reads = base, size, dict(words), stuck or {}, [], []

    def read(self, uc, offset, size, data):
        self.reads.append(offset)
        return self.words.get(offset, 0)

    def write(self, uc, offset, size, value, data):
        self.writes.append((offset, value))
        self.words[offset] = self.stuck.get(offset, value)


def members(image):
    """Member offsets of the service's anonymous state structure and its enumerators."""
    layout, enums = None, {}
    with open(image, 'rb') as stream:
        elf = ELFFile(stream)
        for cu in elf.get_dwarf_info().iter_CUs():
            if not cu.get_top_DIE().attributes['DW_AT_name'].value.decode().endswith(UNIT):
                continue
            for die in cu.iter_DIEs():
                if die.tag == 'DW_TAG_variable' and die.attributes.get('DW_AT_name') and \
                        die.attributes['DW_AT_name'].value == b'vd' and 'DW_AT_type' in die.attributes:
                    layout = {c.attributes['DW_AT_name'].value.decode(): c.attributes['DW_AT_data_member_location'].value
                              for c in die.get_DIE_from_attribute('DW_AT_type').iter_children() if c.tag == 'DW_TAG_member'}
                if die.tag == 'DW_TAG_enumerator':
                    enums[die.attributes['DW_AT_name'].value.decode()] = die.attributes['DW_AT_const_value'].value
    return layout, enums


def run_case(image, parts, layout, enums, name, stuck=None, uncertain=False, application_close=False, schedule=None, row_wait=False):
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
    ccu = Registers(CCU, 0x2000, FEL if row_wait else HOLD_B, stuck)
    uc.mem_write(RET, (0xd65f03c0).to_bytes(4, 'little'))             # ret
    if row_wait:
        rom = ROM.read_bytes()
        assert hashlib.sha256(rom).hexdigest() == ROM_SHA256, 'published nonsecure ROM differs'
        uc.mem_map(ROM_BASE, len(rom))
        uc.mem_write(ROM_BASE, rom)
    others = {'rtc': Registers(RTC, 0x1000, {0x160: 0x983fb0f7}), 'r_ccu': Registers(R_CCU, 0x1000, {0x1ac: 1}),
              'gic': Registers(GIC, 0x1000, {}), 'pck': Registers(PCK, 0x1000, {0: 8, 8: 8}),
              've': Registers(VE, 0x1000, {})}
    ccu.words[0] = 0xf8675f00
    for regs in (ccu, *others.values()):
        uc.mmio_map(regs.base, regs.size, regs.read, None, regs.write, None)
    state = {'time': 1000, 'requested': 0, 'notes': [], 'slice_done': False, 'waits': [], 'translations': 0}
    L = image.layouts
    vd = image.address('vd', UNIT)

    def put(address, value, n=4): uc.mem_write(address, int(value).to_bytes(n, 'little'))
    def get(address, n=4): return int.from_bytes(uc.mem_read(address, n), 'little')
    config = vd + layout['config']
    decoder = vd + layout['decoder']
    unit = decoder + L['a733_video_decoder']['unit']
    def unit_field(field): return unit + L['a733_video_unit'][field]
    def config_field(field): return config + L['a733_video_platform_config'][field]

    board = image.address('board')
    put(board + L['board_application']['session'] + L['a733_board_session']['deadline_us'], 60_000_000, 8)

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

    def pmic_now(m):
        put(m.reg_read(UC_ARM64_REG_X1), state['time'], 8)
        ret(m, 1)

    def translate(m):
        address, out = m.reg_read(UC_ARM64_REG_X0), m.reg_read(UC_ARM64_REG_X2)
        state['translations'] += 1
        # physical, Device-nGnRnE (MAIR attribute 0), outer shareable (PAR_EL1.SH 2)
        uc.mem_write(out, address.to_bytes(8, 'little') + (0).to_bytes(4, 'little') + (2).to_bytes(4, 'little'))
        ret(m, 1)

    def wait(m):
        state['waits'].append((m.reg_read(UC_ARM64_REG_X0), state['time']))
        state['slice_done'] = True
        m.emu_stop()

    def note(m):
        state['notes'].append((m.reg_read(UC_ARM64_REG_X0), m.reg_read(UC_ARM64_REG_X1)))
        ret(m)

    hooks = {
        'sunstone_deadline_register': lambda m: ret(m, 1),
        'sunstone_deadline_now_us': now,
        'sunstone_deadline_wait_until': wait,
        'ulTaskGenericNotifyTake': wait,  # the owner task's idle block
        'a733_counter_clock_now': counter_now,
        'a733_pmic_now': pmic_now,
        'ss_counter_ticks': lambda m: ret(m, state['time'] * 24),
        'aarch64_device_translate': translate,
        'a733_platform_lock_enter': lambda m: ret(m, 1),
        'a733_platform_lock_leave': lambda m: ret(m),
        'a733_clock_token_status': lambda m: ret(m, 0),       # A733_CLOCK_OK: the decoder's clock lease is held
        'a733_gpu_resources_status': lambda m: ret(m, 0),     # A733_GPU_RESOURCE_OK: its PCK lease is held
        'board_live': lambda m: ret(m, 1),
        'os_cpu_return_requested': lambda m: ret(m, state['requested']),
        'os_cpu_return_failing': lambda m: ret(m, 0),
        'os_application_wake_task': lambda m: ret(m),
        'os_session_admitted': lambda m: ret(m, SESSION),
        'a733_rail_poll': lambda m: ret(m, 0),                  # A733_RAIL_READY: the decoder's DCDC2 token
    }
    for symbol, callback in hooks.items():
        if symbol in image.symbols:
            hook_addresses(uc, {image.address(symbol)}, lambda m, a, s, u, c=callback: c(m))
    hook_addresses(uc, {NOTE}, lambda m, a, s, u: note(m))
    hook_addresses(uc, {PMIC_NOW}, lambda m, a, s, u: pmic_now(m))   # the PMIC adapter's clock (a733_pmic_now)
    hook_addresses(uc, {SENTINEL}, lambda m, a, s, u: m.emu_stop())

    def enter(function, *args, stack=STACKS + 0x80000):
        for register, value in zip((UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3), args):
            uc.reg_write(register, value)
        uc.reg_write(UC_ARM64_REG_SP, stack)
        uc.reg_write(UC_ARM64_REG_LR, SENTINEL)
        uc.reg_write(UC_ARM64_REG_PC, function)

    def call(function, *args, stack=STACKS + 0x80000):
        enter(function, *args, stack=stack)
        uc.emu_start(function, SENTINEL, count=5_000_000)
        return uc.reg_read(UC_ARM64_REG_X0)

    # The configuration tokens() builds, then the binding's own initialization.
    put(config_field('clocks'), SCRATCH, 8); put(config_field('power'), SCRATCH, 8)
    put(config_field('owner'), VD_OWNER, 8); put(config_field('handoff'), 1, 8)
    put(config_field('module'), 0x81000000); put(config_field('module_hz'), 624_000_000)
    for i, (word, hz) in enumerate(((0, 24_000_000), (0x80000000, 24_000_000), (0x80000000, 24_000_000))):
        put(config_field('fabric') + 4 * i, word); put(config_field('fabric_hz') + 4 * i, hz)
    put(config_field('start'), COLD_NS_ROM)
    row = vd + layout['row']
    put(config_field('row'), row, 8); put(row + L['a733_power_domain_row']['domain'], 2)
    assert call(image.address('a733_video_platform_scopes'), config, SCRATCH + 0x100) == 1, (name, 'scopes refused')
    shared = get(SCRATCH + 0x100, 16)
    call(image.address('a733_video_platform_exclusive'), config)
    exclusive = uc.reg_read(UC_ARM64_REG_X0) | uc.reg_read(UC_ARM64_REG_X1) << 64
    clock, token = config_field('clock'), L['a733_clock_token']
    put(clock + token['id'], 1, 8); put(clock + token['owner'], VD_OWNER, 8)
    put(clock + token['shared'], shared, 16); put(clock + token['exclusive'], exclusive, 16)
    lease, pck_layout = config_field('pck'), L['a733_gpu_resource_lease']
    put(lease + pck_layout['token'], 1, 8); put(lease + pck_layout['mask'], OSC_PCK); put(lease + pck_layout['mode'], STABLE)
    assert call(image.address('a733_video_decoder_init'), decoder, config, image.address('ops', UNIT), 0) == 1, \
        (name, 'the binding refused its configuration')
    # The init checks the pages, so no hold that follows walks a page table.
    assert state['translations'] == MAPPED_PAGES, (name, 'the init must check the pages', state['translations'])
    if row_wait:
        return row_wait_case(uc, image, layout, enums, state, put, get, ret, task_call=lambda: call(
            image.address('os_video_decode_task', UNIT), 0, stack=STACKS + 0x40000), ccu=ccu, others=others, vd=vd,
            at_deadline=row_wait == 'deadline')
    # The decoder as hold B's refusal left it.
    put(unit_field('failed'), 1)
    for field, value in (('pll_inherited', PLL_INHERITED), ('pll_state', 1), ('pll_started', 1), ('pll_locked', 1),
                         ('pll_done', 1)):
        put(unit_field(field), value)
    if uncertain:
        # An uncertain access keeps the register mutex (a733_video_unit_refuse_held); an image before the
        # failure-cause flags has no uncertain member.
        put(unit_field('locked'), 1)
        if 'uncertain' in L['a733_video_unit']:
            put(unit_field('uncertain'), 1)
    for field, value in (('admitted', 1), ('requested', 1), ('cold_step', 4)):
        put(decoder + L['a733_video_decoder'][field], value)
    put(vd + layout['initialized'], 1)
    put(vd + layout['phase'], enums['FAULT'])
    put(vd + layout['fault_step'], enums['VD_STEP_COLD']); put(vd + layout['fault_value'], 0x204)
    if 'info' in layout:
        info = vd + layout['info']
        put(info + L['vdec_check_record']['magic'], 0x49444556)

    task = image.address('os_video_decode_task', UNIT)
    def owner_slice():
        state['slice_done'] = False
        call(task, 0, stack=STACKS + 0x40000)
        assert state['slice_done'], (name, 'the owner task did not reach its wait')

    close_answer = None
    if application_close:
        put(vd + layout['op'], enums['CLOSE_OP']); put(vd + layout['request'], enums['POSTED'])
        owner_slice()
        if get(vd + layout['request']) == enums['DONE']:
            close_answer = get(vd + layout['result']) - (1 << 32) if get(vd + layout['result']) >> 31 else \
                get(vd + layout['result'])
    quiesce = image.address('video_decode_quiesce', UNIT)
    assert call(image.address('os_quiesce_register'), quiesce) == 1, (name, 'registration refused')
    state['requested'] = 1
    start, results, events = state['time'], [], None
    while state['time'] - start < BOUND_US:
        result = call(image.address('os_quiesce_step'), NOTE) & 0xffffffff
        results.append(result)
        if result == QUIESCE['DONE']:
            break
        if schedule and events is None:
            # The owner's close, with one more quiesce step on the supervisor's core placed among its stores.
            shared = [(vd + layout[n], 4) for n in ('phase', 'close_started', 'reported', 'quiesced') if n in layout]
            cores = TwoCores(uc, shared, SENTINEL)
            state['slice_done'] = False
            enter(task, 0, stack=STACKS + 0x40000)
            writer = uc.context_save()
            enter(image.address('os_quiesce_step'), NOTE)
            reader = uc.context_save()
            events = cores.run(writer, reader, schedule, lambda: state['slice_done'],
                               lambda: uc.reg_read(UC_ARM64_REG_PC) == SENTINEL)
            results.append(uc.reg_read(UC_ARM64_REG_X0) & 0xffffffff)
            cores.close()
            assert state['slice_done'], (name, 'the owner task did not reach its wait')
            if results[-1] == QUIESCE['DONE']:
                break
        else:
            owner_slice()
        state['time'] += PASS_US
    record = {}
    if 'info' in layout and 'fault_closed' in L['vdec_check_record']:
        record = {k: get(info + L['vdec_check_record'][k]) for k in ('fault_closed', 'fault_step', 'failed')}
    return {
        'case': name, 'passes': len(results), 'last_result': results[-1],
        'done_within_bound': results[-1] == QUIESCE['DONE'], 'elapsed_us': state['time'] - start,
        'phase': get(vd + layout['phase']), 'recorded_fault_step': get(vd + layout['fault_step']),
        'close_answer': close_answer, 'record': record, 'schedule': schedule, 'events': events,
        'any_fault': QUIESCE['FAULT'] in results,
        'notes': [(hex(s), hex(v)) for s, v in state['notes']],
        'ccu': {hex(k): hex(ccu.words.get(k, 0)) for k in (0x220, 0xa88, 0xa8c, 0x5e4)},
        'ccu_writes': [(hex(a), hex(v)) for a, v in ccu.writes],
        've_accesses': len(others['ve'].reads) + len(others['ve'].writes), 'translations': state['translations'],
    }


def row_wait_case(uc, image, layout, enums, state, put, get, ret, task_call, ccu, others, vd, at_deadline=False):
    """The open of a CLOSED service whose row the owner never answers: the two bounds, then the refusal; or, with
    at_deadline, the supply wait, then the refusal of the binding at the open's deadline."""
    L = image.layouts
    put(vd + layout['initialized'], 1)
    put(vd + layout['rail'] + L['a733_rail_token']['id'], 1, 8)
    put(vd + layout['pck_token'], 1, 8)
    row = vd + layout['row']
    put(row + L['a733_power_domain_row']['domains'], SCRATCH, 8)
    put(row + L['a733_power_domain_row']['notify'], RET, 8)
    providers = image.address('providers')
    shared = providers + L['board_provider']['shared']
    pmic = shared + L['a733_native_usb']['pmic']
    put(providers + L['board_provider']['domains'] + L['a733_power_domains']['pmic'], pmic, 8)
    put(pmic + L['a733_pmic']['pmic_bus_ops'], PMIC_NOW, 8)  # its first member, now
    put(pmic + L['a733_pmic']['config'] + L['a733_pmic_config']['startup_deadline'], 0, 8)  # the provider started
    authority = shared + L['a733_native_usb']['rails'] + L['a733_rail_leases']['authority']
    put(vd + layout['op'], enums['OPEN_OP']); put(vd + layout['request'], enums['POSTED'])
    rows = []

    def resume():
        # Return from the wait the task stopped in, as a notification, and run on to its next wait.
        state['slice_done'] = False
        ret(uc, 0)
        uc.emu_start(uc.reg_read(UC_ARM64_REG_PC), SENTINEL, count=5_000_000)
        assert state['slice_done'], 'the owner task did not reach its next wait'
        target, at = state['waits'][-1]
        rows.append(target - at)

    state['slice_done'] = False
    task_call()
    target, at = state['waits'][-1]
    rows.append(target - at)                                   # before verification: the supply budget
    if at_deadline:
        state['time'] += CALL_US + TICK_US
        resume()                                               # past the open's deadline: refused, then the idle block
    else:
        put(authority, get(authority) | DCDC2_VERIFIED)
        resume()                                               # verified: the PCK600 bound
        state['time'] += PCK_BOUND_US + 2 * TICK_US
        resume()                                               # past it: refused, then the idle block
    supply, verified = SETTLE_US + BUDGET_US + TICK_US, PCK_BOUND_US + 2 * TICK_US
    problems = []
    if not supply - 100 <= rows[0] <= supply:
        problems.append(f'before verification the wait must be the supply budget, {supply} us: {rows[0]} us')
    if not at_deadline and not verified - 100 <= rows[1] <= verified:
        problems.append(f'after verification the wait must be the PCK600 bound plus two ticks, {verified} us: {rows[1]} us')
    if get(vd + layout['request']) != enums['DONE'] or get(vd + layout['result']) != 0xffffffff:
        problems.append('the open must be answered -1')
    if get(vd + layout['phase']) != enums['CLOSED'] or get(vd + layout['fault_step']) != enums['VD_STEP_ROW_TIMEOUT']:
        problems.append('the service must end CLOSED with step VD_STEP_ROW_TIMEOUT')
    if ccu.writes or others['ve'].reads or others['ve'].writes:
        problems.append('an unanswered row leaves nothing written and no close')
    if state['translations'] != MAPPED_PAGES:
        problems.append(f"the pages are checked once, {MAPPED_PAGES} translations: {state['translations']}")
    unit = vd + layout['decoder'] + L['a733_video_decoder']['unit']
    if get(unit + L['a733_video_unit']['deadline'], 8) or get(unit + L['a733_video_unit']['failed']):
        problems.append('the refused open must leave the binding with no deadline and not failed, so a reopen runs')
    return {'case': 'row-wait-deadline' if at_deadline else 'row-wait', 'waits_us': rows, 'passes': len(state['waits']), 'elapsed_us': state['time'],
            'phase': get(vd + layout['phase']), 'recorded_fault_step': get(vd + layout['fault_step']),
            'ccu_writes': [(hex(a), hex(v)) for a, v in ccu.writes], 'problems': problems, 'passed': not problems,
            'notes': [(hex(s), hex(v)) for s, v in state['notes']]}


def record_layout(image):
    """The board record's offsets in apps/vdec-check/host.py against the image's DWARF: a list of differences."""
    spec = importlib.util.spec_from_file_location('vdec_check_host', Path(__file__).resolve().parents[1] /
                                                  'apps/vdec-check/host.py')
    host = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(host)
    L, problems = image.layouts, []
    nested = {'cold': 'a733_video_cold_record', 'transition': 'a733_power_transition'}

    def compare(members, name):
        offsets = host.layout(members)[0]
        if set(offsets) != set(L[name]):
            problems.append(f'{name} members differ: {sorted(set(offsets) ^ set(L[name]))}')
        for member, kind, _ in members:
            if L[name].get(member) != offsets[member][0]:
                problems.append(f'{name}.{member} at {L[name].get(member)} in the image, {offsets[member][0]} in host.py')
            if isinstance(kind, tuple):
                compare(kind, nested[member])
    compare(host.RECORD, 'vdec_check_record')
    if L['video_decode_check_result']['info'] != host.HEADER.size:
        problems.append('the record does not follow the 32-byte header')
    return problems


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    image = Image(a.image, LAYOUTS)
    layout, enums = members(a.image)
    with a.image.open('rb') as stream:
        parts = [(int(s['p_vaddr']), bytes(s.data()), int(s['p_memsz']))
                 for s in ELFFile(stream).iter_segments() if s['p_type'] == 'PT_LOAD']
    assert layout and enums['CLOSED'] == 0 and enums['FAULT'] == 4
    cases = [('quiesce-closes', {}), ('application-closes', {'application_close': True}),
             ('close-step-fails', {'stuck': {0xa8c: 0x00040004}}), ('uncertain-access', {'uncertain': True})]
    rows, failures = [], []
    problems = record_layout(image)
    rows.append({'case': 'record-layout', 'passed': not problems, 'problems': problems})
    if problems:
        failures.append('record-layout')
    for name, how in (('row-wait', True), ('row-wait-deadline', 'deadline')):
        row = run_case(image, parts, layout, enums, name, row_wait=how)
        rows.append(row)
        if not row['passed']:
            failures.append(name)
    for name, options in cases:
        row = run_case(image, parts, layout, enums, name, **options)
        problems = []
        if name in ('close-step-fails', 'uncertain-access'):
            if row['done_within_bound'] or row['last_result'] != QUIESCE['FAULT']:
                problems.append('a failed or refused close must leave the service FAULT')
            if not row['notes'] or any(int(s, 16) & 0xfff != SITE_DECODER for s, _ in row['notes']):
                problems.append('fault notes with site 0x941 expected')
            if name == 'uncertain-access' and row['ccu_writes']:
                problems.append('an uncertain failure must not be touched')
        else:
            if not row['done_within_bound']:
                problems.append(f'the registry did not reach DONE within {BOUND_US} us: the return stops at 0x742')
            if row['notes']:
                problems.append('a closed service must not publish a fault note')
            if row['phase'] != enums['CLOSED'] or row['recorded_fault_step'] != enums['VD_STEP_COLD']:
                problems.append('the service must be CLOSED with its first fault kept')
            ccu = {int(k, 16): int(v, 16) for k, v in row['ccu'].items()}
            if ccu[0xa88] & 0x80000000 or ccu[0xa8c] & 0x40004 or ccu[0x220] != 0x01204d00 or ccu[0x5e4] != 0x40306:
                problems.append('module gate, bus gate and reset must be closed and PLL_VE0 disabled to 0x01204d00')
            if row['record'].get('fault_closed') != 1:
                problems.append('the record must say the fault was closed')
            if name == 'application-closes' and row['close_answer'] != -1:
                problems.append('the application close of a faulted service must be answered -1')
        if row['ve_accesses']:
            problems.append('the close must not touch a decoder register')
        if row['translations'] != MAPPED_PAGES:
            problems.append(f"the pages are checked once, at init, {MAPPED_PAGES} translations: {row['translations']}")
        row['passed'] = not problems
        row['problems'] = problems
        if problems:
            failures.append(name)
        rows.append(row)
    # The quiesce step on the supervisor's core placed at every point among the close's stores to the words it reads,
    # with its loads straddling one or more of them: no placement may report the fault of a close that succeeds.
    writer_events = run_case(image, parts, layout, enums, 'quiesce-closes', schedule=(0, 0, 0))['events']['writer']
    sweep = [(w, k, b) for w in range(writer_events + 1) for k in range(4) for b in range(w, writer_events + 1)]
    races = []
    for schedule in sweep:
        row = run_case(image, parts, layout, enums, 'quiesce-closes', schedule=schedule)
        if row['any_fault'] or row['notes'] or not row['done_within_bound'] or row['phase'] != enums['CLOSED']:
            races.append({'schedule': schedule, 'events': row['events'], 'notes': row['notes'], 'phase': row['phase']})
    rows.append({'case': 'quiesce-among-close-stores', 'writer_events': writer_events, 'schedules': len(sweep),
                 'reported': races[:8], 'passed': not races and writer_events >= 2,
                 'problems': [f'{len(races)} of {len(sweep)} placements reported 0x941 during a close that succeeds']
                 if races else [] if writer_events >= 2 else ['the close must store at least two shared words']})
    if not rows[-1]['passed']:
        failures.append('quiesce-among-close-stores')
    root = Path(__file__).resolve().parents[1]
    save(a.output / 'result.json', {
        'passed': not failures, 'physical_access': False, 'image': str(a.image),
        'image_sha256': hashlib.sha256(a.image.read_bytes()).hexdigest(), 'cases': rows,
        'sources_sha256': {n: hashlib.sha256((root / n).read_bytes()).hexdigest()
                           for n in ('apps/os/video_decode.c', 'apps/os/quiesce.c', 'drivers/video/a733_video_platform.c',
                                     'drivers/video/a733_video_unit.c', 'checks/os-vdec-fault-return.py')}})
    for row in rows:
        detail = f"{row['schedules']} placements over {row['writer_events']} close stores" if 'schedules' in row else \
            f"{row['passes']} passes, {row['elapsed_us']} us" if 'passes' in row else 'host.py offsets match the DWARF'
        print('PASS' if row['passed'] else 'FAIL', row['case'], detail, '; '.join(row['problems']))
    if failures:
        raise SystemExit(f'{len(failures)} of {len(rows)} cases failed')
    print('PASS faulted video decoder service: closes and returns within the bound; a failed or uncertain close stays '
          'FAULT')


if __name__ == '__main__':
    main()
