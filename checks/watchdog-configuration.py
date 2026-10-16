"""Check the compiled SRAM diagnostic's ownership and restoration behavior.

No board access, clock model, watchdog expiry, or reset retention is involved.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm_const import (
    UC_CPU_ARM_CORTEX_A15, UC_ARM_REG_R0, UC_ARM_REG_R12,
    UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_CPSR,
)

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("image", type=Path)
args = parser.parse_args()
image = args.image.read_bytes()
if not 0 < len(image) <= 0x800:
    raise SystemExit("SRAM image exceeds reserved interval")

FLAG, ENTRY, ALT = 0x070501c0, 0x070501c4, 0x070501d4
CONFIG, MODE, OUTPUT = 0x02050010, 0x02050014, 0x02050018
MAGIC = 0xfa50392f


def check(case, expected, fault=None, initial=None, request=0x57444346):
    cpu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    cpu.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_A15)
    cpu.mem_map(0x40000, 0xc0000)
    cpu.mem_map(0x02050000, 0x1000)
    cpu.mem_map(0x07050000, 0x1000)
    cpu.mem_write(0x52000, image)
    cpu.mem_write(0x52800, struct.pack("<16I", request, *([0] * 15)))
    cpu.mem_write(0x52ff0, bytes([0xa5]) * 16)
    cpu.mem_write(0x54000, bytes([0x5a]) * 16)
    registers = {FLAG: 0, ENTRY: 0x12345678, ALT: 0,
                 CONFIG: 0x22, MODE: 0xa2, OUTPUT: 7}
    registers.update(initial or {})
    before = registers.copy()
    writes = []
    returned = []
    general = list(range(UC_ARM_REG_R0, UC_ARM_REG_R12 + 1))
    saved = {reg: 0x98760000 + i for i, reg in enumerate(general)}
    saved.update({UC_ARM_REG_SP: 0xef000, UC_ARM_REG_LR: 0x60000,
                  UC_ARM_REG_CPSR: 0xa0000153})
    cpu.reg_write(UC_ARM_REG_CPSR, saved[UC_ARM_REG_CPSR])
    for reg, value in saved.items():
        cpu.reg_write(reg, value)

    def read(uc, access, address, size, value, unused):
        if address < 0x02000000:
            return
        if address not in registers or size != 4:
            raise RuntimeError(f"unexpected register read {address:#x}")
        uc.mem_write(address, struct.pack("<I", registers[address]))

    def write(uc, access, address, size, value, unused):
        if address < 0x02000000:
            return
        if address not in (FLAG, ENTRY, CONFIG, OUTPUT) or size != 4:
            raise RuntimeError(f"forbidden register write {address:#x}")
        writes.append((address, value))
        if address in (CONFIG, OUTPUT):
            if value >> 16 != 0x16aa:
                raise RuntimeError("unkeyed watchdog write")
            value &= 0xffff
        if fault == (address, value):
            return
        if address == ENTRY and registers[FLAG] == MAGIC:
            raise RuntimeError("entry modified while redirect active")
        if address == FLAG and value == MAGIC and registers[ENTRY] != 0x20:
            raise RuntimeError("redirect activated before its entry")
        registers[address] = value
        if fault == "watchdog became active" and (address, value) == (OUTPUT, 7):
            registers[MODE] |= 1

    def instruction(uc, address, size, unused):
        if address == 0x60000:
            returned.append(True)
            uc.emu_stop()

    cpu.hook_add(UC_HOOK_MEM_READ, read)
    cpu.hook_add(UC_HOOK_MEM_WRITE, write)
    cpu.hook_add(UC_HOOK_CODE, instruction)
    cpu.emu_start(0x52000, 0x100000, count=20000)
    changed = {reg: (hex(value), hex(cpu.reg_read(reg))) for reg, value in saved.items()
               if cpu.reg_read(reg) != value}
    if not returned or changed:
        raise RuntimeError(f"{case}: FEL caller state was not preserved: {changed}")
    result = struct.unpack("<16I", cpu.mem_read(0x52800, 64))
    if result[2] != expected:
        raise RuntimeError(f"{case}: unexpected result {result}")
    if request != 0x57444346:
        if writes or any(result[1:]):
            raise RuntimeError("invalid request changed state")
    elif result[1] != 0x57525031:
        raise RuntimeError("missing diagnostic signature")
    if expected in (0, 2, 3, 4) and registers != before:
        raise RuntimeError(f"{case}: register restoration failed")
    if expected == 2 and writes:
        raise RuntimeError("existing owner was modified")
    if expected == 5:
        if result[14] != 1 or registers[ENTRY] != 0x20:
            raise RuntimeError("failed restoration lost recovery context")
    if (cpu.mem_read(0x52ff0, 16) != bytes([0xa5]) * 16 or
            cpu.mem_read(0x54000, 16) != bytes([0x5a]) * 16):
        raise RuntimeError("private stack guard changed")
    return {"case": case, "status": result[2], "register_writes": len(writes),
            "context_retained": bool(result[14])}


cases = [
    check("configuration and restoration", 0),
    check("invalid request", 0, request=0),
    check("active watchdog owner", 2, initial={MODE: 0xa3}),
    check("existing redirect owner", 2, initial={FLAG: MAGIC}),
    check("alternate dispatch owner", 2, initial={ALT: 0xefe8}),
    check("ignored entry write", 3, fault=(ENTRY, 0x20)),
    check("ignored redirect flag", 3, fault=(FLAG, MAGIC)),
    check("ignored watchdog configuration", 4, fault=(CONFIG, 0x21)),
    check("ignored reset output", 4, fault=(OUTPUT, 2)),
    check("failed redirect disable", 5, fault=(FLAG, 0)),
    check("failed entry restoration", 5, fault=(ENTRY, 0x12345678)),
    check("watchdog became active before restoration", 5,
          fault="watchdog became active"),
]
print(json.dumps({"image_sha256": hashlib.sha256(image).hexdigest(),
                  "cases": cases,
                  "limitation": "Instruction and register model; no hardware reset or retention."}, indent=2))
