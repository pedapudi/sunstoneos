#!/usr/bin/env python3
"""Execute read-only DMA observation with guarded apertures and delayed GIC state."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_LR, UC_ARM64_REG_PC, UC_ARM64_REG_SP
from harness import Image, hook_addresses, instruction_sites

CHECK_INPUTS = ['checks/dma-authority-control.c', 'checks/dma-authority-control.h',
                'checks/dma-authority-control.S', 'checks/dma-authority-control.ld',
                'cmake/dma-authority-control.cmake', 'include/a733_dma_regs.h']
SCRATCH, STOP = 0x81000000, 0x8101f000
GIC, DMA, ENABLE = 0x03400000, 0x04601000, 0x03400110


class Machine:
    def __init__(self, image, case):
        self.image, self.case = image, case
        self.cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        with image.path.open('rb') as stream:
            for segment in ELFFile(stream).iter_segments():
                if segment['p_type'] == 'PT_LOAD' and segment['p_memsz']:
                    start = int(segment['p_vaddr']) & ~4095
                    end = (int(segment['p_vaddr'] + segment['p_memsz']) + 4095) & ~4095
                    self.cpu.mem_map(start, end - start)
        for start, size in [(SCRATCH, 0x20000), (0x02002000, 0x2000),
                            (0x07010000, 0x1000), (0x07050000, 0x1000),
                            (GIC, 0x10000), (DMA, 0x1000)]:
            self.cpu.mem_map(start, size)
        for address, data in image.parts:
            self.cpu.mem_write(address, data)
        self.now, self.reads, self.writes, self.control_reads = 1000, [], [], 0
        self.ready = False
        self.put(0x02002704, 0 if case == 'cold' else 0x10001)
        self.put(0x020025e4, 4 if case == 'cold' else 0x40303)
        if case == 'reset': self.put(0x02002704, 1)
        if case == 'bus-gated': self.put(0x02002704, 0x10000)
        if case == 'mbus-gated': self.put(0x020025e4, 0x40302)
        if case == 'e902-bus': self.put(0x0701021c, 1)
        if case == 'e902-clock': self.put(0x07010210, 1 << 31)
        if case == 'e902-start': self.put(0x07050008, 1)
        self.put(DMA + 0x28, 4)
        self.put(DMA + 0x34, 1)
        self.put(DMA + 0x38, 0)
        self.put(DMA + 0x30, 1 if case == 'busy' else 0)
        self.cpu.reg_write(UC_ARM64_REG_SP, SCRATCH + 0x18000)
        self.cpu.reg_write(UC_ARM64_REG_LR, STOP)
        self.counters = {address: word & 31 for address in instruction_sites(image.parts,
            lambda word: word & 0xffffffe0 in (0xd53be020, 0xd53be040))
            for word in [int.from_bytes(self.cpu.mem_read(address, 4), 'little')]}
        hook_addresses(self.cpu, [*self.counters, STOP, image.address('sunstone_probe_fault')], self.code)
        for start, end in [(0x02002000, 0x02003fff), (0x07010000, 0x07010fff),
                           (0x07050000, 0x07050fff), (GIC, GIC + 0xffff), (DMA, DMA + 0xfff)]:
            self.cpu.hook_add(UC_HOOK_MEM_READ, self.read, begin=start, end=end)
            self.cpu.hook_add(UC_HOOK_MEM_WRITE, self.write, begin=start, end=end)
        self.fault = None

    def put(self, address, value):
        self.cpu.mem_write(address, struct.pack('<I', value))

    def code(self, uc, address, size, data):
        if address == STOP:
            uc.emu_stop()
        elif address == self.image.address('sunstone_probe_fault'):
            self.fault = uc.reg_read(UC_ARM64_REG_X0)
            uc.emu_stop()
        elif address in self.counters:
            uc.reg_write(self.counters[address] + UC_ARM64_REG_X0, self.now)
            self.now += 100
            uc.reg_write(UC_ARM64_REG_PC, address + 4)

    def read(self, uc, access, address, size, value, data):
        self.reads.append(address)
        if address == GIC:
            self.control_reads += 1
            self.ready = self.case != 'stuck-rwp' and self.control_reads >= 4
            self.put(GIC, 0x33 if self.ready else 0x80000033)
        if address == ENABLE:
            # Enable readback stays stale until distributor propagation ends.
            self.put(ENABLE, 1 << 15 if self.ready else 0)

    def write(self, uc, access, address, size, value, data):
        self.writes.append((address, size, value))

    def run(self):
        self.cpu.reg_write(UC_ARM64_REG_X0, SCRATCH)
        self.cpu.emu_start(self.image.address('dma_authority_collect'), STOP + 4, count=200000)
        fields = self.image.layouts['dma_authority_sample']
        def field(name):
            return struct.unpack('<I', self.cpu.mem_read(SCRATCH + fields[name], 4))[0]
        dma_reads = [a for a in self.reads if DMA <= a < DMA + 0x1000]
        assert not self.writes, self.writes
        if self.case == 'stuck-rwp':
            assert self.fault == 0x31 and self.now >= 3400 and ENABLE not in self.reads
            assert not dma_reads
        else:
            assert self.fault is None
            assert field('gic_enable') == 1 << 15, 'stale enable read before RWP completion'
            assert self.ready
            allowed = self.case in ('enabled', 'busy')
            assert field('aperture_sampled') == allowed
            assert len(dma_reads) == (100 if allowed else 0)
            assert field('autogate') == (4 if allowed else 0xffffffff)
            if allowed:
                assert field('busy') == (1 if self.case == 'busy' else 0)
            else:
                channel = self.cpu.mem_read(SCRATCH + fields['channel'], 16 * 6 * 4)
                assert channel == b'\xff' * len(channel)
        return dict(case=self.case, passed=True, dma_reads=len(dma_reads),
                    gic_polls=self.control_reads, mmio_writes=self.writes, fault=self.fault)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    image = Image(args.image, {'dma_authority_sample', 'dma_authority_result'})
    image.path = args.image
    cases = ('cold', 'reset', 'bus-gated', 'mbus-gated', 'e902-bus', 'e902-clock',
             'e902-start', 'enabled', 'busy', 'stuck-rwp')
    rows = [Machine(image, case).run() for case in cases]
    # Mutating the real wait branch must expose the stale readback.
    m = Machine(image, 'enabled')
    branches = instruction_sites(image.parts, lambda word:
        word & 0xfff8001f == 0x36f80001)  # TBZ w1,#31 in the inlined RWP wait.
    assert branches
    original = int.from_bytes(m.cpu.mem_read(branches[0], 4), 'little')
    displacement = (original >> 5) & 0x3fff
    if displacement & 0x2000:
        displacement -= 0x4000
    m.cpu.mem_write(branches[0], struct.pack('<I', 0x14000000 | (displacement & 0x3ffffff)))
    try:
        m.run()
    except AssertionError as error:
        assert 'stale enable read' in str(error), str(error)
        rows.append(dict(case='wait-removal-control', passed=True, refusal=str(error)))
    else:
        raise AssertionError('Removing distributor wait did not fail')
    (args.output / 'result.json').write_text(json.dumps(dict(passed=True, image=str(args.image),
        image_sha256=hashlib.sha256(args.image.read_bytes()).hexdigest(), cases=rows,
        scope='Actual collector code with modeled MMIO/timers; no board access or permission proof'), indent=2) + '\n')
    print('PASS read-only DMA collector:', len(rows), 'cases')


if __name__ == '__main__':
    main()
