"""Execute the pinned ROM's early reset branches without contacting hardware.

This checks binary control flow. It does not model reset, register retention,
clocks, access permissions, USB initialization, or the physical Cortex-A55.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ
from unicorn.arm_const import (
    UC_ARM_REG_CPSR, UC_ARM_REG_PC, UC_CPU_ARM_CORTEX_A15,
)

ROM_HASH = "3a2a7d289951f61095afdabe7813b067295223ce588c422d64200fc7e4431eb1"
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("rom", type=Path, help="decoded fixs-brom/blob.hex")
args = parser.parse_args()
rom = args.rom.read_bytes()
if hashlib.sha256(rom).hexdigest() != ROM_HASH:
    raise SystemExit("ROM differs from the inspected A733 binary")


def check(name, start, redirect, rtc_flag, alternate, expected):
    cpu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    cpu.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_A15)
    for base, length in ((0, 0x11000), (0x40000, 0xc0000),
                         (0x07000000, 0x100000), (0x03000000, 0x1000)):
        cpu.mem_map(base, length)
    cpu.mem_write(0, rom)
    cpu.mem_write(0x070501c0, struct.pack("<II", redirect, 0x20))
    # The independent ROM dispatch at +0x1d4 takes precedence later in boot.
    # A nonzero conflict must be detected before preparing recovery.
    cpu.mem_write(0x070501d4, struct.pack("<II", alternate, 0x10000))
    cpu.mem_write(0x07090108, struct.pack("<I", rtc_flag))
    cpu.reg_write(UC_ARM_REG_CPSR, 0xd3)
    visited, mmio_reads = set(), set()

    def instruction(uc, address, size, unused):
        visited.add(address)
        if address == 0x1c4:
            # Unicorn rejects this NSACR write. Ignore only that instruction;
            # no following branch in this bounded prefix reads NSACR.
            # Security/access effects remain outside this check's evidence.
            uc.reg_write(UC_ARM_REG_PC, address + 4)
        if address in (0xb30, 0x2330, 0x10000):
            uc.emu_stop()

    def read(uc, access, address, size, value, unused):
        if address >= 0x02000000:
            mmio_reads.add(address)

    cpu.hook_add(UC_HOOK_CODE, instruction)
    cpu.hook_add(UC_HOOK_MEM_READ, read)
    cpu.emu_start(start, 0x11000, count=1000000)
    endpoint = cpu.reg_read(UC_ARM_REG_PC)
    if endpoint != expected:
        raise RuntimeError(f"{name}: stopped at {endpoint:#x}, expected {expected:#x}")
    if 0x07090108 in mmio_reads:
        raise RuntimeError("early ROM unexpectedly read the bootloader's FEL flag")
    if redirect and start == 0 and not {0x164, 0xcc, 0x20} <= visited:
        raise RuntimeError("direct redirect did not pass through the inspected branch")
    return {"case": name, "endpoint": hex(endpoint),
            "mmio_reads": [hex(address) for address in sorted(mmio_reads)]}


cases = [
    check("normal reset reaches boot dispatcher", 0, 0, 0, 0, 0xb30),
    check("bootloader RTC flag still reaches boot dispatcher",
          0, 0, 0x5aa5a55a, 0, 0xb30),
    check("explicit FEL entry reaches FEL dispatcher", 0x20, 0, 0, 0, 0x2330),
    check("CPU0 redirect reaches FEL dispatcher", 0, 0xfa50392f, 0, 0, 0x2330),
    check("alternate dispatch overrides redirect",
          0, 0xfa50392f, 0, 0xefe8, 0x10000),
]
print(json.dumps({"rom_sha256": ROM_HASH, "cases": cases,
                  "limitation": "Binary control flow only; hardware reset and retention untested.",
                  "emulation_exception": "NSACR write at 0x1c4 skipped."}, indent=2))
