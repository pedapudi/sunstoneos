"""Inspect ROM-owned reset logging in the captured A733 binary; no board access."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn.arm_const import (UC_CPU_ARM_CORTEX_A15, UC_ARM_REG_CPSR, UC_ARM_REG_SP,
                               UC_ARM_REG_LR, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_PC)

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('rom', type=Path)
args = parser.parse_args()
rom = args.rom.read_bytes()
rom_hash = hashlib.sha256(rom).hexdigest()
if rom_hash != 'a8e31821e8a1611d75867825914c58373c732742cb8f2235e8168c1d7e93b5aa':
    raise SystemExit('Expected the pinned physical A733 ROM capture.')
LOG = 0x07090304


def execute(name, start, before, expected, endpoint, arguments=None):
    cpu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    cpu.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_A15)
    for base, size in ((0, 0x11000), (0x40000, 0xc0000), (0x07000000, 0x100000)):
        cpu.mem_map(base, size)
    cpu.mem_write(0, rom)
    cpu.mem_write(LOG, struct.pack('<I', before))
    cpu.mem_write(0x070501c0, struct.pack('<II', 0xfa50392f, 0x20))
    cpu.reg_write(UC_ARM_REG_CPSR, 0xd3)
    cpu.reg_write(UC_ARM_REG_SP, 0xef000)
    cpu.reg_write(UC_ARM_REG_LR, endpoint)
    if arguments:
        cpu.reg_write(UC_ARM_REG_R0, arguments[0])
        cpu.reg_write(UC_ARM_REG_R1, arguments[1])
    writes = []

    def record(uc, access, address, size, value, unused):
        if address == LOG:
            writes.append({'pc': hex(uc.reg_read(UC_ARM_REG_PC)), 'value': hex(value)})

    def stop(uc, address, size, unused):
        if address == endpoint:
            uc.emu_stop()

    cpu.hook_add(UC_HOOK_MEM_WRITE, record)
    cpu.hook_add(UC_HOOK_CODE, stop)
    cpu.emu_start(start, 0x11000, count=1000000)
    after = struct.unpack('<I', cpu.mem_read(LOG, 4))[0]
    if cpu.reg_read(UC_ARM_REG_PC) != endpoint or after != expected:
        raise RuntimeError(f'{name}: endpoint or register mutation differs: {after:#x}')
    if start == 0x20 and writes:
        raise RuntimeError('Forced-FEL prefix unexpectedly changed reset log.')
    return {'case': name, 'start': hex(start), 'endpoint': hex(endpoint),
            'before': hex(before), 'after': hex(after), 'writes': writes}

cases = [
    execute('reset selects low half when bit 15 is clear', 0, 0x92341234,
            0x12348001, 0x17c),
    execute('reset selects high half when bit 15 is set', 0, 0x92349234,
            0x80011234, 0x17c),
    execute('forced FEL entry bypasses reset-log mutation', 0x20, 0x92349234,
            0x92349234, 0x17c),
    execute('software log helper changes active low-half record', 0xd14, 0x12348001,
            0x12348702, 0x40000, (7, 2)),
    execute('software log helper changes active high-half record', 0xd14, 0x80011234,
            0x87021234, 0x40000, (7, 2)),
]
print(json.dumps({'rom_sha256': rom_hash, 'cases': cases,
                  'scope': 'Executed reset prefix and logging helper only; no skipped instructions.',
                  'limitations': ['Synthetic MMIO and register retention.',
                                  'Execution stops before FEL initialization and USB operation.',
                                  'ROM-written log values do not identify the hardware reset source.']}, indent=2))
