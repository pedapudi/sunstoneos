#!/usr/bin/env python3
"""Check the linked USB host interrupt route and dispatch without board access."""
import argparse
import hashlib
import json
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_LR, UC_ARM64_REG_PSTATE, UC_ARM64_REG_TPIDR_EL1

D = 0x03400000
R = 0x03460000
STOP = 0x50fff000
SESSION = 0x50010000
BIT = 1 << 27
EXPECTED = {D+0x94: 0x5daa0000, D+0xd14: 0xf7ffffff,
            D+0xc2c: 0xaa2aaaaa, D+0x4b8: 0xd8384858, D+0x65d8: 0}
CASES = ['route', 'enabled', 'pending', 'active', 'rwp', 'lpi', 'redistributor-rwp',
         'wrong-affinity', 'wrong-el', 'expired', 'groups-enabled', 'nonhost',
         'ignored-priority', 'ignored-route', 'dispatch', 'dispatch-wrong-cpu',
         'dispatch-expired', 'dispatch-unexpected']


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    # Additional workload selectors require capable images; keep legacy defaults.
    p.add_argument('--case', action='append', choices=CASES+['route-dual', 'route-ss', 'route-ss-hid'])
    args = p.parse_args()
    digest = hashlib.sha256(args.image.read_bytes()).hexdigest()
    with args.image.open('rb') as f:
        elf = ELFFile(f)
        parts = [(int(s['p_vaddr']), s.data()) for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']
        symbols = {}
        for s in elf.get_section_by_name('.symtab').iter_symbols():
            if s['st_value']:
                symbols.setdefault(s.name, set()).add(int(s['st_value']))
        layouts = {}
        for cu in elf.get_dwarf_info().iter_CUs():
            for die in cu.iter_DIEs():
                name = die.attributes.get('DW_AT_name')
                if die.tag != 'DW_TAG_structure_type' or not name:
                    continue
                for child in die.iter_children():
                    member = child.attributes.get('DW_AT_name')
                    offset = child.attributes.get('DW_AT_data_member_location')
                    if member and offset:
                        layouts.setdefault((name.value, member.value), set()).add(offset.value)
    def addr(name):
        values = symbols.get(name, set())
        assert len(values) == 1, (name, values)
        return next(iter(values))
    def offset(struct, member):
        values = layouts[(struct.encode(), member.encode())]
        assert len(values) == 1, (struct, member, values)
        return next(iter(values))
    names = ['board_host_irq_admit', 'board_host_irq_route', 'board_session_snapshot',
             'board_host_irq', 'vApplicationIRQHandler', 'a733_smp_exit_privileged',
             'admission', 'diagnostic_deadline']
    syms = {n: addr(n) for n in names}
    workload = offset('a733_board_session', 'workload')
    deadline = offset('a733_smp_contract', 'startup_deadline')
    args.output.mkdir(parents=True, exist_ok=True)
    for case in args.case or CASES:
        cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        cpu.reg_write(UC_ARM64_REG_PSTATE, 13)
        # Production dispatch indexes the initialized runtime CPU ID, not MPIDR.
        cpu.reg_write(UC_ARM64_REG_TPIDR_EL1, 1 if case == 'dispatch-wrong-cpu' else 0)
        for base, size in [(D, 0x10000), (R, 0x20000), (0x41000000, 0x1200000), (0x50000000, 0x1000000)]:
            cpu.mem_map(base, size)
        for base, data in parts:
            assert 0x41000000 <= base and base+len(data) <= 0x42200000
            if data:
                cpu.mem_write(base, data)
        regs = {D: 0x30, D+4: 7, R: 0, R+8: 0, D+0x94: 0x55aa0000,
                D+0xd14: 0xffffffff, D+0xc2c: 0xaaaaaaaa,
                D+0x4b8: 0x28384858, D+0x65d8: 0x10000}
        blocked = {'enabled': D+0x114, 'pending': D+0x214, 'active': D+0x314}
        if case in blocked:
            regs[blocked[case]] = BIT
        if case == 'rwp': regs[D] |= 1 << 31
        if case == 'lpi': regs[R] = 1
        if case == 'redistributor-rwp': regs[R] = 8
        if case == 'groups-enabled': regs[D] |= 3
        ignored = {'ignored-priority': D+0x4b8, 'ignored-route': D+0x65d8}.get(case)
        writes, reads, calls = [], [], []
        terminal = None
        clock = 10
        expiry = 5 if case in ('expired', 'dispatch-expired') else 1000
        cpu.mem_write(syms['admission']+deadline, expiry.to_bytes(8, 'little'))
        cpu.mem_write(syms['diagnostic_deadline'], expiry.to_bytes(8, 'little'))
        selected_workload = {'nonhost': 2, 'route-dual': 4, 'route-ss': 6, 'route-ss-hid': 8}.get(case, 0)
        cpu.mem_write(SESSION+workload, selected_workload.to_bytes(4, 'little'))
        def read(uc, access, address, size, value, unused):
            reads.append(address)
            assert case != 'nonhost', 'Non-host session accessed the GIC'
            uc.mem_write(address, regs.get(address, 0).to_bytes(size, 'little'))
        def write(uc, access, address, size, value, unused):
            assert address in EXPECTED, ('Unexpected GIC write', hex(address))
            writes.append((address, size, value))
            if address != ignored:
                regs[address] = value
        def ret(value=0):
            cpu.reg_write(UC_ARM64_REG_X0, value)
            cpu.reg_write(UC_ARM64_REG_PC, cpu.reg_read(UC_ARM64_REG_LR))
        def code(uc, address, size, unused):
            nonlocal terminal, clock
            ins = int.from_bytes(uc.mem_read(address, 4), 'little')
            kind = ins & 0xffffffe0
            if kind in (0xd5384240, 0xd53800a0, 0xd53be020):
                # Model CurrentEL, MPIDR and counter explicitly; no cache/timing claim.
                value = 4 if case == 'wrong-el' else 12
                if kind == 0xd53800a0:
                    value = 0x80000100 if case in ('wrong-affinity', 'dispatch-wrong-cpu') else 0x80000000
                if kind == 0xd53be020:
                    value = clock
                    clock += 1
                uc.reg_write(UC_ARM64_REG_X0+(ins & 31), value)
                uc.reg_write(UC_ARM64_REG_PC, address+4)
                return
            if address == syms['board_session_snapshot']:
                ret(SESSION)
            elif address == syms['board_host_irq']:
                calls.append('host-handler')
                ret()
            elif address == syms['a733_smp_exit_privileged']:
                terminal = (uc.reg_read(UC_ARM64_REG_X0), uc.reg_read(UC_ARM64_REG_X1))
                uc.emu_stop()
        cpu.hook_add(UC_HOOK_MEM_READ, read, begin=D, end=R+0x1ffff)
        cpu.hook_add(UC_HOOK_MEM_WRITE, write, begin=D, end=R+0x1ffff)
        cpu.hook_add(UC_HOOK_CODE, code)
        cpu.reg_write(UC_ARM64_REG_SP, 0x50ffe000)
        cpu.reg_write(UC_ARM64_REG_LR, STOP)
        interrupt = 188 if case == 'dispatch-unexpected' else 187
        cpu.reg_write(UC_ARM64_REG_X0, interrupt)
        entry = syms['vApplicationIRQHandler' if case.startswith('dispatch') else 'board_host_irq_route']
        cpu.emu_start(entry, STOP, count=50000)
        if case in ('dispatch-wrong-cpu', 'dispatch-expired', 'dispatch-unexpected'):
            assert terminal == ((3, interrupt) if case == 'dispatch-expired' else (4, interrupt+0x100)), terminal
            assert not calls and not writes
        else:
            assert cpu.reg_read(UC_ARM64_REG_PC) == STOP, 'Instruction budget exhausted'
            if case == 'dispatch':
                assert calls == ['host-handler'] and not writes
            elif case in ('route', 'route-dual', 'route-ss', 'route-ss-hid', 'ignored-priority', 'ignored-route'):
                assert [(a, v) for a, s, v in writes] == list(EXPECTED.items()), writes
                assert [s for a, s, v in writes] == [4, 4, 4, 4, 8]
                assert cpu.reg_read(UC_ARM64_REG_X0) == (case in ('route', 'route-dual', 'route-ss', 'route-ss-hid'))
            else:
                assert not writes
                assert cpu.reg_read(UC_ARM64_REG_X0) == (case == 'nonhost')
        result = {'case': case, 'passed': True, 'image_sha256': digest, 'workload': selected_workload, 'writes': writes,
                  'terminal': terminal, 'physical_access': False,
                  'scope': 'Linked EL3 route and dispatch; validated-session provider, GIC registers and ISR body modeled. No scheduling, interrupt-delivery or recovery qualification.'}
        (args.output/(case+'.json')).write_text(json.dumps(result, indent=2)+'\n')
        print(case, 'PASS')


if __name__ == '__main__':
    main()
