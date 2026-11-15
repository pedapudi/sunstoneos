#!/usr/bin/env python3
"""Execute the linked G2D operation path of the OS image (apps/os/g2d.c, docs/g2d-os.md#operations) under AArch64 emulation.

The owner task's serve and do_run code runs as linked in the image, with the RCQ command builder
(a733_g2d_commands_at) and the CPU cache maintenance (aarch64_device_cache, whose argument checks run as on the board; its CurrentEL, SCTLR_EL1 and CTR_EL0 reads return
the board's values).
The IOMMU owner calls, the large-region slots, the clocks, the G2D registers and the engine are modeled: a launch
(0x28 = 1) sets task end and cfg_finish in 0x24, and the owner's wait runs the image's own interrupt handler,
os_g2d_irq. Writing one to 0x24 clears task end (bit 0) only: cfg_finish (bit 2) stays set once a job has set it.
Board run 1794722460072814405-2b77c3eb (port 3-1, stage 3) measured that: after the first copy 0x24 read 0x4 through
the handler's acknowledgment, the mixer reset pulse and a write of 5, and the second copy's launch check read 0x4. On
that run the update bit (0x28 bit 0) read 0 after the job.

Cases:
- surfaces: every surface byte size and start the stage 2, 3 and 4 plans draw. The plans' rows are a whole number of
  4-byte words (stride = width times bytes per pixel, rounded up to 4, plus 64), so the cases take every start offset
  0, 4, ..., 60 within a 64-byte line and every row-total size class modulo 64, at the widths and heights the plans
  use (320 to 640 by 180 to 360), with the stage-2 surface, 420 by 271 at stride 1744, first. Each must complete with
  result 1, one interrupt, the vendor launch sequence, and two clean-and-invalidate calls over whole 64-byte lines
  that cover the surface and stay inside its region.
- cache-refusal: the first cache maintenance refuses. The operation fails at step 59 (RUN_CACHE) with no launch write,
  and the service stays open, so the application's close can run (this check does not run the close).
- timeout: the engine never sets task end. The operation fails at step 62 (RUN_TIMEOUT) and the service is FAULT,
  which retains its gates, leases and mapping.
- Close after a fault (docs/g2d-os.md#operations): the application's close of the FAULT service runs through the image's
  own close with the CCU and PCK1 registers modeled. The close must stop the engine (0x28 update off, the 0x08 AHB
  reset pulse) and read it idle before master 10 is detached, and then gate and reset the engine.
  - timeout-close: after the timeout the engine is still running (0x28 bit 0 and 0x100 bit 31 set) until the reset
    pulse. The close returns 1, the service is CLOSED, and the fault record keeps step 62.
  - timeout-stuck-close: the engine still reads running after the reset pulse. The close fails, the service stays
    FAULT with step 62 kept, master 10 is not detached and the bus gate stays open.
  - launch-fault-close: 0x24 bit 0 (task end) does not clear, so the launch fails at step 61 (RUN_LAUNCH); the close
    succeeds.
  - attach-fault-close: the first attach faults after the IOMMU owner recorded master 10 attached. The operation
    fails at step 57 (RUN_ATTACH) and the close detaches master 10, although the service never saw the attach
    complete.
  An image built before the close of a faulted service fails the four close cases: its close refuses a FAULT service.
- Stage 3 (blits-update-sticks, blits-update-clears): the two copies of the stage-3 plan in one session, from region
  1 to region 0, the first converting ARGB8888 to BGRA8888. One attach maps the output read-write, the source
  read-only and the command page, in that order; the second operation maps nothing. Each operation writes 0x28 = 0
  before its idle check (the vendor order), its command page equals the image's own builder's page for the plan and
  the expected device addresses, the output's lines are cleaned and invalidated and the source's cleaned in whole
  lines, and the record holds the prefetch words once, the 0x24 word before each launch (0 for the first, 4 with
  cfg_finish left set for the second) and the after-operation 0x24, 0x28 and 0x100 words. The engine model leaves the
  update bit set after a job in the first case and clears it in the second, as the board did. The stage-2 image fails
  both cases (it refuses a blit) and every surface case (it reads 0x28 before writing it). The first stage-3 image
  (04f243dbf) fails both cases at the second copy's launch, step 61 with value 4, as on the board.
- fills-update-sticks: two stage-2 fills in one session with an engine that leaves 0x28 bit 0 set after a job. Both
  must complete. The stage-2 image fails the second at step 60 (RUN_BUSY), because it reads 0x28 before writing it.

Usage: os-g2d-operation.py --image ELF --output DIR"""
import argparse
import hashlib
import json
import struct
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import UC_HOOK_MEM_UNMAPPED
from unicorn.arm64_const import (UC_ARM64_REG_PC, UC_ARM64_REG_LR, UC_ARM64_REG_SP, UC_ARM64_REG_X0,
                                 UC_ARM64_REG_X1, UC_ARM64_REG_X2)
from unicorn import arm64_const
from harness import machine as harness_machine, Image, hook_addresses, instruction_sites, save, skip_crumbs

GATE_RUNS = [['--image', '{elf:ret-g2d}', '--output', '{output}']]
UNIT = '/apps/os/g2d.c'
SURFACE, SOURCE, REGION_BYTES = 0x60000000, 0x60200000, 0x200000
RCQ, OWNER, SLOT, SLOT1, STACK = 0x60400000, 0x60500000, 0x60502000, 0x60502100, 0x60800000
BUILT = RCQ + 0x100000          # the builder's own page for the expected arguments
SESSION = RCQ + 0x101000        # the session that page is built from
IOVA_BASE = 0x10000000
LINE = 64
# System registers that the cache maintenance reads, as the image's EL1 tasks see them on the board: CurrentEL 1, the
# MMU and data cache on, and CTR_EL0 with DminLine and CWG 4 (64-byte lines). The emulated core reports none of these.
MRS = {0xd5384240: 4, 0xd5381000: 0x30d01805, 0xd53b0020: (4 << 24) | (4 << 16)}   # CurrentEL, SCTLR_EL1, CTR_EL0
# apps/os/g2d.c: enum phase, enum op, the request states, the step numbers.
CLOSED, OPEN, FAULT, CLOSE_OP, RUN_OP, POSTED, DONE = 0, 2, 4, 2, 3, 1, 2
STEP_RUN_ATTACH, STEP_RUN_CACHE, STEP_RUN_LAUNCH, STEP_RUN_TIMEOUT = 57, 59, 61, 62
CACHE_CLEAN, CACHE_CLEAN_INVALIDATE = 0, 1     # enum aarch64_cache_op
IOMMU_READ, IOMMU_WRITE = 1, 2
PREFETCH = [0x30079, 0x3007f]   # the owner's bank words (docs/iommu.md)
CCU, PCK1, G2D = 0x02002000, 0x07061000, 0x05440000
IOMMU_OWNER_FAULT, G2D_MASTER = 2, 10
# The CCU words of an open engine: module clock on, bus gate open and G2D reset deasserted, DE_SYS reset deasserted;
# the DE and DI gates closed, so the close also asserts the DE_SYS reset.
OPENED_CCU = {0xa40: 0x80000000, 0xa44: 0x00010001, 0xa74: 0x00010000}
# g2d_mixer_apply: the update bit off first, then the interrupt enables, the RCQ header and the update bit on. The
# owner reads 0x28 and 0x100 idle between the first two writes.
LAUNCH = [(0x28, 0), (0x20, 0), (0x24, 1), (0x2c, None), (0x30, 0), (0x34, 128), (0x20, 0x10), (0x28, 1)]
# The stage-3 plan (apps/g2d-check/host.py, seed 0xbb67ae85): 395 by 257 at stride 1644, operation 1 a 282 by 114 copy
# from (108, 13) to (8, 44) converting ARGB8888 to BGRA8888, operation 2 a 172 by 48 copy from (222, 206) to
# (108, 131) unconverted.
STAGE3 = (395, 257, 1644)
# The command pages of the board-qualified image 916988f84c39 (runs 1794725199336568290-5c9e9da6 and
# 1794714427314630502-a44050c7), by SHA-256: both stage-3 copies, and a stage-2-shaped fill. The vendor path's pages
# must stay byte-identical to them.
QUALIFIED_PAGES = {'blit1': '9199b2f7878a615c78fc40cbc61fb8adc76fc10861aff902e26ee90b0236ac21',
                   'blit2': '9e77ad4dfe814f5ef2e1dc32b018d51ae80326abc359a89a5da8d296499fab2d',
                   'fill': 'e4ed360e8c298dbd9977aa45d92a9298a8ad6092ddeb5e3d8c38051942fd94ce'}
# The exact-copy candidates (include/a733_g2d_application.h): the page words, at their byte offsets, in which each
# path's copy page differs from the vendor path's. The video layer's attribute word is at 128 and the blender at 384.
V0, BLD = 128, 384
PATH_WORDS = {1: {BLD + 0x10: 0x00a1b2c3, BLD + 0x44: 0x00d4e5f6},
              2: {V0: None, BLD + 0x40: 1, BLD + 0x60: 1},
              3: {BLD + 0x80: 0, BLD + 0x84: 0x61082},
              4: {BLD + 0x00: 0x100}}
STAGE3_OPS = [dict(w=282, h=114, sx=108, sy=13, dx=8, dy=44, format=0, output_format=3),
              dict(w=172, h=48, sx=222, sy=206, dx=108, dy=131, format=0, output_format=0)]


def stage2():
    return 420, 271, 1744, 0


def shapes():
    """The stage-2 surface, then one surface per (start offset within a line, size modulo 64) class."""
    out, seen = [stage2()], set()
    for offset in range(0, LINE, 4):
        for want in range(0, LINE, 4):
            found = None
            for width in range(320, 641):
                stride = width * 4 + 64
                for height in range(180, 361):
                    if (offset + stride * height) % LINE == (offset + want) % LINE:
                        found = (width, height, stride, offset)
                        break
                if found:
                    break
            assert found, (offset, want)
            key = (offset % LINE, (found[2] * found[1]) % LINE)
            if key not in seen:
                seen.add(key)
                out.append(found)
    return out


class G2d:
    """The engine's registers as the operation path sees them, and the writes it made.

    An engine that does not complete keeps running after the launch: the mixer's start (0x100 bit 31) stays set until
    the mixer AHB reset (0x08 bit 0) is asserted, or for good when the engine is stuck. A write of one to 0x24 clears
    task end (bit 0) and leaves cfg_finish (bit 2) set, as board run 1794722460072814405-2b77c3eb measured. A stuck
    0x24 bit (launch_fault) never clears."""
    def __init__(self, completes, stuck=False, status_stuck=0, update_sticks=False):
        self.regs = {0x08: 3, 0x20: 0, 0x24: status_stuck, 0x28: 0, 0x100: 0}
        self.writes, self.completes, self.launched, self.interrupts = [], completes, False, 0
        self.stuck, self.status_stuck, self.running, self.update_sticks = stuck, status_stuck, False, update_sticks

    def read(self, off):
        return self.regs.get(off, 0)

    def write(self, off, value):
        self.writes.append((off, value))
        if off == 0x24:
            # Task end is write-one-to-clear; cfg_finish stays set (board run 1794722460072814405-2b77c3eb).
            self.regs[0x24] = (self.regs[0x24] & ~(value & 1) & 0xffffffff) | self.status_stuck
            return
        self.regs[off] = value
        if off == 0x28 and value & 1:
            self.launched = True
            if self.completes:
                self.regs[0x24] |= 5                   # task end and cfg_finish
                if not self.update_sticks:
                    self.regs[0x28] = 0
            else:
                self.running = True
                self.regs[0x100] = 0x80000000
        if off == 0x08 and not value & 1 and self.running and not self.stuck:
            self.running = False                       # the mixer reset stops the engine and clears its registers
            self.regs[0x100] = 0


class Registers:
    """Plain 32-bit registers of one page."""
    def __init__(self, base, words):
        self.base, self.words, self.writes = base, dict(words), []

    def read(self, uc, offset, size, data):
        return self.words.get(offset, 0)

    def write(self, uc, offset, size, value, data):
        self.writes.append((offset, value))
        self.words[offset] = value


def run_case(image, parts, fields, name, width, height, stride, offset, refuse_cache=False, completes=True,
             stuck=False, launch_fault=False, attach_fault=False, close=False, blits=(), update_sticks=False,
             fills=1):
    uc = harness_machine()
    low = min(a for a, _, _ in parts) & ~0xfff
    high = (max(a + n for a, _, n in parts) + 0xfff) & ~0xfff
    uc.mem_map(low, high - low)
    for address, data, _ in parts:
        uc.mem_write(address, data)
    uc.mem_map(SURFACE, 2 * REGION_BYTES)
    uc.mem_map(RCQ, 0x300000)
    skip_crumbs(uc, image)
    engine = G2d(completes, stuck, 1 if launch_fault else 0, update_sticks)
    ccu, pck = Registers(CCU, OPENED_CCU), Registers(PCK1, {0x8: 8})
    state = {'time': 1000, 'ticks': 24000, 'iova': IOVA_BASE, 'cache': [], 'stopped': False, 'irq_entered': 0,
             'unmapped': None, 'detach': [], 'events': [], 'attach': [], 'map': [], 'unmap': [], 'prefetch': 0}
    ol = image.layouts['a733_iommu_owner']
    g2d = image.address('g2d', UNIT)
    board = image.address('board')

    def put(address, value, n=4): uc.mem_write(address, int(value).to_bytes(n, 'little'))
    def get(address, n=4): return int.from_bytes(uc.mem_read(address, n), 'little')
    def field(name): return g2d + fields['g2d'][name]

    # The session and the slot.
    put(board + image.layouts['board_application']['session'] + image.layouts['a733_board_session']['deadline_us'],
        60_000_000, 8)
    slot = image.layouts['large_slot']
    for at, base in ((SLOT, SURFACE), (SLOT1, SOURCE)):
        put(at + slot['base'], base, 8); put(at + slot['bytes'], REGION_BYTES, 8)
        put(at + slot['flags'], 1); put(at + slot['state'], 2)
    # The open service with one posted fill.
    put(field('phase'), OPEN); put(field('op'), RUN_OP); put(field('request'), POSTED)
    job = field('job')
    jl, sl = image.layouts['ss_g2d_job'], image.layouts['ss_g2d_surface']
    w, h = 1 + width // 3, 1 + height // 5
    dx, dy = width - w, height - h
    put(job + jl['op'], 1); put(job + jl['color'], 0x83b0ce22)
    out = job + jl['output']
    put(out + sl['region'], 0); put(out + sl['format'], 0); put(out + sl['offset'], offset, 8)
    put(out + sl['width'], width); put(out + sl['height'], height); put(out + sl['stride'], stride)
    put(job + jl['dx'], dx); put(job + jl['dy'], dy); put(job + jl['w'], w); put(job + jl['h'], h)

    def post_blit(op):
        # A blit from region 1 (the source) to region 0 (the output), both of the shape's geometry.
        uc.mem_write(job, bytes(jl['reserved'] + (4 if 'path' in jl else 8)))
        put(job + jl['op'], 2)
        if 'path' in jl:
            put(job + jl['path'], op.get('path', 0))
        for surface, region, fmt in ((job + jl['source'], 1, op['format']), (job + jl['output'], 0, op['output_format'])):
            put(surface + sl['region'], region); put(surface + sl['format'], fmt); put(surface + sl['offset'], offset, 8)
            put(surface + sl['width'], width); put(surface + sl['height'], height); put(surface + sl['stride'], stride)
        for k in ('sx', 'sy', 'dx', 'dy', 'w', 'h'):
            put(job + jl[k], op[k])
        put(field('request'), 0); put(field('op'), RUN_OP); put(field('request'), POSTED)
    if blits:
        post_blit(blits[0])

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

    def ticks(m):
        state['ticks'] += 24
        ret(m, state['ticks'])

    def iova(m):
        bytes_, align, at = (m.reg_read(r) for r in (UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2))
        value = (state['iova'] + align - 1) & ~(align - 1)
        state['iova'] = value + bytes_
        put(at, value, 8)
        ret(m, 1)

    def scan(m):
        uc.mem_write(m.reg_read(UC_ARM64_REG_X1), bytes(40))
        ret(m, 1)

    def wait(m):
        target = m.reg_read(UC_ARM64_REG_X0)
        if engine.launched and engine.regs[0x24] & 1 and get(field('armed')):
            # The interrupt: the image's handler runs and returns to the waiter's caller.
            state['irq_entered'] += 1
            m.reg_write(UC_ARM64_REG_PC, image.address('os_g2d_irq'))
            return
        state['time'] = max(state['time'], target)
        ret(m, 1)

    def g2d_rd(m):
        off, at = m.reg_read(UC_ARM64_REG_X0), m.reg_read(UC_ARM64_REG_X1)
        put(at, engine.read(off))
        ret(m, 1)

    def g2d_wr(m):
        engine.write(m.reg_read(UC_ARM64_REG_X0), m.reg_read(UC_ARM64_REG_X1) & 0xffffffff)
        ret(m, 1)

    def translate(m):
        address, out = m.reg_read(UC_ARM64_REG_X0), m.reg_read(UC_ARM64_REG_X2)
        uc.mem_write(out, address.to_bytes(8, 'little') + bytes(8))      # physical, Device-nGnRnE, shareability 0
        ret(m, 1)

    def span_list(at, count):
        sp = image.layouts['a733_iommu_span']
        return [{'iova': get(at + i * 32 + sp['iova'], 8), 'physical': get(at + i * 32 + sp['physical'], 8),
                 'bytes': get(at + i * 32 + sp['bytes'], 8), 'access': get(at + i * 32 + sp['access'])}
                for i in range(count)]

    def attach(m):
        state['attach'].append(span_list(m.reg_read(UC_ARM64_REG_X1), m.reg_read(UC_ARM64_REG_X2)))
        if not attach_fault:
            put(OWNER + ol['state'], 1); put(OWNER + ol['attached'], 1 << G2D_MASTER, 2)
            ret(m, 1)
            return
        # The owner took its clock lease, recorded master 10 attached, then failed a register write.
        put(OWNER + ol['state'], IOMMU_OWNER_FAULT); put(OWNER + ol['attached'], 1 << G2D_MASTER, 2)
        put(OWNER + ol['fault_step'], 1); put(OWNER + ol['fault_word'], 0x1004)
        ret(m, -1)

    def detach(m):
        state['detach'].append((m.reg_read(UC_ARM64_REG_X0) & 0xffff, engine.running))
        state['events'].append('detach')
        put(OWNER + ol['attached'], 0, 2)
        ret(m, 1)

    def cache(m):
        op, at, n = (m.reg_read(r) for r in (UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2))
        state['cache'].append((op, at, n))
        if refuse_cache and len(state['cache']) == 1:
            ret(m, 0)                                   # otherwise the image's own function runs

    def stop(m):
        state['stopped'] = True
        m.emu_stop()

    def prefetch(m):
        state['prefetch'] += 1
        put(m.reg_read(UC_ARM64_REG_X0), PREFETCH[0]); put(m.reg_read(UC_ARM64_REG_X0) + 4, PREFETCH[1])
        ret(m, 1)

    def mmio_read(m, offset, size, data):
        return engine.read(offset)

    def mmio_write(m, offset, size, value, data):
        if offset == 0x08:
            state['events'].append('reset-%d' % (value & 3))
        engine.write(offset, value)

    def ccu_write(m, offset, size, value, data):
        state['events'].append('ccu-%x' % offset)
        ccu.write(m, offset, size, value, data)

    uc.mmio_map(G2D, 0x1000, mmio_read, None, mmio_write, None)
    uc.mmio_map(CCU, 0x2000, ccu.read, None, ccu_write, None)
    uc.mmio_map(PCK1, 0x1000, pck.read, None, pck.write, None)
    hooks = {
        'sunstone_deadline_register': lambda m: ret(m, 1),
        'sunstone_deadline_now_us': now,
        'sunstone_deadline_wait_until': wait,
        'a733_counter_clock_now': counter_now,
        'ss_counter_ticks': ticks,
        'os_large_slot': lambda m: ret(m, {0: SLOT, 1: SLOT1}.get(m.reg_read(UC_ARM64_REG_X0), 0)),
        'os_owner_memory': lambda m: ret(m, RCQ),
        'os_iommu_iova': iova,
        'os_iommu_attach': attach,
        'os_iommu_detach': detach,
        'aarch64_device_translate': translate,
        'a733_platform_lock_enter': lambda m: ret(m, 1),
        'a733_platform_lock_leave': lambda m: ret(m),
        'board_live': lambda m: ret(m, 1),
        'gicv3_spi_masked': lambda m: ret(m, 1),
        'os_iommu_map': lambda m: (state['map'].append(span_list(m.reg_read(UC_ARM64_REG_X0), m.reg_read(UC_ARM64_REG_X1))), ret(m, 1)),
        'os_iommu_unmap': lambda m: (state['unmap'].append(span_list(m.reg_read(UC_ARM64_REG_X0), m.reg_read(UC_ARM64_REG_X1))), ret(m, 1)),
        'os_iommu_prefetch': prefetch,
        'os_iommu_owner': lambda m: ret(m, OWNER),
        'os_iommu_scan': scan,
        'aarch64_device_cache': cache,
        'gicv3_spi_mask': lambda m: ret(m, 1),
        'vTaskGenericNotifyGiveFromISR': lambda m: ret(m),
        'ucPortGetCoreID': lambda m: ret(m, 0),
        'os_application_wake_task': stop,
    }
    for symbol, callback in hooks.items():
        if symbol == 'os_iommu_prefetch' and not image.symbols.get(symbol):
            continue                                    # an image before the stage-3 record
        hook_addresses(uc, {image.address(symbol)}, lambda m, a, s, u, c=callback: c(m))
    for symbol, callback in (('g2d_rd', g2d_rd), ('g2d_wr', g2d_wr), ('pause_us', lambda m: ret(m))):
        hook_addresses(uc, {image.address(symbol, UNIT)}, lambda m, a, s, u, c=callback: c(m))

    def mrs(m, address, size, unused):
        if m.reg_read(UC_ARM64_REG_PC) != address:
            return                                      # the entry hook already returned from the function
        word = int.from_bytes(m.mem_read(address, 4), 'little')
        if word & 31 != 31:
            m.reg_write(getattr(arm64_const, f'UC_ARM64_REG_X{word & 31}'), MRS[word & 0xffffffe0])
        m.reg_write(UC_ARM64_REG_PC, address + 4)
    start = image.address('aarch64_device_cache')
    hook_addresses(uc, [a for a in instruction_sites([(a, d) for a, d, _ in parts], lambda w: w & 0xffffffe0 in MRS)
                        if start <= a < start + 0x100], mrs)

    def unmapped(m, access, address, size, value, data):
        state['unmapped'] = (access, hex(address), hex(m.reg_read(UC_ARM64_REG_PC)))
        return False
    uc.hook_add(UC_HOOK_MEM_UNMAPPED, unmapped)
    uc.mem_map(STACK - 0x10000, 0x10000)

    def serve(what):
        state['stopped'] = False
        uc.reg_write(UC_ARM64_REG_SP, STACK - 0x100)
        uc.reg_write(UC_ARM64_REG_LR, 0)
        try:
            uc.emu_start(image.address('os_g2d_task', UNIT), 0, count=60_000_000)
        except Exception as error:
            raise AssertionError(f'{name}: {what}: emulation stopped at {hex(uc.reg_read(UC_ARM64_REG_PC))}: {error}; '
                                 f'unmapped access {state["unmapped"]}')
        assert state['stopped'], (name, what, 'the owner did not complete the request', hex(uc.reg_read(UC_ARM64_REG_PC)))
    serve('operation')
    ops = []

    def op_record():
        dl = image.layouts['ss_g2d_done']
        done = field('done')
        record = {'result': struct.unpack('<i', uc.mem_read(field('result'), 4))[0],
                  'fault_step': get(field('fault_step')), 'fault_value': get(field('fault_value')),
                  'page': bytes(uc.mem_read(RCQ, 4096)), 'writes': list(engine.writes)}
        for k in ('irq_count', 'status_after', 'rcq_status', 'iommu_attached', 'output_iova'):
            record[k] = get(done + dl[k])
        for k in ('update_after', 'mixer_after', 'source_iova', 'status_launch'):
            record[k] = get(done + dl[k]) if k in dl else None
        record['iommu_prefetch'] = [get(done + dl['iommu_prefetch'] + 4 * b) for b in (0, 1)] if 'iommu_prefetch' in dl else None
        return record
    if fills > 1:
        ops.append(op_record())
        for _ in range(fills - 1):
            engine.writes.clear()
            put(field('request'), 0); put(field('op'), RUN_OP); put(field('request'), POSTED)
            serve('fill %d' % (len(ops) + 1))
            ops.append(op_record())
    if blits:
        ops.append(op_record())
        for op in blits[1:]:
            engine.writes.clear()
            post_blit(op)
            serve('operation %d' % (len(ops) + 1))
            ops.append(op_record())
    run = {'result': struct.unpack('<i', uc.mem_read(field('result'), 4))[0], 'phase': get(field('phase')),
           'fault_step': get(field('fault_step'))}
    if close:
        # The application takes the operation's result, then posts its close.
        state['events'].clear()
        put(field('request'), 0); put(field('op'), CLOSE_OP); put(field('request'), POSTED)
        serve('close')
    done = field('done')
    dl = image.layouts['ss_g2d_done']
    record = {
        'case': name, 'width': width, 'height': height, 'stride': stride, 'offset': offset,
        'result': struct.unpack('<i', uc.mem_read(field('result'), 4))[0],
        'request': get(field('request')), 'phase': get(field('phase')),
        'fault_step': get(field('fault_step')), 'fault_value': get(field('fault_value')),
        'irq_count': get(done + dl['irq_count']), 'stray': get(done + dl['stray_interrupts']),
        'rcq_status': get(done + dl['rcq_status']), 'status_after': get(done + dl['status_after']),
        'cache': [(op, hex(at), n) for op, at, n in state['cache']],
        'writes': [(hex(o), hex(v)) for o, v in engine.writes], 'interrupts': state['irq_entered'],
        'run': run, 'detach': state['detach'], 'events': state['events'], 'running': engine.running,
        'ccu': {hex(k): hex(v) for k, v in sorted(ccu.words.items())},
        'ops': ops,
    }
    if blits:
        record['built'] = [built(uc, image, op, width, height, stride, offset, o) for op, o in zip(blits, ops)]
    return record, state, engine


def saved(record):
    """The record as result.json keeps it: the command pages by digest and the register writes in hex."""
    for o in record.get('ops', []):
        o['page'] = hashlib.sha256(o['page']).hexdigest()
        o['writes'] = [(hex(a), hex(v)) for a, v in o['writes']]
    return record


def built(uc, image, op, width, height, stride, offset, record):
    """The image's own builder, a733_g2d_commands_at, run on the session a blit of op names, with the device addresses
    the operation must use: the output region at IOVA_BASE, the source region at IOVA_BASE + 2 MiB and the command
    page after them. Returns whether the service's command page equals it."""
    sl = image.layouts['a733_g2d_session']
    uc.mem_write(SESSION, bytes(904))
    def put(at, value): uc.mem_write(at, int(value).to_bytes(4, 'little'))
    for k, v in (('width', width), ('height', height), ('stride', stride), ('op', 2), ('format', op['format']),
                 ('output_format', op['output_format']), ('sx', op['sx']), ('sy', op['sy']), ('dx', op['dx']),
                 ('dy', op['dy']), ('w', op['w']), ('h', op['h'])):
        put(SESSION + sl[k], v)
    rcq_iova = IOVA_BASE + 2 * REGION_BYTES
    for register, value in zip((UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, arm64_const.UC_ARM64_REG_X3,
                                arm64_const.UC_ARM64_REG_X4, arm64_const.UC_ARM64_REG_X5),
                               (BUILT, 4096, SESSION, IOVA_BASE + REGION_BYTES + offset, IOVA_BASE + offset, rcq_iova)):
        uc.reg_write(register, value)
    uc.reg_write(UC_ARM64_REG_SP, STACK - 0x100)
    uc.reg_write(UC_ARM64_REG_LR, 0)
    uc.emu_start(image.address('a733_g2d_commands_at'), 0, count=2_000_000)
    return uc.reg_read(UC_ARM64_REG_X0) == 1 and bytes(uc.mem_read(BUILT, 4096)) == record['page']


def check_surface(record, state, engine):
    name = record['case']
    first = SURFACE + record['offset']
    end = first + record['stride'] * record['height']
    assert record['result'] == 1 and record['phase'] == OPEN and record['request'] == DONE, record
    assert record['irq_count'] == 1 and record['stray'] == 0 and state['irq_entered'] == 1, record
    assert record['rcq_status'] & 1 and not record['status_after'] & 1, record
    launch = [(o, v) for o, v in engine.writes if o in (0x20, 0x24, 0x28, 0x2c, 0x30, 0x34)][:len(LAUNCH)]
    assert [o for o, _ in launch] == [o for o, _ in LAUNCH], (name, launch)
    assert all(want is None or v == want for (_, v), (_, want) in zip(launch, LAUNCH)), (name, launch)
    assert len(state['cache']) == 2, (name, state['cache'])
    for op, at, n in state['cache']:
        assert at % LINE == 0 and n % LINE == 0, (name, 'cache range not whole lines', hex(at), n)
        assert at <= first and at + n >= end, (name, 'cache range misses the surface', hex(at), n)
        assert at >= SURFACE and at + n <= SURFACE + REGION_BYTES, (name, 'cache range leaves the region')
        assert first - at < LINE and at + n - end < LINE, (name, 'cache range wider than the surface lines')


def check_blits(record, state, engine, update_sticks):
    """Both stage-3 copies in one session: one attach of the output read-write, the source read-only and the command
    page; the vendor launch order for each; the builder's own page for each; whole-line maintenance of both surfaces."""
    name, ops = record['case'], record['ops']
    assert len(ops) == 2 and all(o['result'] == 1 for o in ops) and record['phase'] == OPEN, \
        (name, [(o['result'], o['fault_step'], hex(o['fault_value'])) for o in ops])
    spans = state['attach']
    assert len(spans) == 1 and len(spans[0]) == 3, (name, 'one attach of three spans', spans)
    out, src, rcq = spans[0]
    assert (out['iova'], out['physical'], out['bytes'], out['access']) == \
        (IOVA_BASE, SURFACE, REGION_BYTES, IOMMU_READ | IOMMU_WRITE), (name, 'output span', out)
    assert (src['iova'], src['physical'], src['bytes'], src['access']) == \
        (IOVA_BASE + REGION_BYTES, SOURCE, REGION_BYTES, IOMMU_READ), (name, 'the source must be mapped read-only', src)
    assert (rcq['iova'], rcq['physical'], rcq['bytes'], rcq['access']) == \
        (IOVA_BASE + 2 * REGION_BYTES, RCQ, 4096, IOMMU_READ), (name, 'command page span', rcq)
    assert not state['map'] and not state['unmap'], (name, 'the second operation must reuse the mappings', state)
    assert state['prefetch'] == 1 and ops[0]['iommu_prefetch'] == PREFETCH and ops[1]['iommu_prefetch'] == [0, 0], \
        (name, 'prefetch words recorded once, after the attach', [o['iommu_prefetch'] for o in ops])
    assert [o['iommu_attached'] for o in ops] == [1, 0] and [o['irq_count'] for o in ops] == [1, 2], (name, ops)
    assert [o['status_launch'] for o in ops] == [0, 4], \
        (name, '0x24 before each launch: clear, then cfg_finish left set by the first job', [o['status_launch'] for o in ops])
    for o in ops:
        assert o['output_iova'] == IOVA_BASE + record['offset'], (name, hex(o['output_iova']))
        assert o['source_iova'] == IOVA_BASE + REGION_BYTES + record['offset'], (name, hex(o['source_iova']))
        launch = [(a, v) for a, v in o['writes'] if a in (0x20, 0x24, 0x28, 0x2c, 0x30, 0x34)][:len(LAUNCH)]
        assert [a for a, _ in launch] == [a for a, _ in LAUNCH] and \
            all(want is None or v == want for (_, v), (_, want) in zip(launch, LAUNCH)), (name, 'launch order', launch)
        assert launch[3][1] == IOVA_BASE + 2 * REGION_BYTES, (name, 'RCQ header address', launch)
        assert o['status_after'] == 4 and o['update_after'] == (1 if update_sticks else 0) and o['mixer_after'] == 0, \
            (name, 'after-operation words', hex(o['status_after']), hex(o['update_after']), hex(o['mixer_after']))
    assert record['built'] == [True, True], (name, 'the command page differs from the builder\'s for the plan')
    pages = [hashlib.sha256(o['page']).hexdigest() for o in ops]
    assert pages == [QUALIFIED_PAGES['blit1'], QUALIFIED_PAGES['blit2']], \
        (name, 'the vendor path\'s copy pages must equal the board-qualified image\'s', pages)
    cache = state['cache']
    assert [op for op, _, _ in cache] == [CACHE_CLEAN_INVALIDATE, CACHE_CLEAN, CACHE_CLEAN_INVALIDATE] * 2, (name, cache)
    for (op, at, n), base in zip(cache, [SURFACE, SOURCE, SURFACE] * 2):
        first, end = base + record['offset'], base + record['offset'] + record['stride'] * record['height']
        assert at % LINE == 0 and n % LINE == 0 and at <= first and at + n >= end and \
            first - at < LINE and at + n - end < LINE, (name, 'maintenance must cover the surface in whole lines', hex(at), n)


def check_fills(record):
    """Two fills in one session with an engine that leaves the update bit set: the second one launches."""
    name, ops = record['case'], record['ops']
    assert [(o['result'], o['fault_step']) for o in ops] == [(1, 0), (1, 0)] and record['phase'] == OPEN, \
        (name, 'the second operation must launch although 0x28 read 1 after the first',
         [(o['result'], o['fault_step'], hex(o['fault_value'])) for o in ops])
    assert [o['irq_count'] for o in ops] == [1, 2], (name, ops)
    pages = [hashlib.sha256(o['page']).hexdigest() for o in ops]
    assert pages == [QUALIFIED_PAGES['fill']] * 2, (name, 'the fill pages must equal the board-qualified image\'s', pages)


def check_paths(record):
    """Stage-3 operation 2's copy once on each path, 0 to 4, in one session: every copy completes, and each candidate's
    page differs from path 0's in exactly its PATH_WORDS, with those values; path 0's page is the qualified one."""
    name, ops = record['case'], record['ops']
    assert [(o['result'], o['fault_step']) for o in ops] == [(1, 0)] * 5 and record['phase'] == OPEN, \
        (name, [(o['result'], o['fault_step'], hex(o['fault_value'])) for o in ops])
    base = ops[0]['page']
    assert hashlib.sha256(base).hexdigest() == QUALIFIED_PAGES['blit2'], (name, 'path 0 page')
    word = lambda page, at: struct.unpack_from('<I', page, at)[0]
    for path in range(1, 5):
        page = ops[path]['page']
        differ = {at for at in range(0, len(page), 4) if word(page, at) != word(base, at)}
        want = PATH_WORDS[path]
        assert differ == set(want), (name, path, 'differing words', sorted(hex(a) for a in differ))
        for at, value in want.items():
            expect = word(base, V0) | 1 << 16 if value is None else value
            assert word(page, at) == expect, (name, path, hex(at), hex(word(page, at)), hex(expect))


def check_close(record, step, closes):
    name, run, events = record['case'], record['run'], record['events']
    assert run['result'] == -1 and run['phase'] == FAULT and run['fault_step'] == step, (name, 'operation', run)
    assert record['fault_step'] == step, (name, 'the close must keep the first fault', record['fault_step'])
    stop = [e for e in events if e.startswith('reset-')]
    assert stop[:2] == ['reset-0', 'reset-3'], (name, 'the close must pulse the mixer and rotator resets', events)
    ccu = {int(k, 16): int(v, 16) for k, v in record['ccu'].items()}
    if not closes:
        assert record['result'] == -1 and record['phase'] == FAULT, (name, 'a running engine must leave FAULT', record)
        assert not record['detach'], (name, 'master 10 detached while the engine runs', record['detach'])
        assert ccu[0xa44] & 0x10001 == 0x10001, (name, 'the bus gate and reset must stay open', record['ccu'])
        return
    assert record['result'] == 1 and record['phase'] == CLOSED and record['request'] == DONE, (name, record)
    assert record['detach'] == [(1 << G2D_MASTER, False)], (name, 'one detach of master 10, engine stopped', record['detach'])
    assert events.index('reset-3') < events.index('detach') < events.index('ccu-a40'), (name, 'order', events)
    assert not ccu[0xa40] & 0x80000000 and not ccu[0xa44] & 0x10001 and not ccu[0xa74] & 0x10000, (name, record['ccu'])


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    image = Image(a.image, {'board_application', 'a733_board_session', 'large_slot', 'ss_g2d_job', 'ss_g2d_surface',
                            'ss_g2d_done', 'a733_iommu_owner', 'a733_iommu_span', 'a733_g2d_session'})
    with a.image.open('rb') as stream:
        elf = ELFFile(stream)
        parts = [(int(s['p_vaddr']), bytes(s.data()), int(s['p_memsz']))
                 for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']
        fields = {}
        for cu in elf.get_dwarf_info().iter_CUs():
            if not cu.get_top_DIE().attributes['DW_AT_name'].value.decode().endswith(UNIT):
                continue
            for die in cu.iter_DIEs():
                if die.tag == 'DW_TAG_variable' and die.attributes.get('DW_AT_name') and \
                        die.attributes['DW_AT_name'].value == b'g2d' and 'DW_AT_type' in die.attributes:
                    typ = die.get_DIE_from_attribute('DW_AT_type')
                    fields['g2d'] = {c.attributes['DW_AT_name'].value.decode(): c.attributes['DW_AT_data_member_location'].value
                                     for c in typ.iter_children() if c.tag == 'DW_TAG_member'}
                if die.tag == 'DW_TAG_enumerator':
                    n, v = die.attributes['DW_AT_name'].value.decode(), die.attributes['DW_AT_const_value'].value
                    want = {'CLOSED': CLOSED, 'OPEN': OPEN, 'FAULT': FAULT, 'CLOSE_OP': CLOSE_OP, 'RUN_OP': RUN_OP,
                            'POSTED': POSTED, 'DONE': DONE, 'G2D_STEP_RUN_ATTACH': STEP_RUN_ATTACH,
                            'G2D_STEP_RUN_CACHE': STEP_RUN_CACHE, 'G2D_STEP_RUN_LAUNCH': STEP_RUN_LAUNCH,
                            'G2D_STEP_RUN_TIMEOUT': STEP_RUN_TIMEOUT, 'A733_IOMMU_OWNER_FAULT': IOMMU_OWNER_FAULT}.get(n)
                    assert want is None or want == v, (n, v)
    assert 'g2d' in fields, 'no g2d state in ' + UNIT
    rows, failures = [], []
    cases = [('surface-%dx%d-stride%d-offset%d' % s, s, {}) for s in shapes()]
    cases += [('cache-refusal', stage2(), {'refuse_cache': True}), ('timeout', stage2(), {'completes': False})]
    closes = {'timeout-close': ({'completes': False}, STEP_RUN_TIMEOUT, True),
              'timeout-stuck-close': ({'completes': False, 'stuck': True}, STEP_RUN_TIMEOUT, False),
              'launch-fault-close': ({'launch_fault': True}, STEP_RUN_LAUNCH, True),
              'attach-fault-close': ({'attach_fault': True}, STEP_RUN_ATTACH, True)}
    cases += [(n, stage2(), dict(o, close=True)) for n, (o, _, _) in closes.items()]
    # Stage 3: both copies in one session, with the update bit left set by the engine and cleared by it (the board's
    # behaviour on run 1794722460072814405-2b77c3eb).
    blits = {'blits-update-sticks': True, 'blits-update-clears': False}
    cases += [(n, STAGE3 + (0,), {'blits': STAGE3_OPS, 'update_sticks': v}) for n, v in blits.items()]
    cases += [('fills-update-sticks', stage2(), {'fills': 2, 'update_sticks': True})]
    # The exact-copy candidates: stage-3 operation 2's copy on paths 0 to 4 in one session.
    cases += [('copy-paths', STAGE3 + (0,), {'blits': [dict(STAGE3_OPS[1], path=p) for p in range(5)]})]
    # A path outside 0 to 4 is refused before any write.
    cases += [('path-refused', STAGE3 + (0,), {'blits': [dict(STAGE3_OPS[1], path=5)]})]
    for name, shape, options in cases:
        try:
            record, state, engine = run_case(image, parts, fields, name, *shape, **options)
            if name.startswith('surface'):
                check_surface(record, state, engine)
            elif name == 'fills-update-sticks':
                check_fills(record)
            elif name == 'copy-paths':
                check_paths(record)
            elif name == 'path-refused':
                op = record['ops'][0]
                assert (op['result'], op['fault_step']) == (-1, 48) and record['phase'] == OPEN, (record['ops'], record['phase'])
                assert not record['ops'][0]['writes'], (name, 'a refused path wrote', record['ops'][0]['writes'])
            elif name in blits:
                check_blits(record, state, engine, blits[name])
            elif name in closes:
                check_close(record, *closes[name][1:])
            elif name == 'cache-refusal':
                assert record['result'] == -1 and record['fault_step'] == STEP_RUN_CACHE, record
                assert record['phase'] == OPEN, (record, 'a refused cache maintenance must leave the service open')
                assert not any(o == 0x28 and v & 1 for o, v in engine.writes), (record, 'launched')
            else:
                assert record['result'] == -1 and record['fault_step'] == STEP_RUN_TIMEOUT, record
                assert record['phase'] == FAULT, record
            record['passed'] = True
            saved(record)
        except AssertionError as error:
            record = {'case': name, 'passed': False, 'error': str(error)}
            failures.append(name)
        rows.append(record)
    save(a.output / 'result.json', {
        'passed': not failures, 'physical_access': False, 'image': str(a.image),
        'image_sha256': hashlib.sha256(a.image.read_bytes()).hexdigest(), 'cases': rows,
        'sources_sha256': {n: hashlib.sha256((Path(__file__).resolve().parents[1] / n).read_bytes()).hexdigest()
                           for n in ('apps/os/g2d.c', 'apps/os/iommu.c', 'drivers/g2d/a733_g2d.c', 'checks/os-g2d-operation.py')}})
    if failures:
        for row in rows:
            if not row['passed']:
                print('FAIL', row['case'], row['error'][:600])
        raise SystemExit(f'{len(failures)} of {len(rows)} cases failed')
    print(f'PASS G2D operation path: {len(rows) - 5 - len(closes) - len(blits)} surfaces, cache refusal leaves the '
          f'service open, timeout faults, {len(closes)} closes after a fault, both stage-3 copies in one session, two '
          f'fills with the update bit left set, the vendor path\'s pages equal the qualified image\'s, and the four '
          f'exact-copy candidates change only their own words; a path outside 0 to 4 is refused')


if __name__ == '__main__':
    main()
