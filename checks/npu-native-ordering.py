#!/usr/bin/env python3
"""Exercise the linked NPU adapter's posted-write and delay boundaries.

Architectural transaction completion and peripheral readiness are separate
models. Device-nGnRE may acknowledge before endpoint arrival; this check
does not establish a physical reset pulse or device timing qualification.
"""
import argparse
import hashlib
import json
from pathlib import Path

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_LR

import harness
from register_latency import PostedWrites, RegisterLatency

RAM = 0x43000000
STOP = RAM + 0xff000
REGISTERS = 0x03600000
DSB_SY = 0xd5033f9f
NOP = 0xd503201f
UNIT = 'platform/a733/npu_native.c'


class Machine:
    def __init__(self, path, omit_dsb=False, stuck=False):
        self.image = image = harness.Image(path, layout_names=('a733_npu_native', 'counter_clock'))
        self.u = u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        u.mem_map(0x40000000, 0x04000000)
        u.mem_map(REGISTERS, 4096)
        for address, data in image.parts:
            u.mem_write(address, data)
        self.native = RAM
        native = image.layouts['a733_npu_native']
        self.put(RAM + native['initialized'], 1, 4)
        self.put(RAM + native['registers'], REGISTERS)
        self.put(RAM + native['clock'] + image.layouts['counter_clock']['frequency'], 24000000, 4)
        self.clock = 23
        self.posted = PostedWrites({})
        self.ready = RegisterLatency(0, 0, 1, 1, pending=False, stuck=stuck)
        self.status = 0
        self.completion_times = []
        self.stats = {'instructions': 0, 'dmb': 0, 'dsb': 0, 'stores': 0}
        entry = image.address('write32', UNIT)
        with Path(path).open('rb') as stream:
            symbols = ELFFile(stream).get_section_by_name('.symtab')
            sizes = [int(s['st_size']) for s in symbols.iter_symbols()
                     if int(s['st_value']) == entry and s['st_info']['type'] == 'STT_FUNC']
        assert len(sizes) == 1
        sites = [at for at in range(entry, entry + sizes[0], 4) if self.get(at, 4) == DSB_SY]
        assert len(sites) == 1, sites
        self.post_dsb = sites[0]
        if omit_dsb:
            self.put(self.post_dsb, NOP, 4)
        self.stride = image.address('aarch64_device_cache_stride')
        u.hook_add(UC_HOOK_CODE, self.code)
        u.hook_add(UC_HOOK_MEM_WRITE, self.store, begin=REGISTERS, end=REGISTERS + 4095)
        u.hook_add(UC_HOOK_MEM_READ, self.load, begin=REGISTERS, end=REGISTERS + 4095)

    def get(self, address, width=8):
        return int.from_bytes(self.u.mem_read(address, width), 'little')

    def put(self, address, value, width=8):
        self.u.mem_write(address, int(value).to_bytes(width, 'little'))

    def ret(self, value):
        self.u.reg_write(UC_ARM64_REG_X0, value)
        self.u.reg_write(UC_ARM64_REG_PC, self.u.reg_read(UC_ARM64_REG_LR))

    def code(self, cpu, address, size, unused):
        self.stats['instructions'] += 1
        if address == self.stride:
            self.ret(64)
            return
        word = self.get(address, 4)
        if word == DSB_SY:
            self.stats['dsb'] += 1
            if self.posted.pending:
                # Charge a permitted CPU/interconnect completion delay. The
                # device may still be unready after this architectural point.
                self.clock += 7 * 24
                for at, value in self.posted.complete():
                    self.completion_times.append((at, value, self.clock))
                self.ready = RegisterLatency(self.clock, 12 * 24, 1, 1, stuck=self.ready.stuck)
                self.status = 0
        elif word & 0xfffff0ff == 0xd50330bf:
            self.stats['dmb'] += 1
        elif word & 0xffffffe0 in (0xd53be000, 0xd53be020):
            value = 24000000 if word & 0xffffffe0 == 0xd53be000 else self.clock
            if word & 0xffffffe0 == 0xd53be020:
                self.clock += 24
            self.u.reg_write(UC_ARM64_REG_X0 + (word & 31), value)
            self.u.reg_write(UC_ARM64_REG_PC, address + 4)

    def store(self, cpu, access, address, size, value, unused):
        assert size == 4
        self.stats['stores'] += 1
        self.posted.write(address, value)

    def load(self, cpu, access, address, size, value, unused):
        assert size == 4
        self.status = self.ready.apply(self.clock, self.status)
        self.put(address, self.status if address == REGISTERS + 4 else self.posted.read(address), 4)

    def call(self, name, *args):
        self.u.reg_write(UC_ARM64_REG_SP, RAM + 0xfe000)
        self.u.reg_write(UC_ARM64_REG_LR, STOP)
        for i, value in enumerate(args):
            self.u.reg_write(UC_ARM64_REG_X0 + i, value)
        self.u.emu_start(self.image.address(name, UNIT), STOP, count=200000)
        assert self.u.reg_read(UC_ARM64_REG_PC) == STOP, name
        return self.u.reg_read(UC_ARM64_REG_X0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    results = []
    # Vendor reset/FSCALE/cancel holds; the native adapter owns the same
    # architectural boundary for each register write.
    for offset, value, hold in ((0, 0x70b00, 5), (0, 0x70900, 20),
                                (0x3a8, 1, 20), (0x3a8, 0, 40),
                                (0, 0x70900, 50), (0x3a8, 8, 100)):
        m = Machine(args.image)
        assert m.call('write32', m.native, offset, value) == 1
        assert not m.posted.pending and m.posted.read(REGISTERS + offset) == value
        assert m.stats['stores'] == m.stats['dmb'] == m.stats['dsb'] == 1
        completed = m.completion_times[-1][2]
        assert m.ready.pending and m.status == 0  # DSB does not mean ready.
        m.call('delay', m.native, hold)
        assert m.clock - completed >= hold * 24
        results.append({'offset': offset, 'value': value, 'hold_us': hold,
                        'architectural_completed_at': completed,
                        'delay_returned_at': m.clock, 'stats': m.stats})

    m = Machine(args.image, stuck=True)
    assert m.call('write32', m.native, 0x3a8, 1) == 1
    m.clock += 100 * 24
    assert m.call('read32', m.native, 4, RAM + 0x10000) == 1
    assert m.get(RAM + 0x10000, 4) == 0 and m.ready.pending and not m.posted.pending

    m = Machine(args.image)
    assert m.call('write32', m.native, 1, 0x1234) == 0
    assert not m.posted.pending and not m.stats['stores'] and not m.stats['dmb'] and not m.stats['dsb']
    assert m.get(m.native + m.image.layouts['a733_npu_native']['failed'], 4) == 1
    assert m.call('write32', m.native, 0, 0x1234) == 0 and not m.stats['stores']

    m = Machine(args.image, omit_dsb=True)
    assert m.call('write32', m.native, 0, 0x70b00) == 1
    m.call('delay', m.native, 5)
    assert m.stats['dmb'] == 1 and m.stats['dsb'] == 0
    assert m.posted.pending and not m.completion_times
    # The same acceptance predicate used by positive cases rejects the
    # opcode-reverted adapter: a DMB and elapsed hold cannot complete a store.
    harness.refused(lambda: require_completed(m), 'store returned before architectural completion', AssertionError)
    args.output.mkdir(parents=True, exist_ok=False)
    (args.output / 'results.json').write_text(json.dumps({
        'image_sha256': hashlib.sha256(args.image.read_bytes()).hexdigest(),
        'physical_access': False, 'cases': results,
        'negative_control': m.stats,
        'scope': __doc__.strip()}, indent=2) + '\n')
    print('PASS linked NPU write: pre-DMB/store/post-DSB, six hold origins, separate stuck readiness, refused access and removed-DSB negative control')


def require_completed(machine):
    assert not machine.posted.pending, 'store returned before architectural completion'


if __name__ == '__main__':
    main()
