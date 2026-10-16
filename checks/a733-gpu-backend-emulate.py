#!/usr/bin/env python3
"""Execute native GPU backend accesses with modeled translations and power admission."""
import argparse
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE, UC_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_X0, UC_ARM64_REG_LR

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('elf', type=Path)
mode = p.add_mutually_exclusive_group()
mode.add_argument('--clock-only', action='store_true')
mode.add_argument('--event-only', action='store_true')
a = p.parse_args()
u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
for address, size in [(0x40000000, 0x400000), (0x00200000, 0x1000),
                      (0x01800000, 0xb000), (0x03400000, 0x10000)]:
    u.mem_map(address, size)
with a.elf.open('rb') as f:
    elf = ELFFile(f)
    for segment in elf.iter_segments():
        if segment['p_type'] == 'PT_LOAD':
            u.mem_write(segment['p_vaddr'], segment.data())
    symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
u.cpr_write(3, 3, 14, 0, 0, 24000000)
clock = [100]
def word(name):
    return int.from_bytes(u.mem_read(symbols[name], 4), 'little')
def instruction(uc, address, size, context):
    # Ordinary RAM initialization/copying is outside the MMIO binding check.
    if address in (symbols.get('memset'), symbols.get('memcpy'), symbols.get('memcmp')):
        target = uc.reg_read(UC_ARM64_REG_X0)
        source = uc.reg_read(UC_ARM64_REG_X0 + 1)
        count = uc.reg_read(UC_ARM64_REG_X0 + 2)
        assert count <= 0x100000
        result = target
        if address == symbols.get('memset'):
            uc.mem_write(target, bytes([source & 255]) * count)
        elif address == symbols.get('memcpy'):
            uc.mem_write(target, bytes(uc.mem_read(source, count)))
        else:
            left, right = uc.mem_read(target, count), uc.mem_read(source, count)
            result = next((x-y for x,y in zip(left, right) if x != y), 0)
        uc.reg_write(UC_ARM64_REG_X0, result & ((1 << 64)-1))
        uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_LR))
        return
    opcode = int.from_bytes(uc.mem_read(address, 4), 'little')
    masked, register = opcode & 0xffffffe0, opcode & 31
    value = None
    if masked == 0xd53be020:  # CNTPCT_EL0
        value = clock[0] * 24
        clock[0] += 1
    elif masked == 0xd5384240:  # CurrentEL
        value = 8 if word('mapping_mode') == 4 else 4
    elif masked == 0xd5381000:  # SCTLR_EL1
        value = 5
    elif masked == 0xd53b0020:  # CTR_EL0
        value = 0x04040000
    elif masked in (0xd5087800, 0xd5087820):  # AT S1E1R/W
        virtual = uc.reg_read(UC_ARM64_REG_X0 + register)
        device = 0x01800000 <= virtual < 0x0180b000 or 0x03400000 <= virtual < 0x03410000
        attributes = 0 if device else 0x44 if symbols['resource_arena'] <= virtual < symbols['resource_arena'] + 64*4096 else 0xff
        mode = word('mapping_mode')
        physical = virtual
        if device and mode == 1:
            attributes = 0xff
        if device and mode == 2:
            physical += 0x1000
        par = (physical & 0x000ffffffffff000) | attributes << 56 | 3 << 7
        if device and mode == 3 and masked == 0xd5087820:
            par = 1
        uc.cpr_write(3, 0, 7, 4, 0, par)
        uc.reg_write(UC_ARM64_REG_PC, address + 4)
    if value is not None:
        assert register < 29
        uc.reg_write(UC_ARM64_REG_X0 + register, value)
        uc.reg_write(UC_ARM64_REG_PC, address + 4)
def access(uc, operation, address, size, value, context):
    if not word('observe'):
        return
    if 0x01800000 <= address < 0x0180b000:
        assert word('power_enabled'), f'GPU access before power ON at PC {uc.reg_read(UC_ARM64_REG_PC):#x}, address {address:#x}, operation {operation}'
        writing = operation == UC_MEM_WRITE
        if writing:
            assert word('identity_confirmed'), 'GPU write before identity admission'
        elif word('late_identity') and address == 0x01800020:
            clock[0] += 100001
        expected = word('expected_access')
        if expected:
            assert expected == (2 if writing else 1)
            assert address - 0x01800000 == word('expected_offset')
            assert size == word('expected_width'), 'wrong device transaction width'
            if writing:
                wanted = int.from_bytes(uc.mem_read(symbols['expected_value'], 8), 'little')
                assert value & ((1 << (size*8)) - 1) == wanted
        count = word('mmio_count') + 1
        uc.mem_write(symbols['mmio_count'], count.to_bytes(4, 'little'))
u.hook_add(UC_HOOK_CODE, instruction)
u.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, access)
u.reg_write(UC_ARM64_REG_SP, 0x40400000 - 16)
u.reg_write(UC_ARM64_REG_LR, 0x403ff000)
u.emu_start(symbols['backend_clock_check' if a.clock_only else 'backend_event_check' if a.event_only else 'backend_check'], 0x403ff000, timeout=20000000, count=5000000)
assert u.reg_read(UC_ARM64_REG_PC) == 0x403ff000, 'execution budget exhausted'
assert u.reg_read(UC_ARM64_REG_X0) == 0, f"backend check failed at line {u.reg_read(UC_ARM64_REG_X0)}"
print('PASS native GPU backend: actual firmware event receive and restricted queue-owned destination' if a.event_only else 'PASS native GPU backend: private platform/controller clock regression refusal' if a.clock_only else 'PASS native GPU backend: identity, MMIO trace widths, memory/GIC composition, mapping refusal and retained isolation failures')
