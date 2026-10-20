#!/usr/bin/env python3
"""Check the linked SPI3 interrupt route and CPU dispatch with modeled GIC state."""
import argparse
import hashlib
import json
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_LR, UC_ARM64_REG_PSTATE, UC_ARM64_REG_TPIDR_EL1

D, BIT, STOP = 0x03400000, 1 << 27, 0x901ff000
REGISTERS = {D: 0x33, D+4: 7, D+0x84: 0x55aa0000,
             D+0xd04: 0xffffffff, D+0xc0c: 0xaaaaaaaa, D+0x438: 0x28384858}
EXPECTED = {D+0x84: 0x5daa0000, D+0xd04: 0xf7ffffff,
            D+0xc0c: 0xaa2aaaaa, D+0x438: 0xd8384858, D+0x61d8: 0}
CASES = ('route', 'group-readback', 'wrong-affinity', 'rwp', 'unimplemented',
         'dispatch-cpu0', 'dispatch-cpu1')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--case', action='append', choices=CASES)
    a = p.parse_args()
    digest = hashlib.sha256(a.image.read_bytes()).hexdigest()
    with a.image.open('rb') as stream:
        e = ELFFile(stream)
        parts = [(int(s['p_vaddr']), s.data()) for s in e.iter_segments() if s['p_type'] == 'PT_LOAD']
        symbols = {}
        for s in e.get_section_by_name('.symtab').iter_symbols():
            if s['st_value']:
                symbols.setdefault(s.name, set()).add(int(s['st_value']))

    def address(name):
        values = symbols.get(name, set())
        assert len(values) == 1, (name, values)
        return next(iter(values))

    syms = {n: address(n) for n in ('spi_irq_route', 'spi_interrupt', 'vApplicationIRQHandler',
                                    'a733_smp_counter', 'a733_smp_exit_privileged', 'diagnostic_deadline')}
    a.output.mkdir(parents=True, exist_ok=True)
    for case in a.case or CASES:
        output = a.output/(case+'.json')
        assert not output.exists(), ('Preserve completed evidence', output)
        u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        u.reg_write(UC_ARM64_REG_PSTATE, 5 if case.startswith('dispatch') else 13)
        u.reg_write(UC_ARM64_REG_TPIDR_EL1, int(case == 'dispatch-cpu1'))
        for base, size in ((D, 0x10000), (0x41000000, 0x1200000), (0x90000000, 0x200000)):
            u.mem_map(base, size)
        for base, data in parts:
            assert 0x41000000 <= base and base+len(data) <= 0x42200000
            if data:
                u.mem_write(base, data)
        regs = dict(REGISTERS)
        if case == 'rwp':
            regs[D] |= 1 << 31
        if case == 'unimplemented':
            regs[D+4] = 0
        for base, value in regs.items():
            u.mem_write(base, value.to_bytes(4, 'little'))
        u.mem_write(syms['diagnostic_deadline'], (1000).to_bytes(8, 'little'))
        writes, calls, exits = [], [], []

        def write(cpu, access, at, size, value, unused):
            assert at in EXPECTED, ('Unexpected GIC write', hex(at))
            writes.append((at, size, value))

        def read(cpu, access, at, size, value, unused):
            if case == 'group-readback' and at == D+0x84 and writes:
                cpu.mem_write(at, (EXPECTED[at] & ~BIT).to_bytes(size, 'little'))

        def ret(value):
            u.reg_write(UC_ARM64_REG_X0, value)
            u.reg_write(UC_ARM64_REG_PC, u.reg_read(UC_ARM64_REG_LR))

        def code(cpu, at, size, unused):
            if at == syms['spi_interrupt']:
                calls.append(at)
                ret(0)
            elif at == syms['a733_smp_counter']:
                ret(10)
            elif at == syms['a733_smp_exit_privileged']:
                exits.append([u.reg_read(UC_ARM64_REG_X0), u.reg_read(UC_ARM64_REG_X1)])
                u.reg_write(UC_ARM64_REG_PC, STOP)

        u.hook_add(UC_HOOK_MEM_WRITE, write, begin=D, end=D+0xffff)
        u.hook_add(UC_HOOK_MEM_READ, read, begin=D, end=D+0xffff)
        u.hook_add(UC_HOOK_CODE, code)
        u.reg_write(UC_ARM64_REG_SP, 0x901fe000)
        u.reg_write(UC_ARM64_REG_LR, STOP)
        u.reg_write(UC_ARM64_REG_X0, 59 if case.startswith('dispatch') else
                    0x80000100 if case == 'wrong-affinity' else 0x80000000)
        u.emu_start(syms['vApplicationIRQHandler'] if case.startswith('dispatch') else
                    syms['spi_irq_route'], STOP, count=50000)
        assert u.reg_read(UC_ARM64_REG_PC) == STOP, 'Instruction budget exhausted'
        if case == 'dispatch-cpu0':
            assert calls == [syms['spi_interrupt']] and not writes and not exits
        elif case == 'dispatch-cpu1':
            assert exits == [[4, 59+0x100]] and not calls and not writes
        elif case in ('wrong-affinity', 'rwp', 'unimplemented'):
            assert u.reg_read(UC_ARM64_REG_X0) == 0 and not writes
        else:
            assert [(at, value) for at, size, value in writes] == list(EXPECTED.items()), writes
            assert [size for at, size, value in writes] == [4, 4, 4, 4, 8]
            assert u.reg_read(UC_ARM64_REG_X0) == int(case == 'route')
        output.write_text(json.dumps(dict(
            passed=True, case=case, image_sha256=digest, writes=writes, physical_access=False,
            scope='Actual linked route and dispatcher. GIC, counter, ISR body and terminal exit modeled; no scheduler or physical interrupt delivery.'
        ), indent=2)+'\n')
        print(case, 'PASS', flush=True)


if __name__ == '__main__':
    main()
