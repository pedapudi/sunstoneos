#!/usr/bin/env python3
"""Execute the AArch64 resource adapter with modeled MMIO and I2C transport."""
import argparse
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_X0, UC_ARM64_REG_LR, UC_ARM64_REG_CP_REG

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('elf', type=Path)
a = p.parse_args()
u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
u.mem_map(0x40000000, 0x200000)
u.mem_map(0x00200000, 0x1000)
u.mem_map(0x02002000, 0x2000)
u.mem_map(0x07010000, 0x1000)
u.mem_map(0x07065000, 0x1000)
u.mem_map(0x07090000, 0x1000)
u.mem_map(0x07083000, 0x1000)
with a.elf.open('rb') as f:
    elf = ELFFile(f)
    for segment in elf.iter_segments():
        if segment['p_type'] == 'PT_LOAD':
            u.mem_write(segment['p_vaddr'], segment.data())
    entry = elf.get_section_by_name('.symtab').get_symbol_by_name('native_check')[0]['st_value']
u.cpr_write(3, 3, 14, 0, 0, 24000000)
clock = [100]
def instruction(uc, address, size, context):
    word = int.from_bytes(uc.mem_read(address, 4), "little")
    if word & 0xffffffe0 == 0xd53be020:  # MRS Xt, CNTPCT_EL0
        target = word & 31
        assert target < 29
        uc.reg_write(UC_ARM64_REG_X0 + target, clock[0] * 24)
        clock[0] += 1
        uc.reg_write(UC_ARM64_REG_PC, address + 4)
u.hook_add(UC_HOOK_CODE, instruction)
u.reg_write(UC_ARM64_REG_SP, 0x40200000 - 16)
u.reg_write(UC_ARM64_REG_LR, 0x401ff000)
u.emu_start(entry, 0x401ff000, timeout=10000000, count=200000)
assert u.reg_read(UC_ARM64_REG_PC) == 0x401ff000, 'execution budget exhausted'
assert u.reg_read(UC_ARM64_REG_X0) == 0, f"native check failed at line {u.reg_read(UC_ARM64_REG_X0)}"
print('PASS AArch64 native resource adapter: MMIO gates, six-register I2C binding, retained transport/restoration failures and lock admission')
