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

ROM_PROFILES = {
    "3a2a7d289951f61095afdabe7813b067295223ce588c422d64200fc7e4431eb1":
        ("published ROM", 0x2330),
    "a8e31821e8a1611d75867825914c58373c732742cb8f2235e8168c1d7e93b5aa":
        ("physical board ROM", 0x2514),
}
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("rom", type=Path, help="pinned published or captured physical ROM")
args = parser.parse_args()
rom = args.rom.read_bytes()
rom_hash = hashlib.sha256(rom).hexdigest()
if rom_hash not in ROM_PROFILES:
    raise SystemExit("ROM differs from the inspected A733 binary")
profile, fel_entry = ROM_PROFILES[rom_hash]


def check(name, start, redirect, rtc_flag, alternate, expected,
          boot_samples=None, secure=0):
    cpu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    cpu.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_A15)
    for base, length in ((0, 0x11000), (0x40000, 0xc0000),
                         (0x07000000, 0x100000), (0x03000000, 0x10000),
                         (0x08020000, 0x1000)):
        cpu.mem_map(base, length)
    cpu.mem_write(0, rom)
    cpu.mem_write(0x070501c0, struct.pack("<II", redirect, 0x20))
    # The independent ROM dispatch at +0x1d4 takes precedence later in boot.
    # A nonzero conflict must be detected before preparing recovery.
    cpu.mem_write(0x070501d4, struct.pack("<II", alternate, 0x10000))
    cpu.mem_write(0x07090108, struct.pack("<I", rtc_flag))
    cpu.mem_write(0x030060a0, struct.pack("<I", secure))
    cpu.reg_write(UC_ARM_REG_CPSR, 0xd3)
    visited, mmio_reads = set(), set()
    sampled = []
    endpoints = (0xb30, fel_entry, 0x10000) if boot_samples is None else (
        0xb88, fel_entry, 0x10000)

    def instruction(uc, address, size, unused):
        visited.add(address)
        if address == 0x1c4:
            # Unicorn rejects this NSACR write. Ignore only that instruction;
            # no following branch in this bounded prefix reads NSACR.
            # Security/access effects remain outside this check's evidence.
            uc.reg_write(UC_ARM_REG_PC, address + 4)
        if address in endpoints:
            uc.emu_stop()

    def read(uc, access, address, size, value, unused):
        if boot_samples is not None and address == 0x03000024:
            if size != 4 or len(sampled) >= len(boot_samples):
                raise RuntimeError("unexpected boot-input read")
            level = boot_samples[len(sampled)]
            sampled.append(level)
            uc.mem_write(address, struct.pack("<I", level << 8))
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
    if boot_samples is not None and sampled != boot_samples:
        raise RuntimeError("boot-input sampling differs from the inspected sequence")
    return {"case": name, "endpoint": hex(endpoint),
            "mmio_reads": [hex(address) for address in sorted(mmio_reads)],
            "boot_input_samples": sampled}


cases = [
    check("normal reset reaches boot dispatcher", 0, 0, 0, 0, 0xb30),
    check("bootloader RTC flag still reaches boot dispatcher",
          0, 0, 0x5aa5a55a, 0, 0xb30),
    check("explicit FEL entry reaches FEL dispatcher", 0x20, 0, 0, 0, fel_entry),
    check("CPU0 redirect reaches FEL dispatcher", 0, 0xfa50392f, 0, 0, fel_entry),
    check("alternate dispatch overrides redirect",
          0, 0xfa50392f, 0, 0xefe8, 0x10000),
]
if profile == "physical board ROM":
    cases.extend([
        check("four low boot-input samples select FEL without redirect",
              0, 0, 0, 0, fel_entry, [0, 0, 0, 0]),
        check("four high boot-input samples continue toward storage boot",
              0, 0, 0, 0, 0xb88, [1, 1, 1, 1]),
        check("one high boot-input sample continues toward storage boot",
              0, 0, 0, 0, 0xb88, [0, 0, 1, 0]),
        check("low boot input selects FEL with secure indicator set",
              0, 0, 0, 0, fel_entry, [0, 0, 0, 0], secure=1),
    ])
print(json.dumps({"rom_sha256": rom_hash, "profile": profile, "cases": cases,
                  "limitation": "Binary control flow only; hardware reset and retention untested.",
                  "emulation_exception": "NSACR write at 0x1c4 skipped."}, indent=2))
