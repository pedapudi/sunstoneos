#!/usr/bin/env python3
"""Exercise the compiled AArch64 DCD's zero-length IN submission against MMIO memory."""
import argparse
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3, UC_ARM64_REG_X4, UC_ARM64_REG_LR, UC_ARM64_REG_SP, UC_ARM64_REG_PC

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("image")
args = parser.parse_args()
cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
cpu.mem_map(0x41000000, 0x200000)
cpu.mem_map(0x43000000, 0x10000)
cpu.mem_map(0x42000000, 0x5000)
cpu.mem_map(0x04100000, 0x1000)
with open(args.image, "rb") as stream:
    elf = ELFFile(stream)
    for segment in elf.iter_segments():
        if segment["p_type"] == "PT_LOAD":
            cpu.mem_write(segment["p_vaddr"], segment.data())
    function = elf.get_section_by_name(".symtab").get_symbol_by_name("dcd_edpt_xfer")[0]["st_value"]
for reg, value in ((UC_ARM64_REG_X0, 0), (UC_ARM64_REG_X1, 0x81),
                   (UC_ARM64_REG_X2, 0), (UC_ARM64_REG_X3, 0), (UC_ARM64_REG_X4, 0),
                   (UC_ARM64_REG_LR, 0x43000000), (UC_ARM64_REG_SP, 0x4300fff0)):
    cpu.reg_write(reg, value)
cpu.emu_start(function, 0x43000000, count=10000)
assert cpu.reg_read(UC_ARM64_REG_PC) == 0x43000000, "DCD did not return"
assert cpu.reg_read(UC_ARM64_REG_X0) == 1, "DCD refused the transfer"
assert cpu.mem_read(0x04100042, 1) == b"\x01", "Wrong endpoint selected"
assert int.from_bytes(cpu.mem_read(0x04100082, 2), "little") & 1, "Zero-length packet was not armed"
print("PASS: compiled DCD selects endpoint 1 and arms TXPKTRDY for a zero-length IN transfer.")
