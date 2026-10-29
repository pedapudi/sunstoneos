#!/usr/bin/env python3
"""Check the linked PCIe MSI route and dispatch; no physical interrupt evidence."""
import argparse
import hashlib
import json
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_LR, UC_ARM64_REG_PSTATE, UC_ARM64_REG_TPIDR_EL1

D = 0x03400000
BIT = 1 << 25
STOP = 0x50fff000
REGISTERS = {D: 0x33, D+4: 7, D+0x94: 0x55aa0000, D+0xd14: 0xffffffff,
             D+0xc2c: 0xaaaaaaaa, D+0x4b8: 0x28384858}
EXPECTED = {D+0x94: 0x57aa0000, D+0xd14: 0xfdffffff,
            D+0xc2c: 0xaaa2aaaa, D+0x4b8: 0x2838d858, D+0x65c8: 0}
BLOCKED = {'enabled': D+0x114, 'pending': D+0x214, 'active': D+0x314,
           'rwp': D, 'unimplemented': D+4}
CASES = ['route'] + list(BLOCKED) + ['reject-'+hex(a) for a in EXPECTED] + ['dispatch', 'dispatch-expired']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--nvme', action='store_true', help='Check the optional NVMe MSI-X image')
    parser.add_argument('--case', choices=CASES, action='append')
    args = parser.parse_args()
    digest = hashlib.sha256(args.image.read_bytes()).hexdigest()
    with args.image.open('rb') as stream:
        elf = ELFFile(stream)
        parts = [(int(s['p_vaddr']), s.data()) for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']
        symbols = {}
        for s in elf.get_section_by_name('.symtab').iter_symbols():
            if s['st_value']:
                symbols.setdefault(s.name, set()).add(int(s['st_value']))
    def addr(name):
        values = symbols.get(name, set())
        assert len(values) == 1, (name, values)
        return next(iter(values))
    names = ['pcie_test_irq_route', 'vApplicationIRQHandler', 'pcie_test_application_irq',
             'a733_smp_counter', 'a733_smp_exit_privileged', 'diagnostic_deadline']
    syms = {n: addr(n.replace('pcie_test','pcie_nvme') if args.nvme else n) for n in names}
    args.output.mkdir(parents=True, exist_ok=True)
    for case in args.case or CASES:
        output = args.output/(case+'.json')
        assert not output.exists(), ('Preserve completed evidence', output)
        cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        cpu.reg_write(UC_ARM64_REG_PSTATE, 5 if case.startswith('dispatch') else 13)
        cpu.reg_write(UC_ARM64_REG_TPIDR_EL1, 0)
        for base, size in [(D, 0x10000), (0x41000000, 0x1200000), (0x50000000, 0x1000000)]:
            cpu.mem_map(base, size)
        for base, data in parts:
            assert 0x41000000 <= base and base+len(data) <= 0x42200000
            if data:
                cpu.mem_write(base, data)
        regs = dict(REGISTERS)
        if case in BLOCKED:
            regs[BLOCKED[case]] = (1 << 31) if case == 'rwp' else 4 if case == 'unimplemented' else BIT
        for address, value in regs.items():
            cpu.mem_write(address, value.to_bytes(4, 'little'))
        expiry = 5 if case == 'dispatch-expired' else 1000
        cpu.mem_write(syms['diagnostic_deadline'], expiry.to_bytes(8, 'little'))
        writes, calls, exits = [], [], []
        def write(uc, access, address, size, value, unused):
            assert address in EXPECTED, ('Unexpected GIC write', hex(address))
            writes.append((address, size, value))
        def read(uc, access, address, size, value, unused):
            if case.startswith('reject-') and address == int(case[7:], 16) and any(w[0] == address for w in writes):
                uc.mem_write(address, (EXPECTED[address] ^ 16).to_bytes(size, 'little'))
        def code(uc, address, size, unused):
            if address == syms['a733_smp_counter']:
                uc.reg_write(UC_ARM64_REG_X0, 10)
                uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_LR))
            elif address == syms['pcie_test_application_irq']:
                calls.append(address)
                uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_LR))
            elif address == syms['a733_smp_exit_privileged']:
                exits.append([uc.reg_read(UC_ARM64_REG_X0), uc.reg_read(UC_ARM64_REG_X1)])
                uc.reg_write(UC_ARM64_REG_PC, STOP)
        cpu.hook_add(UC_HOOK_MEM_WRITE, write, begin=D, end=D+0xffff)
        cpu.hook_add(UC_HOOK_MEM_READ, read, begin=D, end=D+0xffff)
        cpu.hook_add(UC_HOOK_CODE, code)
        cpu.reg_write(UC_ARM64_REG_SP, 0x50ffe000)
        cpu.reg_write(UC_ARM64_REG_LR, STOP)
        cpu.reg_write(UC_ARM64_REG_X0, 185 if case.startswith('dispatch') else 0x80000000)
        cpu.emu_start(syms['vApplicationIRQHandler'] if case.startswith('dispatch') else syms['pcie_test_irq_route'], STOP, count=50000)
        assert cpu.reg_read(UC_ARM64_REG_PC) == STOP, 'Instruction budget exhausted'
        if case == 'dispatch':
            assert calls == [syms['pcie_test_application_irq']] and not writes and not exits
        elif case == 'dispatch-expired':
            assert exits == [[3, 185]] and not writes and not calls
        elif case in BLOCKED:
            assert cpu.reg_read(UC_ARM64_REG_X0) == 0 and not writes
        else:
            assert [(a, v) for a, size, v in writes] == list(EXPECTED.items()), writes
            assert [size for a, size, v in writes] == [4, 4, 4, 4, 8]
            assert cpu.reg_read(UC_ARM64_REG_X0) == (1 if case == 'route' else 0)
        result = {'case': case, 'passed': True, 'image_sha256': digest, 'writes': writes,
                  'isr_calls': len(calls), 'retained_exits': exits, 'physical_access': False,
                  'scope': 'Actual linked route and PCIe dispatch branch. GIC registers, clock, ISR body and terminal exit are modeled. No scheduler, startup or physical interrupt execution.'}
        output.write_text(json.dumps(result, indent=2)+'\n')
        print(case, 'PASS')

if __name__ == '__main__':
    main()
