#!/usr/bin/env python3
"""The returning SD0 read image's actual setup, SD task and report under the SD card models.

usage: storage-read-returning.py --image ELF --board-config CONFIG --output NEW_DIRECTORY

Runs, under Unicorn, the linked image's sdrt_setup(), sdrt_bind(), sdrt_step()
until it ends, sdrt_quiet() and sdrt_report() for each case, with the card
and register models of checks/storage-read-native.py,
checks/storage-read-service.py and checks/storage-read-sd-modes.py (the UHS-I
card and PF bank selector for profile 3), and the same provider, PMIC, pin,
rail, translation, scheduler and time hooks those checks use. The input is
the one tools/storage-read-returning.py packs from a configuration and the
supervised checker's throughput plan. Each case's log goes through the host
decoder. The cases use FIXTURE, the 24 MHz fabric configuration of the
supervised checks' fixtures, except board-3, which uses CONFIG, the port 3-3
configuration that the board run's input carries, with the CCU words that
port 3-3 read on 2026-10-25.

Cases:
- profile-2: high speed at 50 MHz, all 256 reads, each read's CRC-32 equal to
  that of the model's payload, the throughput summary computed;
- profile-3: S18R, CMD11, one PF selector write, SDR104 at 150 MHz, the
  64-delay CMD19 sweep, then the same 256 reads;
- window: the clock passes the plan's end after 10 reads; the image stops
  starting reads and reports 10 of 256 with "INCOMPLETE ... window";
- absent: card detect reports no card; the provider stops and the report
  says where;
- crc-identify: a data CRC error on the CMD6 switch status during
  identification (the supervised checks' crc-fault model); the provider stops
  and retains the register lock and the report says where;
- crc-read: a data CRC error on the first read; the same;
- not-parked: the PMIC service has not parked; sdrt_quiet() refuses, so the
  image would exit with status 1;
- board-3: profile 3 with CONFIG and the fabric configuration the host tool
  derives from port 3-3's pre-launch read (0x03000002, 0xc3000000,
  0x84000000), with the CCU words the image then finds: the MBUS word as the
  monitor's restore leaves it (0x81000000), and PERI0 PLL 0xff126310, gates
  0x8fff0fff, status 0x0fff0000: SDR104 at 150 MHz and all 256 reads;
- board-mbus-skip: the same configuration, with the monitor's restore
  skipped (MBUS still 0x84000000): the SD0 provider refuses the fabric word
  and the image stops with the reason in its report;
- board-old-fabric: CONFIG with the fabric words of lifetime A's session
  template (0x03000002, 0xc3000000, 0x81000000): sdrt_setup() refuses it,
  because bit 30 of 0xc3000000 is outside every provider's fabric mask;
- stall-update: profile 2, then the clock-update command never completes;
  the image gates the module clock instead (result 3);
- stall-reset: the crc-read stop in the middle of a read, then the FIFO and
  DMA interface reset never clears; the image issues no clock update and
  gates the module clock (result 3).

Every case then runs sdrt_clock_off(), the image's last step before its own
exit, and requires the vendor driver's order: FIFO and DMA interface reset
when a read was outstanding, CLKCR with the card clock off and MASK_DATA0
set, the clock-update command, RINT written back, and MASK_DATA0 cleared
(result 1); the module clock gate after a stalled update (result 3); and no
write where the controller was never enabled (result 2).

No board, USB or queue access.
"""
import argparse
import binascii
import hashlib
import importlib.util
import json
import struct
import sys
from pathlib import Path
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import (UC_ARM64_REG_X0, UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_LR,
                                 UC_ARM64_REG_PSTATE, UC_ARM64_REG_CP_REG)

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'checks'))
import harness  # noqa: E402


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec); spec.loader.exec_module(value); return value


modes = module('sd_returning_modes', ROOT / 'checks/storage-read-sd-modes.py')
service = modes.service
host = module('sd_returning_host', ROOT / 'tools/storage-read-returning.py')
LAYOUTS = ('sdrt', 'a733_mmc_platform', 'sunxi_mmc', 'sunxi_mmc_card', 'a733_pmic', 'a733_pmic_slot',
           'a733_pmic_snapshot', 'a733_platform_locks')
HOOKS = ('a733_counter_clock_now', 'sdrt_ticks', 'aarch64_device_translate',
         'xTaskGetCurrentTaskHandle', 'xTaskGetSchedulerState', 'xQueueGetMutexHolder', 'xQueueCreateMutexStatic',
         'vTaskDelay', 'a733_platform_lock_enter', 'a733_platform_lock_leave', 'a733_platform_lock_retain',
         'a733_rail_acquire', 'a733_rail_poll', 'a733_rail_status', 'a733_rail_retain', 'a733_pmic_identity',
         'a733_pmic_live', 'a733_pmic_submit', 'a733_pmic_result', 'a733_pmic_reap', 'a733_pins_acquire_banked',
         'a733_pins_held_banked', 'a733_pins_read_inputs_banked', 'platform_puts', 'platform_hex', 'platform_exit')
STOP = 0x50fff000
# The fabric and pins of the supervised checks' fixture session
# (checks/storage-read-fixture.py): 24 MHz fabric, PF inputs, SD route.
FIXTURE = {'fabric': [0, 0x80000000, 0x80000000], 'fabric_hz': [24000000] * 3,
           'pmic_pins': [[2, 1, 1], [2, 1, 1]], 'expected_pins': [[0, 0, 0]] * 7,
           'desired_pins': [[2, 1, 0]] * 6 + [[0, 1, 0]]}
# CCU words port 3-3 read: PERI0 PLL, gates and status on 2026-10-25
# (board-session-20261025-3/port-3-3/bb9-read, board-session-20261025-7/port-3-3/ts-read-0
# to -3), and the AHB, NSI and MBUS fabric words 0x500, 0x580 and 0x588
# (board-session-20261023/port-3-3/r9-v4-capture). 0x580 reads 0xc3000000; bit
# 30 lies outside the providers' fabric mask, so a configuration carries 0x83000000.
PORT_3_3 = {0x020020a0: 0xff126310, 0x02003908: 0x8fff0fff, 0x02003988: 0x0fff0000,
            0x02002500: 0x03000002, 0x02002580: 0xc3000000, 0x02002588: 0x84000000}


class Card(modes.UhsMachine):
    """The UHS-capable card, PF bank and controller models of the supervised SD0 checks, driving this image."""

    def __init__(self, image, profile, variant='nominal'):
        # The models' state, set as their constructors set it; the board
        # image's fields that those constructors poke do not exist here.
        self.image = image; self.variant = variant; self.profile = profile
        self.cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM); self.cpu.reg_write(UC_ARM64_REG_PSTATE, 5)
        # CNTFRQ_EL0 at the board's 24 MHz, which the counter clock and the
        # GPU resource adapter check at initialization.
        self.cpu.reg_write(UC_ARM64_REG_CP_REG, (14, 0, 3, 3, 0, 24000000))
        for a, n in [(0x02000000, 0x06000000), (0x40000000, 0x04000000), (0x50000000, 0x01000000)]:
            self.cpu.mem_map(a, n)
        for a, data in image.parts: self.cpu.mem_write(a, data)
        for name in ('ullCriticalNesting', 'ullPortInterruptNesting'):
            self.cpu.mem_write(image.address(name), bytes(8))
        self.time = 1000000; self.held = False; self.retained = False; self.finished = False
        self.trace = []; self.commands = []; self.fifo = self.total = self.data_command = self.state = self.ocr = 0
        self.present = True; self.byte_addressed = False; self.first_sector = 0
        self.width = 1; self.card_hs = 0; self.mode_app = False; self.mode_data = bytes(); self.module_changes = []
        self.phase = 0; self.bank_reads = None; self.pf_writes = 0; self.cmd19 = 0; self.sdr = 0
        self.log = ''; self.exit = None; self.mutexes = 0; self.window_after = None; self.final = False
        self.regs = {0x07090160: (1 << 28) | 2, 0x02002000: 0xf8000000 | (99 << 8) | (99 << 16), 0x02002580: 1 << 31,
                     0x02002588: 1 << 31, 0x020025c0: 1 << 24, 0x020025e0: 1 << 30, 0x0200259c: 0x30001,
                     0x02000040: 2 << 10, 0x02000048: 2 << 10, 0x02000070: 1, 0x04020300: 0x03050530,
                     0x0402005c: 0x81710331, 0x0402010c: 0x80001234, 0x04020054: 0x12345676, 0x04020058: 0xffff,
                     0x04020104: 7, 0x04020140: 0xa5a40000,
                     0x020020a0: 0xfe000000 | (99 << 8) | (1 << 20), 0x02003908: ((1 << 11) | (1 << 2) | (1 << 1)) * 0x10001,
                     0x02003988: ((1 << 11) | (1 << 2) | (1 << 1)) << 16}
        if profile == 3:
            self.regs[0x02000040] = 1 << 10  # adaptive PF withstand mode, as port 3-3 reports
            gates = (1 << 11) | (1 << 9) | (1 << 5) | (1 << 4) | (1 << 2) | (1 << 1)
            self.regs.update({0x02003908: gates * 0x10001, 0x02003988: gates << 16})
        csd = (1 << 126) | (9 << 80) | ((5 | (1 << 10)) << 84) | (0x7fff << 48) | (0x32 << 96)
        self.csd = [(csd >> (96 - 32 * i)) & 0xffffffff for i in range(4)]; self.cid = [0x12345678 + i for i in range(4)]
        self.hooks = {image.address(n): n for n in HOOKS}
        self.base = image.address('sdrt'); self.layout = image.layouts['sdrt']
        self.cpu.hook_add(UC_HOOK_CODE, self.code)
        self.cpu.hook_add(UC_HOOK_MEM_READ, self.read_mmio, begin=0x02000000, end=0x07ffffff)
        self.cpu.hook_add(UC_HOOK_MEM_WRITE, self.write_mmio, begin=0x02000000, end=0x07ffffff)

    def string(self, a):
        out = bytearray()
        while (b := self.cpu.mem_read(a + len(out), 1)[0]): out.append(b)
        return out.decode()

    def code(self, cpu, a, size, unused):
        if a == STOP: self.finished = True; cpu.emu_stop(); return
        name = self.hooks.get(a)
        if not name: return
        args = [cpu.reg_read(UC_ARM64_REG_X0 + i) for i in range(8)]
        if name == 'a733_counter_clock_now': self.write(args[1], self.time, 8); self.ret(1)
        # Model time advances 10 us at each counter read, so a read's
        # first-poll-to-completion time counts its polls and is never zero.
        elif name == 'sdrt_ticks': self.time += 10; self.ret(self.time * 24)
        elif name == 'aarch64_device_translate':
            self.cpu.mem_write(args[2], struct.pack('<Q2I', args[0], 0xff if args[0] >= 0x40000000 else 0,
                                                    3 if args[0] >= 0x40000000 else 2)); self.ret(1)
        elif name == 'xTaskGetCurrentTaskHandle': self.ret(1)
        elif name == 'xTaskGetSchedulerState': self.ret(2)
        elif name == 'xQueueGetMutexHolder': self.ret(1 if self.held else 0)
        elif name == 'xQueueCreateMutexStatic': self.mutexes += 1; self.ret(0x100 + self.mutexes)
        elif name == 'vTaskDelay': raise AssertionError('the SD step or report waited')
        elif name == 'platform_puts': self.log += self.string(args[0]); self.ret()
        elif name == 'platform_hex': self.log += f'0x{args[0]:016x}'; self.ret()
        elif name == 'platform_exit': self.exit = args[0]; self.finished = True; cpu.emu_stop()
        elif name == 'a733_platform_lock_enter':
            assert not self.held and not self.retained; self.held = True; self.ret(1)
        elif name == 'a733_platform_lock_leave': assert self.held; self.held = False; self.ret()
        elif name == 'a733_platform_lock_retain': assert self.held; self.held = False; self.retained = True; self.ret(1)
        elif name == 'a733_rail_acquire': self.cpu.mem_write(args[5], struct.pack('<4Q', 1, args[1], args[2], args[3])); self.ret(1)
        elif name in ('a733_rail_poll', 'a733_rail_status'): self.ret(0)
        elif name == 'a733_rail_retain': self.ret(5)
        elif name == 'a733_pmic_identity':
            self.cpu.mem_write(args[1], struct.pack('<3Q', self.base + self.layout['pmic'], 10, 1)); self.ret(1)
        elif name == 'a733_pmic_live': self.ret(1)
        elif name == 'a733_pmic_submit': self.pending_rail = args[3]; self.write(args[5] + 24, 1, 8); self.ret(0)
        elif name == 'a733_pmic_result':
            p = args[2]; pl = self.image.layouts['a733_pmic_snapshot']; self.cpu.mem_write(p, bytes(24))
            for f, v in [('rail', self.pending_rail), ('chip', 3), ('enabled', 1), ('decoded', 1),
                         ('programmed_uv', 1800000 if self.pending_rail == 24 else 3300000)]:
                self.write(p + pl[f], v, 1 if f == 'chip' else 4)
            self.ret(2)
        elif name == 'a733_pmic_reap': self.ret(2)
        elif name == 'a733_pins_acquire_banked': self.cpu.mem_write(args[4], struct.pack('<2Q', 1, args[1]) + bytes(52)); self.ret(0)
        elif name == 'a733_pins_held_banked': self.write(args[2], 1); self.ret(0)
        elif name == 'a733_pins_read_inputs_banked': self.write(args[4], 0 if self.present else 1 << 6); self.ret(0)
        else: raise AssertionError(name)

    # Profile 3 uses the UHS-I card; profile 2 the high-speed card of the same check.
    def read_mmio(self, cpu, access, a, size, value, unused):
        (modes.UhsMachine if self.profile == 3 else modes.Machine).read_mmio(self, cpu, access, a, size, value, unused)
        if self.variant in ('crc-read', 'crc-read-stall-reset') and a == 0x04020038 and self.data_command in (17, 18) and self.fifo >= 64:
            v = self.read(a) | (1 << 7); self.write(a, v); self.trace[-1][2] = v  # data CRC error

    def write_mmio(self, cpu, access, a, size, value, unused):
        if self.final and a == 0x04020018 and value & (1 << 21) and self.variant == 'stall-update':
            self.trace.append(['write', a, value]); self.regs[a] = value; return  # START never clears
        if self.final and a == 0x04020000 and value & 6 and self.variant == 'stall-reset':
            self.trace.append(['write', a, value]); self.regs[a] = value; return  # the reset bits never clear
        if self.final and a == 0x02002d00:
            self.trace.append(['write', a, value]); self.regs[a] = value; return  # module clock gate
        (modes.UhsMachine if self.profile == 3 else modes.Machine).write_mmio(self, cpu, access, a, size, value, unused)

    def field(self, name, n=4): return self.read(self.base + self.layout[name], n)

    def call(self, name, *args):
        self.finished = False; self.cpu.reg_write(UC_ARM64_REG_SP, 0x50ffeff0); self.cpu.reg_write(UC_ARM64_REG_LR, STOP)
        for i, v in enumerate(args): self.cpu.reg_write(UC_ARM64_REG_X0 + i, v)
        self.cpu.emu_start(self.image.address(name), 0, count=20000000)
        assert self.finished, (name, hex(self.cpu.reg_read(UC_ARM64_REG_PC)))
        value = self.cpu.reg_read(UC_ARM64_REG_X0) & 0xffffffff
        return value - (1 << 32) if value >> 31 else value


def run_case(image, raw, profile, variant, registers=None):
    m = Card(image, profile, variant)
    if registers is not None:
        m.regs.update(registers)
    m.cpu.mem_write(0x43000000, raw)
    assert m.call('sdrt_setup') == 1, 'setup refused the input'
    assert m.call('sdrt_bind', 2) == 1, 'PMIC bind failed'
    if variant == 'absent': m.present = False
    if variant == 'crc-identify': m.variant = 'crc-fault'
    start = m.field('start', 8)
    steps = 0
    ticks = 0
    while True:
        r = m.call('sdrt_step'); steps += 1; ticks += r == 0
        assert not m.held, 'step returned with the register lock held'
        if r in (1, -1): break
        if r == 0: m.time += 1000  # one scheduler tick
        if variant == 'window' and m.field('done') == 10 and m.window_after is None:
            m.window_after = m.time; m.time = start + 1550000
        assert steps < 20000, (m.field('stage'), m.field('reason'))
    m.ticks = ticks
    m.write(m.base + m.layout['pmic_parked'], 0 if variant == 'not-parked' else 1)
    quiet = m.call('sdrt_quiet')
    # The final quiesce writes the controller without the register lock,
    # after every provider has stopped; waive the model's lock assertion.
    before = len(m.trace); m.held = True; m.final = True
    if variant == 'stall-update-final': m.variant = 'stall-update'
    if variant == 'crc-read-stall-reset': m.variant = 'stall-reset'
    clock_off = m.call('sdrt_clock_off'); m.held = False; m.final = False
    m.write(m.base + m.layout['clock_off'], clock_off)
    m.clock_writes = m.trace[before:]
    m.call('sdrt_report', 0 if r > 0 else 1)
    return m, r, quiet, steps


def run(elf, board, out):
    out.mkdir(parents=True, exist_ok=False)
    image = harness.Image(elf, LAYOUTS)
    for name in HOOKS + ('sdrt_setup', 'sdrt_bind', 'sdrt_step', 'sdrt_quiet', 'sdrt_report', 'sdrt'):
        image.address(name)
    checker = host.throughput()
    plan = checker.throughput_plan(0, 4194304)
    results = {}
    for name, profile, variant in [('profile-2', 2, 'nominal'), ('profile-3', 3, 'nominal'), ('window', 2, 'window'),
                                   ('absent', 2, 'absent'), ('crc-identify', 2, 'crc-identify'),
                                   ('crc-read', 2, 'crc-read'), ('not-parked', 2, 'not-parked'),
                                   ('board-3', 3, 'nominal'), ('board-mbus-skip', 2, 'nominal'),
                                   ('stall-update', 2, 'stall-update-final'),
                                   ('stall-reset', 2, 'crc-read-stall-reset')]:
        registers = dict(PORT_3_3)
        if name.startswith('board-'):
            (fabric, fabric_hz) = host.fabric(host.restored([PORT_3_3[a] for a in host.FABRIC]))
            config = dict(board, fabric=fabric, fabric_hz=fabric_hz)
            if name == 'board-3': registers[0x02002588] = host.MBUS_RESTORED
        else:
            config = FIXTURE
        raw = host.input_bytes(profile, 0x1234567, config, plan, ids=(7, 9, 11))
        m, r, quiet, steps = run_case(image, raw, profile, variant, registers if name.startswith('board-') else None)
        (out / f'{name}.log').write_text(m.log)
        value = host.decode_log(m.log, raw, 0 if r > 0 and quiet and m.field('done') == len(plan) else 1)
        reads = value['reads']
        for item in reads:
            assert item['crc32'] == binascii.crc32(service.payload(item['first'], item['blocks'])), item
        writes = [(a, v) for kind, a, v in m.clock_writes if kind == 'write']
        stalled = r == -1 and value['words'].get('SD_STAGE') in (4, 5)
        if value['words']['SD_CLOCK_OFF'] == 2:
            # Stopped before the controller's clock, bus gate and reset were enabled.
            assert name in ('absent', 'board-mbus-skip') and not writes, name
        else:
            expected = [0x04020000] if stalled else []
            if name == 'stall-reset':
                expected += [0x02002d00]
            else:
                expected += [0x04020004, 0x04020018, 0x04020038]
                expected += [0x02002d00] if name == 'stall-update' else [0x04020004]
            assert [a for a, _ in writes] == expected, (name, [hex(a) for a, _ in writes])
            if stalled: assert writes[0][1] & 6 == 6
            if name != 'stall-reset':
                clkcr = writes[1 if stalled else 0][1]
                assert clkcr & (1 << 31) and not clkcr & (3 << 16) and writes[1 + stalled][1] == 0x80202000
            if name in ('stall-update', 'stall-reset'):
                assert value['words']['SD_CLOCK_OFF'] == 3 and not writes[-1][1] & (1 << 31)
            else:
                assert value['words']['SD_CLOCK_OFF'] == 1 and not writes[-1][1] & ((1 << 31) | (1 << 16)), name
        if name in ('profile-2', 'profile-3', 'board-3', 'stall-update'):
            assert r == 1 and quiet == 1 and value['complete'] and len(reads) == 256, (name, r, quiet, value['outcome'])
            card = value['card']
            assert card['sectors'] == 33554432 and card['bus_width'] == 4
            assert card['card_hz'] == (50000000 if profile == 2 else 150000000)
            if name.startswith('board-'):
                assert host.parse_input(raw)['profile'] == profile and struct.unpack_from('<3I', raw, 72) == tuple(config['fabric'])
            assert card['timing'] == (1 if profile == 2 else 3)
            assert value['first_sector_hex'] == service.payload(0, 1).hex()
            assert set(value['throughput']) >= {'sequential', 'random', 'single'}, value['throughput']
            if profile == 3:
                assert m.pf_writes == 1 and m.phase == 3 and m.sdr == 3 and m.cmd19 == 12 * 20 + 52
                assert (card['uhs_outcome'], card['signal_1v8'], card['tuning_start'], card['tuning_width'],
                        card['sample_delay']) == (1, True, 20, 12, 26), card
            else:
                assert m.pf_writes == 0 and m.card_hs == 1
            assert not any(a == 0x04020018 and v & (1 << 10) for kind, a, v in m.trace if kind == 'write')
        elif name == 'window':
            assert r == 1 and len(reads) == 10 and value['outcome'] == 'window' and not value['complete']
        elif name == 'absent':
            assert r == -1 and not reads and value['outcome'] == 'stopped' and value['words']['SD_REASON'] == 3
        elif name == 'crc-identify':
            assert r == -1 and not reads and value['outcome'] == 'stopped' and value['words']['SD_REASON'] == 3
            assert value['words']['SD_PLATFORM_FAULT'] == 1 and m.retained
        elif name in ('crc-read', 'stall-reset'):
            assert r == -1 and not reads and value['outcome'] == 'stopped' and value['words']['SD_REASON'] in (6, 7)
            assert value['words']['SD_PLATFORM_FAULT'] == 1 and m.retained and value['card'] is not None
        elif name == 'board-mbus-skip':
            assert r == -1 and not reads and value['outcome'] == 'stopped' and value['words']['SD_PLATFORM_FAULT'] == 1
        elif name == 'not-parked':
            assert r == 1 and quiet == 0 and len(reads) == 256
        results[name] = {'passed': True, 'steps': steps, 'tick_waits': m.ticks,
                         'model_elapsed_us': m.time - m.field('start', 8), 'commands': len(m.commands), 'mmio_accesses': len(m.trace),
                         'reads': len(reads), 'outcome': value['outcome'], 'quiet': quiet,
                         'pf_selector_writes': m.pf_writes, 'log_bytes': len(m.log.encode()),
                         'throughput': value.get('throughput')}
        assert len(m.log.encode()) <= 16127 - 200, 'log does not fit the result page with the start lines'
    # Lifetime A's template words: 0xc3000000 has bit 30 outside the fabric mask.
    old = dict(board, fabric=[0x03000002, 0xc3000000, 0x81000000], fabric_hz=[200000000, 600000000, 600000000])
    m = Card(image, 2); m.regs.update(PORT_3_3)
    m.cpu.mem_write(0x43000000, host.input_bytes(2, 0x1234567, old, plan, ids=(7, 9, 11)))
    assert m.call('sdrt_setup') == 0 and m.call('a733_mmc_platform_scopes', m.base + m.layout['config'], 0x50031000) == 0
    results['board-old-fabric'] = {'passed': True, 'setup_refused': True}
    (out / 'result.json').write_text(json.dumps({
        'passed': True, 'physical_access': False, 'image_sha256': hashlib.sha256(Path(elf).read_bytes()).hexdigest(),
        'cases': results,
        'scope': ('Actual linked input copy, provider construction, SD0 task steps (controller, platform, native '
                  'aperture filter, clock leases, UHS-I PF selector write and tuning), quiescence check and report; '
                  'rail, pin, PMIC, translation, scheduler, time and card/MMIO modeled as in the supervised SD0 '
                  'checks. No monitor, scheduler start or physical qualification.')}, indent=1) + '\n')
    print('PASS returning SD0 read image: profiles 2 and 3, window, absent card, CRC fault and PMIC parking')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ('image', 'board_config', 'output'): parser.add_argument('--' + name.replace('_', '-'), type=Path, required=True)
    a = parser.parse_args()
    run(a.image, json.loads(a.board_config.read_text()), a.output)
