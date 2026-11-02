#!/usr/bin/env python3
"""Interleave the linked phase closer and recorder around a preempted cutoff."""
import argparse
import importlib.util
from pathlib import Path
import struct
from unicorn import arm64_const as regs
from harness import Image as LayoutImage

spec = importlib.util.spec_from_file_location('tail', Path(__file__).with_name('latency-cost-tail.py'))
tail = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tail)


class Machine(tail.Machine):
    def __init__(self, image):
        super().__init__(image)
        self.counter = 200
        self.phase_reads = []

    def code(self, cpu, address, size, unused):
        ins = self.image.instructions.get(address)
        if address not in (tail.RETURN, self.stop_address) and ins and ins.mnemonic == 'mrs' and 'cntvct_el0' in ins.op_str:
            register = ins.op_str.split(',')[0].upper()
            cpu.reg_write(getattr(regs, 'UC_ARM64_REG_' + register), self.counter)
            cpu.reg_write(regs.UC_ARM64_REG_PC, address + 4)
            self.trace.append(dict(address=address, mnemonic=ins.mnemonic, operands=ins.op_str))
            return
        if ins and ins.mnemonic == 'ldar':
            base = ins.op_str.split('[')[1].split(']')[0].upper()
            loaded = cpu.reg_read(getattr(regs, 'UC_ARM64_REG_' + base))
            if any(self.image.symbols[name] <= loaded < self.image.symbols[name] + 24
                   for name in ('phase_start', 'phase_end')):
                self.phase_reads.append(len(self.trace))
        super().code(cpu, address, size, unused)

    def call(self, name, *args, stop=None):
        self.stopped = False
        self.cpu.reg_write(regs.UC_ARM64_REG_LR, tail.RETURN)
        for index, value in enumerate(args):
            self.cpu.reg_write(getattr(regs, f'UC_ARM64_REG_X{index}'), value)
        return self.run(self.image.symbols[name], stop)

    def qword(self, address):
        return struct.unpack('<Q', self.cpu.mem_read(address, 8))[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    args = parser.parse_args()
    image = tail.Image(args.image)
    layout = LayoutImage(args.image, {'histogram'}).layouts['histogram']
    assert layout['count'] == 1025 * 8 and layout['site'] == 1025 * 8 + 24
    machine = Machine(image)
    machine.call('ss_latency_init', 1000000)
    setter = image.symbols['ss_latency_phase_set']
    # The setter's first DSB must precede its counter read. Stop immediately
    # before DSB, with the release CLOSING store executed but not yet modeled
    # as globally visible to another core.
    dsb = next(a for a, i in sorted(image.instructions.items()) if a >= setter and i.mnemonic == 'dsb')
    counter = next(a for a, i in sorted(image.instructions.items()) if a > dsb and i.mnemonic == 'mrs' and 'cntvct_el0' in i.op_str)
    machine.call('ss_latency_phase_set', 1, stop=dsb)
    end = image.symbols['phase_end']
    assert machine.qword(end) == (1 << 64) - 1
    saved = machine.cpu.context_save()
    histogram = image.symbols['histogram'] + (7 * 11 + 10) * (1025 * 8 + 32) + layout['count']
    aggregate = image.symbols['ss_latency_results'] + 7 * 640 + 10 * 56
    def sample(start, finish):
        began = len(machine.trace)
        observed = len(machine.phase_reads)
        machine.call('ss_latency_record_at', 7, 10, start, finish, 123)
        assert len(machine.phase_reads) > observed
        assert any(t['mnemonic'] == 'isb' for t in machine.trace[began:machine.phase_reads[observed]])
    # Before publication completion, an observer can still see OPEN. Its
    # already-captured end precedes the future cutoff, so admission is safe.
    machine.cpu.mem_write(end, struct.pack('<Q', 0))
    sample(100, 150)
    assert machine.qword(histogram) == 1
    machine.cpu.mem_write(end, struct.pack('<Q', (1 << 64) - 1))
    machine.cpu.context_restore(saved)
    machine.stopped = False
    machine.run(dsb, counter)
    # DSB completed: all observers now see CLOSING. Preempt before cutoff.
    saved = machine.cpu.context_save()
    sample(100, 250)
    assert machine.qword(histogram) == 1
    machine.cpu.context_restore(saved)
    # Execute the cutoff read, then preempt before final end publication.
    machine.stopped = False
    machine.run(counter, counter + 4)
    saved = machine.cpu.context_save()
    sample(100, 300)
    assert machine.qword(histogram) == 1
    machine.cpu.context_restore(saved)
    machine.stopped = False
    machine.run(counter + 4)
    assert machine.qword(end) == 200
    sample(100, 250)  # Crossing after final publication is also excluded.
    assert machine.qword(histogram) == 1 and machine.qword(aggregate) == 4
    sample(100, 175)  # Delayed completion record wholly before cutoff survives.
    assert machine.qword(histogram) == 2 and machine.qword(aggregate) == 5
    # Negative control reproduces the former capture-before-publication gap
    # with the same linked setter: suppress its CLOSING store and DSB, capture
    # cutoff 200, then let another core finish at 300 before final publication.
    bad = Machine(image)
    bad.call('ss_latency_init', 1000000)
    assert image.instructions[dsb - 4].mnemonic == 'stlr'
    bad.cpu.mem_write(dsb - 4, struct.pack('<I', 0xd503201f))
    bad.cpu.mem_write(dsb, struct.pack('<I', 0xd503201f))
    bad.call('ss_latency_phase_set', 1, stop=counter + 4)
    assert bad.qword(end) == 0
    bad.call('ss_latency_record_at', 7, 10, 100, 300, 123)
    assert bad.qword(histogram) == 1, 'negative control did not expose the old misattribution'
    print('PASS linked phase close: delayed publication, preemption before/after cutoff, crossings and aggregate retention')


if __name__ == '__main__':
    main()
