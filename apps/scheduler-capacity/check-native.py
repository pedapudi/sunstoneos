#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute both compiled AArch64 kernels in an instruction model."""
import argparse
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_CPACR_EL1, UC_ARM64_REG_X0, UC_ARM64_REG_X30, UC_ARM64_REG_SP
import host

p = argparse.ArgumentParser()
p.add_argument('elf', type=Path)
a = p.parse_args()
with a.elf.open('rb') as f:
    elf = ELFFile(f)
    segments = [(s['p_vaddr'], s.data()) for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']
    symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
expected = host.expected_digest()
for kernel in ('capacity_integer', 'capacity_neon'):
    machine = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    machine.mem_map(0x100000, 0x100000)
    machine.mem_map(0x200000, 0x100000)
    for address, data in segments:
        machine.mem_write(address, data)
    machine.reg_write(UC_ARM64_REG_SP, 0x2ff000)
    machine.reg_write(UC_ARM64_REG_X0, 0x200000)
    machine.reg_write(UC_ARM64_REG_X30, 0x1ff000)
    machine.reg_write(UC_ARM64_REG_CPACR_EL1, 3 << 20)
    machine.emu_start(symbols[kernel], 0x1ff000, count=10000000)
    assert machine.reg_read(UC_ARM64_REG_X0) == expected, kernel
print(f'AArch64 scalar and NEON instructions produce independent digest {expected:016x}; instruction model only')
