"""Check wrapper control flow and FEL preservation with simulated blob calls.

This checks the wrapper's assembly only. It does not simulate the DDR hardware,
PMIC, clocks, or the blob functions replaced by the hooks below.
"""
import argparse
from pathlib import Path
import struct
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn.arm_const import *

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("build", type=Path)
args = parser.parse_args()
import json
# A capture variant patches dram_tpr13 bit 2 and dram_tpr10 bits 28:16 after
# the copy (tools/prepare-ddr.py --variant); the default build patches nothing.
variant = json.loads((args.build / "manifest.json").read_text()).get("variant")

for board_result, dram_result in ((0, 4096), (0, 0), (0xffffffff, 0)):
    cpu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    cpu.mem_map(0x40000, 0xc0000)
    original_fel = bytes(range(256)) * 48
    cpu.mem_write(0x47000, original_fel)
    cpu.mem_write(0x8e000, (args.build / "boot0-head.bin").read_bytes())
    cpu.mem_write(0x4a000, (args.build / "boot0-body.bin").read_bytes())
    cpu.mem_write(0x85000, (args.build / "ddr-entry.bin").read_bytes())
    cpu.reg_write(UC_ARM_REG_CPSR, 0x60000053)
    cpu.reg_write(UC_ARM_REG_SP, 0xf3000)
    cpu.reg_write(UC_ARM_REG_LR, 0xf0000)
    registers = (UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7,
                 UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11)
    for index, reg in enumerate(registers):
        cpu.reg_write(reg, 0x12340000 + index)
    words = list(range(1800, 1832))
    words[27], words[30] = 0xb25f0000, 0x10065
    selected = struct.pack("<32I", *words)
    expected = list(words)
    if variant:
        expected[30] &= ~4
    if variant == "init-only":
        expected[27] = (expected[27] & ~0x1fff0000) | 0x00010000
    expected = struct.pack("<32I", *expected)
    called = []

    def call(uc, address, size, data):
        if address not in (0x49898, 0x48fac, 0x49bdc):
            # The wrapper must not enter any other blob path.
            assert not 0x47000 <= address < 0x83000, hex(address)
            return
        called.append(address)
        if address == 0x48fac:
            uc.mem_write(0x47038, selected)
            result = board_result
        elif address == 0x49bdc:
            assert uc.reg_read(UC_ARM_REG_R0) == 0
            assert uc.reg_read(UC_ARM_REG_R1) == 0x831f8
            assert bytes(uc.mem_read(0x831f8, 128)) == expected
            result = dram_result
        else:
            result = 0
        uc.reg_write(UC_ARM_REG_R0, result)
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))

    def protect(uc, access, address, size, value, data):
        if 0x47000 <= address < 0x4a000:
            assert uc.reg_read(UC_ARM_REG_CPSR) & 0xc0 == 0xc0

    cpu.hook_add(UC_HOOK_CODE, call)
    cpu.hook_add(UC_HOOK_MEM_WRITE, protect)
    cpu.emu_start(0x85000, 0xf0000, count=100000)
    assert cpu.reg_read(UC_ARM_REG_PC) == 0xf0000
    assert cpu.reg_read(UC_ARM_REG_SP) == 0xf3000
    assert cpu.reg_read(UC_ARM_REG_CPSR) == 0x60000053
    for index, reg in enumerate(registers):
        assert cpu.reg_read(reg) == 0x12340000 + index
    assert bytes(cpu.mem_read(0x47000, 0x3000)) == original_fel
    result = struct.unpack("<4I", cpu.mem_read(0x86000, 16))
    assert result == (0x53524444, 5, dram_result if not board_result else 0, board_result)
    assert called == [0x49898, 0x48fac] + ([0x49bdc] if not board_result else [])

# Exercise the actual Thumb-to-ARM logging trampoline without the call hooks.
cpu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
cpu.mem_map(0x40000, 0xc0000)
cpu.mem_write(0x4a000, (args.build / "boot0-body.bin").read_bytes())
cpu.mem_write(0x85000, (args.build / "ddr-entry.bin").read_bytes())
cpu.reg_write(UC_ARM_REG_CPSR, 0x53)
cpu.reg_write(UC_ARM_REG_R0, ord("A"))
cpu.reg_write(UC_ARM_REG_LR, 0xf0000)
cpu.emu_start(0x4a541, 0xf0000, count=100)
assert cpu.mem_read(0xe0000, 1) == b"A"
assert struct.unpack("<I", cpu.mem_read(0x86018, 4))[0] == 1
print("PASS wrapper: success/failure returns, FEL state and registers restored, selected parameters copied, logging trampoline.")
