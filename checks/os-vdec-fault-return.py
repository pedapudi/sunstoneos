#!/usr/bin/env python3
"""Execute the linked return of a faulted video decoder service (apps/os/video_decode.c) through the production quiesce
registry.

The image's own os_quiesce_register, os_quiesce_step, decoder quiesce step, owner task and decoder platform binding run
under AArch64 emulation, with the owner task run between supervisor passes as its task would be. The binding is
initialized by its own a733_video_platform_init from a configuration at the vendor point; the decoder is then left as
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
Three cases start from a decode with masters 6 and 7 attached; the IOMMU owner's detach is a hook that records when
it comes among the CCU writes:
- decode-timeout-closes: the service is FAULT after the completion interrupt did not come. The quiesce's close masks
  INTID 81, runs the binding's close and detaches the masters only after reset is asserted; the registry reaches DONE.
- decode-drained-closes: an OPEN service whose decode drained. The application's close detaches the masters before
  any CCU write and is answered 1.
- decode-detach-fails: as decode-timeout-closes, but the detach fails: the service stays FAULT (site 0x941).
One case runs a decode, from an OPEN service whose decoder is started as after the stage-1 open:
- decode-runs: the application's decode request through the image's do_decode, executor, binding and INTID 81
  handler. The large regions, the allocator and the IOMMU owner are hooks; the decoder model sets completion at the
  trigger, and the planes are filled with the pinned reference at the programmed stride before the handler runs. The
  answer is 1, the copied 2,304 bytes and the answer's SHA-256 equal the reference, the three buffers are mapped at
  the predicted device addresses, both masters are scanned, the handler masks INTID 81, the record holds 661 writes
  and one armed interrupt, and the close that follows detaches the masters before any CCU write.
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
- record-layout: every member offset of the board record (struct vdec_check_record, with the binding's cold record,
  the owner's transition record and the decode record embedded) and of the decode's answer in apps/vdec-check/host.py
  equals the image's DWARF, so the host decodes the bytes the image returns; and apps/vdec-check/fixture.h holds the
  bytes of apps/jpeg/fixture.py.
The image of the hold-B run (c1a4e4fc...) fails the first two cases: its quiesce step reports FAULT on every pass
until the bound. The image of the stage-1 rerun (7d63776e...) fails quiesce-among-close-stores: its quiesce step
decides from phase and close_started.

Usage: os-vdec-fault-return.py --image ELF --output DIR"""
import argparse
import hashlib
import importlib.util
import json
import struct
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn.arm64_const import (UC_ARM64_REG_PC, UC_ARM64_REG_LR, UC_ARM64_REG_SP, UC_ARM64_REG_X0,
                                 UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3)
from harness import machine as harness_machine, flat_members, Image, hook_addresses, save, skip_crumbs, TwoCores

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
LAYOUTS = {'board_application', 'a733_board_session', 'a733_video_decoder', 'a733_video_unit', 'vdec_check_decode',
           'ss_video_decode_done',
           'a733_video_platform_config', 'a733_clock_token', 'a733_gpu_resource_lease', 'a733_power_domain_row',
           'vdec_check_record', 'board_provider', 'a733_native_usb', 'a733_rail_leases', 'a733_power_domains',
           'a733_pmic', 'a733_pmic_config', 'a733_rail_token', 'a733_video_cold_record', 'a733_power_transition',
           'a733_power_service_config', 'video_decode_check_result', 'large_slot'}
# The decode case's memory: the application's two large regions and the owner's allocations.
REGIONS, ARENA = 0x61000000, 0x61200000
IOVA_BASE = 0x10000000  # apps/os/iommu.c
VERSION_E0, VERSION_E4 = 0x00033310, 0x00021320  # identity 0x0003331000021320, the board's (run 6b3ec3f5)


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


class Distributor(Registers):
    """The GIC distributor's set-enable and clear-enable words (GICD_ISENABLERn at 0x100, GICD_ICENABLERn at 0x180)."""
    def write(self, uc, offset, size, value, data):
        self.writes.append((offset, value))
        if 0x100 <= offset < 0x180:
            self.words[offset] = self.words.get(offset, 0) | value
        elif 0x180 <= offset < 0x200:
            self.words[offset - 0x80] = self.words.get(offset - 0x80, 0) & ~value
        else:
            self.words[offset] = value


class Decoder(Registers):
    """The decoder's registers: the trigger byte (0x118) sets completion in the status word (0x11c), whose bits are
    write-one-to-clear; every other word is plain."""
    def write(self, uc, offset, size, value, data):
        self.writes.append((offset, value))
        if offset == 0x11c:
            self.words[0x11c] = self.words.get(0x11c, 0) & ~value
        elif offset == 0x118:
            self.words[0x11c] = 1
            self.triggered = True
        else:
            self.words[offset] = value


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
                    layout = flat_members(die.get_DIE_from_attribute('DW_AT_type'))
                if die.tag == 'DW_TAG_enumerator':
                    enums[die.attributes['DW_AT_name'].value.decode()] = die.attributes['DW_AT_const_value'].value
    return layout, enums


def run_case(image, parts, layout, enums, name, stuck=None, uncertain=False, application_close=False, schedule=None,
             row_wait=False, decode=None, detach=1):
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
              'gic': Distributor(GIC, 0x1000, {}), 'pck': Registers(PCK, 0x1000, {0: 8, 8: 8}),
              've': Decoder(VE, 0x1000, {0xe0: VERSION_E0, 0xe4: VERSION_E4})}
    ccu.words[0] = 0xf8675f00
    for regs in (ccu, *others.values()):
        uc.mmio_map(regs.base, regs.size, regs.read, None, regs.write, None)
    state = {'time': 1000, 'requested': 0, 'notes': [], 'slice_done': False, 'waits': [], 'translations': 0,
             'detaches': []}
    L = image.layouts
    vd = image.address('vd', UNIT)

    def put(address, value, n=4): uc.mem_write(address, int(value).to_bytes(n, 'little'))
    def get(address, n=4): return int.from_bytes(uc.mem_read(address, n), 'little')
    config = vd + layout['config']
    decoder = vd + layout['platform.decoder']
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
        # The IOMMU owner's detach of masters 6 and 7: when it comes among the CCU writes, and its answer.
        'os_iommu_detach': lambda m: (state['detaches'].append((m.reg_read(UC_ARM64_REG_X0), len(ccu.writes))),
                                      ret(m, detach)),
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
    assert call(image.address('a733_video_platform_init'), decoder, config, image.address('platform_ops', UNIT), 0) == 1, \
        (name, 'the binding refused its configuration')
    # The init checks the pages, so no hold that follows walks a page table.
    assert state['translations'] == MAPPED_PAGES, (name, 'the init must check the pages', state['translations'])
    if decode == 'run':
        return decode_run_case(uc, image, layout, enums, state, put, get, ret, call, enter, ccu, others, vd, decoder,
                               unit_field)
    if row_wait:
        return row_wait_case(uc, image, layout, enums, state, put, get, ret, task_call=lambda: call(
            image.address('os_video_decode_task', UNIT), 0, stack=STACKS + 0x40000), ccu=ccu, others=others, vd=vd,
            at_deadline=row_wait == 'deadline')
    # The decoder as hold B's refusal left it; or, for a decode, started and prepared, its unit not failed.
    put(unit_field('failed'), 0 if decode else 1)
    for field, value in (('pll_inherited', PLL_INHERITED), ('pll_state', 1), ('pll_started', 1), ('pll_locked', 1),
                         ('pll_done', 1)):
        put(unit_field(field), value)
    if uncertain:
        # An uncertain access keeps the register mutex (a733_video_unit_refuse_held); an image before the
        # failure-cause flags has no uncertain member.
        put(unit_field('locked'), 1)
        if 'uncertain' in L['a733_video_unit']:
            put(unit_field('uncertain'), 1)
    for field, value in (('admitted', 1), ('requested', 1), ('cold_step', 4)) if not decode else \
            (('admitted', 1), ('requested', 1), ('cold_step', 5), ('checked', 1), ('prepared', 1)):
        put(decoder + L['a733_video_decoder'][field], value)
    put(vd + layout['initialized'], 1)
    if decode:
        # Masters 6 and 7 attached; the executor waiting for completion (timeout) or done (drained).
        put(vd + layout['attached'], 1)
        put(vd + layout['run.state'], enums['A733_VIDEO_JPEG_DONE' if decode == 'drained' else 'A733_VIDEO_JPEG_WAIT'])
        put(vd + layout['decoded'], 1)
    if decode == 'drained':
        put(vd + layout['dev.phase'], enums['OS_DEVICE_OPEN'])
    else:
        put(vd + layout['dev.phase'], enums['OS_DEVICE_FAULT'])
        step = 'VD_STEP_DECODE_TIMEOUT' if decode else 'VD_STEP_COLD'
        put(vd + layout['dev.fault_step'], enums[step]); put(vd + layout['dev.fault_value'], 0x204)
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
            shared = [(vd + layout['dev.' + n], 4) for n in ('phase', 'close_started', 'reported', 'quiesced')]
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
        'phase': get(vd + layout['dev.phase']), 'recorded_fault_step': get(vd + layout['dev.fault_step']),
        'close_answer': close_answer, 'record': record, 'schedule': schedule, 'events': events,
        'any_fault': QUIESCE['FAULT'] in results,
        'notes': [(hex(s), hex(v)) for s, v in state['notes']],
        'detaches': state['detaches'], 'gic_writes': [(hex(a), hex(v)) for a, v in others['gic'].writes],
        'reset_write': next((i for i, (a, v) in enumerate(ccu.writes) if a == 0xa8c and not v & 1 << 18), None),
        'ccu': {hex(k): hex(ccu.words.get(k, 0)) for k in (0x220, 0xa88, 0xa8c, 0x5e4)},
        'ccu_writes': [(hex(a), hex(v)) for a, v in ccu.writes],
        've_accesses': len(others['ve'].reads) + len(others['ve'].writes), 'translations': state['translations'],
    }


def decode_run_case(uc, image, layout, enums, state, put, get, ret, call, enter, ccu, others, vd, decoder, unit_field):
    """The service's decode, from an OPEN service whose decoder is started (as after run 6b3ec3f5's open), through
    the image's own do_decode, executor, binding and INTID 81 handler, then the application's close. The IOMMU owner
    and the allocator are answered by hooks; the decoder model completes at the trigger, and the planes are filled
    with the pinned reference at the stride the executor programs before the handler runs."""
    L = image.layouts
    spec = importlib.util.spec_from_file_location('vdec_check_host', Path(__file__).resolve().parents[1] /
                                                  'apps/vdec-check/host.py')
    host = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(host)
    fspec = importlib.util.spec_from_file_location('jpeg_fixture', Path(__file__).resolve().parents[1] /
                                                   'apps/jpeg/fixture.py')
    fixture = importlib.util.module_from_spec(fspec)
    fspec.loader.exec_module(fixture)
    jpeg, reference = fixture.nonconstant(), fixture.nonconstant_reference()
    uc.mem_map(REGIONS, 0x400000)
    slots = SCRATCH + 0x400
    for index, base in enumerate((REGIONS, REGIONS + 0x100000)):
        slot, f = slots + 0x80 * index, L['large_slot']
        put(slot + f['base'], base, 8); put(slot + f['bytes'], 0x100000, 8)
        put(slot + f['flags'], 1); put(slot + f['state'], 2)                # SS_LARGE_WRITE, SS_LARGE_READY
    uc.mem_write(REGIONS, jpeg)
    uc.mem_write(REGIONS + 0x100000, b'\x5a' * host.PIXELS)
    put(unit_field('failed'), 0); put(unit_field('token'), VD_OWNER, 8)
    for field, value in (('admitted', 1), ('requested', 1), ('cold_step', 5), ('checked', 1)):
        put(decoder + L['a733_video_decoder'][field], value)
    put(decoder + L['a733_video_decoder']['version'], VERSION_E0 << 32 | VERSION_E4, 8)
    put(vd + layout['initialized'], 1)
    put(vd + layout['dev.phase'], enums['OS_DEVICE_OPEN'])
    alloc = {'next': ARENA, 'iova': IOVA_BASE, 'spans': [], 'scans': []}

    def owner_memory(m):
        n, align = m.reg_read(UC_ARM64_REG_X0), m.reg_read(UC_ARM64_REG_X1)
        at = -(-alloc['next'] // align) * align
        alloc['next'] = at + n
        ret(m, at)

    def iova(m):
        n, align, out = (m.reg_read(r) for r in (UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2))
        at = -(-alloc['iova'] // align) * align
        alloc['iova'] = at + n
        put(out, at, 8)
        ret(m, 1)

    def attach(m):
        masters, spans, count = (m.reg_read(r) for r in (UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2))
        alloc['spans'] = [(masters, *(get(spans + 32 * i + 8 * k, 8) for k in range(3)), get(spans + 32 * i + 24))
                          for i in range(count)]
        ret(m, 1)

    def scan(m):
        alloc['scans'].append(m.reg_read(UC_ARM64_REG_X0))
        uc.mem_write(m.reg_read(UC_ARM64_REG_X1), bytes(40))
        ret(m, 1)

    for symbol, callback in (('os_large_slot', lambda m: ret(m, slots + 0x80 * m.reg_read(UC_ARM64_REG_X0))),
                             ('os_owner_memory_below', owner_memory), ('os_iommu_iova', iova),
                             ('os_iommu_attach', attach), ('os_iommu_scan', scan)):
        hook_addresses(uc, {image.address(symbol)}, lambda m, a, s, u, c=callback: c(m))
    job = vd + layout['job']
    for field, value, n in (('input_region', 0, 4), ('output_region', 1, 4), ('input_offset', 0, 8),
                            ('output_offset', 0, 8), ('input_bytes', len(jpeg), 4), ('output_bytes', host.PIXELS, 4)):
        put(job + layout['job.' + field] - layout['job'], value, n)
    put(vd + layout['op'], enums['DECODE_OP']); put(vd + layout['request'], enums['POSTED'])
    task = image.address('os_video_decode_task', UNIT)
    problems = []
    state['slice_done'] = False
    call(task, 0, stack=STACKS + 0x40000)
    ve, gic = others['ve'], others['gic']
    if not getattr(ve, 'triggered', False) or get(vd + layout['request']) == enums['DONE']:
        problems.append('the decode must block for INTID 81 after the trigger')
        return {'case': 'decode-runs', 'passed': False, 'problems': problems,
                'result': get(vd + layout['result']), 'request': get(vd + layout['request']),
                'run': (get(vd + layout['run.state']), get(vd + layout['run.index'])), 'waits': state['waits'][-3:],
                'pc': hex(uc.reg_read(UC_ARM64_REG_PC)), 'lr': hex(uc.reg_read(UC_ARM64_REG_LR)),
                've_writes': len(ve.writes), 'fault': (hex(get(vd + layout['dev.fault_step'])),
                                                                hex(get(vd + layout['dev.fault_value'])))}
    if not gic.words.get(0x108, 0) & 1 << 17:
        problems.append('INTID 81 must be enabled for the trigger')
    # The decoder's output: the reference planes at the programmed stride (luma), and Cb then Cr at half of it.
    spans = alloc['spans']
    luma, chroma = spans[1][2], spans[2][2]
    w, h, stride = 48, 32, 64
    for y in range(h):
        uc.mem_write(luma + y * stride, reference[y * w:(y + 1) * w])
    for plane in range(2):
        base = w * h + plane * 24 * 16
        for y in range(16):
            uc.mem_write(chroma + plane * stride * 32 // 4 + y * stride // 2, reference[base + y * 24:base + (y + 1) * 24])
    context = uc.context_save()
    call(image.address('os_video_decode_irq'), stack=STACKS + 0xc0000)
    uc.context_restore(context)
    state['slice_done'] = False
    ret(uc, 0)
    uc.emu_start(uc.reg_read(UC_ARM64_REG_PC), SENTINEL, count=5_000_000)
    output = bytes(uc.mem_read(REGIONS + 0x100000, host.PIXELS))
    done = {k: get(vd + layout['done.' + k]) for k in ('magic', 'width', 'height', 'output_bytes')}
    done_sha = bytes(uc.mem_read(vd + layout['done.sha256'], 32)).hex()
    result = get(vd + layout['result'])
    if get(vd + layout['request']) != enums['DONE'] or result != 1:
        problems.append(f"the decode must be answered 1: {result:#x}, fault "
                        f"{get(vd + layout['dev.fault_step']):#x} {get(vd + layout['dev.fault_value']):#x}")
    if hashlib.sha256(output).hexdigest() != host.REFERENCE_SHA256 or done_sha != host.REFERENCE_SHA256:
        problems.append('the copied active pixels must equal the reference, as must the answer\'s hash')
    if done != {'magic': host.DONE_MAGIC, 'width': 48, 'height': 32, 'output_bytes': host.PIXELS}:
        problems.append(f'answer {done}')
    want_spans = [(0xc0, a, None, 4096, access) for a, access in zip(host.IOVAS, (1, 2, 2))]
    if [(m_, i, n, a) for m_, i, _, n, a in spans] != [(m_, i, n, a) for m_, i, _, n, a in want_spans]:
        problems.append(f'masters 6 and 7 must map the three buffers at the predicted addresses: {spans}')
    if sorted(alloc['scans']) != [6, 7]:
        problems.append(f"the drain must scan masters 6 and 7 once each: {alloc['scans']}")
    if gic.words.get(0x108, 0) & 1 << 17:
        problems.append('the handler must mask INTID 81')
    info = vd + layout['info.decode']
    rec = {k: get(info + L['vdec_check_decode'][k]) for k in ('state', 'writes', 'irq_count', 'stray', 'seen', 'guard')}
    if rec != {'state': enums['A733_VIDEO_JPEG_DONE'], 'writes': 661, 'irq_count': 1, 'stray': 0, 'seen': 1, 'guard': 0}:
        problems.append(f'record {rec}')
    log = [(get(info + L['vdec_check_decode']['log'] + 8 * i), get(info + L['vdec_check_decode']['log'] + 8 * i + 4))
           for i in range(661)]
    decoder_writes = [(o, v) for o, v in ve.writes if o >= 0x100 and o not in (0x118,)]
    # The executor's writes as the decoder saw them: the trigger byte, then the disable and acknowledgment.
    if [o & 0xffffff for o, _ in log][-3:] != [0x118, 0x114, 0x11c]:
        problems.append('the log must end with the trigger, the disable and the acknowledgment')
    log_sha = hashlib.sha256(b''.join(struct.pack('<2I', o, v) for o, v in log)).hexdigest()
    # Then the application's close: the masters detached before the binding's close.
    detaches = state['detaches']
    put(vd + layout['op'], enums['CLOSE_OP']); put(vd + layout['request'], enums['POSTED'])
    state['slice_done'] = False
    call(task, 0, stack=STACKS + 0x40000)
    if get(vd + layout['result']) != 1 or get(vd + layout['dev.phase']) != enums['OS_DEVICE_CLOSED']:
        problems.append('the close after the decode must be answered 1 and end CLOSED')
    if [m_ for m_, _ in detaches] != [0xc0] or detaches[0][1] != 0:
        problems.append(f'the close must detach masters 6 and 7 before any CCU write: {detaches}')
    return {'case': 'decode-runs', 'passed': not problems, 'problems': problems, 'spans': [
        [hex(x) for x in s_] for s_ in spans], 'log_sha256': log_sha, 'decoder_writes': len(decoder_writes),
        'log': [[o & 0xffffff, o >> 24, v] for o, v in log],
        'output_sha256': hashlib.sha256(output).hexdigest(), 'ccu_writes': [(hex(a), hex(v)) for a, v in ccu.writes]}


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
    domains = providers + L['board_provider']['domains']
    put(domains + L['a733_power_domains']['pmic'], pmic, 8)
    # The owner's configured tick, as apps/board/main.c sets it (1000000 / configTICK_RATE_HZ): the row deadlines read it.
    put(domains + L['a733_power_domains']['config'] + L['a733_power_service_config']['tick_us'], TICK_US)
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
    if get(vd + layout['dev.phase']) != enums['OS_DEVICE_CLOSED'] or get(vd + layout['dev.fault_step']) != enums['VD_STEP_ROW_TIMEOUT']:
        problems.append('the service must end CLOSED with step VD_STEP_ROW_TIMEOUT')
    if ccu.writes or others['ve'].reads or others['ve'].writes:
        problems.append('an unanswered row leaves nothing written and no close')
    if state['translations'] != MAPPED_PAGES:
        problems.append(f"the pages are checked once, {MAPPED_PAGES} translations: {state['translations']}")
    unit = vd + layout['platform.decoder'] + L['a733_video_decoder']['unit']
    if get(unit + L['a733_video_unit']['deadline'], 8) or get(unit + L['a733_video_unit']['failed']):
        problems.append('the refused open must leave the binding with no deadline and not failed, so a reopen runs')
    return {'case': 'row-wait-deadline' if at_deadline else 'row-wait', 'waits_us': rows, 'passes': len(state['waits']), 'elapsed_us': state['time'],
            'phase': get(vd + layout['dev.phase']), 'recorded_fault_step': get(vd + layout['dev.fault_step']),
            'ccu_writes': [(hex(a), hex(v)) for a, v in ccu.writes], 'problems': problems, 'passed': not problems,
            'notes': [(hex(s), hex(v)) for s, v in state['notes']]}


def record_layout(image):
    """The board record's offsets in apps/vdec-check/host.py against the image's DWARF: a list of differences."""
    spec = importlib.util.spec_from_file_location('vdec_check_host', Path(__file__).resolve().parents[1] /
                                                  'apps/vdec-check/host.py')
    host = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(host)
    L, problems = image.layouts, []
    nested = {'cold': 'a733_video_cold_record', 'transition': 'a733_power_transition', 'decode': 'vdec_check_decode'}

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
    compare(host.DONE, 'ss_video_decode_done')
    r = L['video_decode_check_result']
    if (r['done'], r['info'], r['pixels']) != (host.HEADER.size, host.HEADER.size + host.DONE_SIZE,
                                               host.HEADER.size + host.DONE_SIZE + host.RECORD_SIZE):
        problems.append('the result is not the 64-byte header, the answer, the record and the pixels')
    # The runner writes the decoder's answer into result.json, so a decoded result must serialize as JSON.
    payload = bytearray(host.SIZE)
    struct.pack_into('<2I', payload, 0, host.CHECK_MAGIC, 2)
    try:
        json.dumps(host.decode(bytes(payload)))
    except (TypeError, ValueError) as error:
        problems.append(f'host.py decode() output does not serialize as JSON: {error}')
    if not host.fixture_header_matches(Path(__file__).resolve().parents[1]):
        problems.append('apps/vdec-check/fixture.h differs from apps/jpeg/fixture.py')
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
    assert layout and enums['OS_DEVICE_CLOSED'] == 0 and enums['OS_DEVICE_FAULT'] == 4
    cases = [('quiesce-closes', {}), ('application-closes', {'application_close': True}),
             ('close-step-fails', {'stuck': {0xa8c: 0x00040004}}), ('uncertain-access', {'uncertain': True})]
    rows, failures = [], []
    problems = record_layout(image)
    rows.append({'case': 'record-layout', 'passed': not problems, 'problems': problems})
    if problems:
        failures.append('record-layout')
    # A decode that timed out after the trigger: the close masks INTID 81, asserts reset through the binding's close,
    # and only then returns masters 6 and 7 to bypass; a drained decode detaches them before any CCU write; a detach
    # that fails leaves the service FAULT.
    for name, options in (('decode-timeout-closes', {'decode': 'timeout'}),
                          ('decode-drained-closes', {'decode': 'drained', 'application_close': True}),
                          ('decode-detach-fails', {'decode': 'timeout', 'detach': -1})):
        row = run_case(image, parts, layout, enums, name, **options)
        problems = []
        if [m for m, _ in row['detaches']] != [0xc0]:
            problems.append(f"masters 6 and 7 must be detached once: {row['detaches']}")
        elif options['decode'] == 'drained':
            if row['detaches'][0][1] != 0:
                problems.append('a drained decode detaches before the binding close')
        elif row['reset_write'] is None or row['detaches'][0][1] <= row['reset_write']:
            problems.append('after a fault the masters return to bypass only after reset is asserted')
        if ('0x188', hex(1 << 17)) not in row['gic_writes']:
            problems.append('the close must mask INTID 81 (GICD_ICENABLER2 bit 17)')
        if name == 'decode-detach-fails':
            if row['done_within_bound'] or not row['notes'] or \
                    any(int(s, 16) & 0xfff != SITE_DECODER for s, _ in row['notes']):
                problems.append('a failed detach must leave the service FAULT with site 0x941 notes')
        else:
            if not row['done_within_bound'] or row['notes'] or row['phase'] != enums['OS_DEVICE_CLOSED']:
                problems.append('the service must close, with no fault note, and the registry reach DONE')
            if name == 'decode-timeout-closes' and row['recorded_fault_step'] != enums['VD_STEP_DECODE_TIMEOUT']:
                problems.append('the first fault must be kept')
            if name == 'decode-drained-closes' and row['close_answer'] != 1:
                problems.append('the close of a drained decode must be answered 1')
        if row['ve_accesses']:
            problems.append('the close must not touch a decoder register')
        row['passed'] = not problems
        row['problems'] = problems
        if problems:
            failures.append(name)
        rows.append(row)
    row = run_case(image, parts, layout, enums, 'decode-runs', decode='run')
    rows.append(row)
    if not row['passed']:
        failures.append('decode-runs')
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
            if row['phase'] != enums['OS_DEVICE_CLOSED'] or row['recorded_fault_step'] != enums['VD_STEP_COLD']:
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
        if row['any_fault'] or row['notes'] or not row['done_within_bound'] or row['phase'] != enums['OS_DEVICE_CLOSED']:
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
