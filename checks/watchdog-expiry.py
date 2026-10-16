"""Execute the SRAM image in an instruction model, without modeling real reset."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm_const import (UC_CPU_ARM_CORTEX_A15, UC_ARM_REG_R0, UC_ARM_REG_R12,
                               UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_CPSR)
from elftools.elf.elffile import ELFFile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('image', type=Path)
parser.add_argument('elf', type=Path)
args = parser.parse_args()
image = args.image.read_bytes()
with args.elf.open('rb') as stream:
    elf = ELFFile(stream)
    wait_address = next(symbol['st_value'] for symbol in elf.get_section_by_name('.symtab').iter_symbols()
                        if symbol.name == 'watchdog_expiry_wait')
assert 0 < len(image) <= 0x800
FLAG, ENTRY, ALT, INPUT = 0x070501c0, 0x070501c4, 0x070501d4, 0x03000024
CONFIG, MODE, OUTPUT, FEED, RTC = 0x02050010, 0x02050014, 0x02050018, 0x0205000c, 0x07090108


def check(name, command=1, status=0, fault=None, initial=None, valid=True):
    cpu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    cpu.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_A15)
    for base, size in ((0x40000, 0xc0000), (0x02050000, 0x1000),
                       (0x07050000, 0x1000), (0x07090000, 0x1000), (0x03000000, 0x1000)):
        cpu.mem_map(base, size)
    cpu.mem_write(0x52000, image)
    request = [0x57445850, command, 0x12345678, 0xedcba987] + [0] * 12
    if not valid:
        request[3] = 0
    cpu.mem_write(0x52800, struct.pack('<16I', *request))
    cpu.mem_write(0x52ff0, b'A' * 16)
    cpu.mem_write(0x54000, b'B' * 16)
    registers = {FLAG: 0, ENTRY: 0x12340000, ALT: 0, INPUT: 1,
                 CONFIG: 0x22, MODE: 0xa2, OUTPUT: 7, RTC: 0x76543210}
    registers.update(initial or {})
    before = registers.copy()
    saved = {reg: 0x98760000 + i for i, reg in enumerate(range(UC_ARM_REG_R0, UC_ARM_REG_R12 + 1))}
    saved.update({UC_ARM_REG_SP: 0xef000, UC_ARM_REG_LR: 0x60000, UC_ARM_REG_CPSR: 0xa0000013})
    cpu.reg_write(UC_ARM_REG_CPSR, saved[UC_ARM_REG_CPSR])
    for reg, value in saved.items():
        cpu.reg_write(reg, value)
    writes, stopped = [], []

    def read(uc, access, address, size, value, unused):
        if address < 0x02000000:
            return
        assert address in registers and size == 4, (name, hex(address))
        uc.mem_write(address, struct.pack('<I', registers[address]))

    def write(uc, access, address, size, value, unused):
        if address < 0x02000000:
            return
        assert address in (FLAG, ENTRY, CONFIG, MODE, OUTPUT, FEED, RTC) and size == 4
        writes.append((address, value))
        if address == FEED:
            assert value == 0x14af
            return
        if address in (CONFIG, MODE, OUTPUT):
            assert value >> 16 == 0x16aa
            value &= 0xffff
        if fault == (address, value):
            return
        if address == ENTRY:
            assert registers[FLAG] == 0
        if address == FLAG and value:
            assert registers[ENTRY] == 0x20
        if address == MODE and value & 1:
            assert registers[FLAG] == 0xfa50392f and registers[RTC] == 0x5aa5a55a
            assert registers[CONFIG] & 3 == 1 and registers[OUTPUT] == 2
            assert value & 0xf1 == 0x41
        registers[address] = value

    def instruction(uc, address, size, unused):
        if address in (0x60000, wait_address):
            stopped.append(address)
            uc.emu_stop()

    cpu.hook_add(UC_HOOK_MEM_READ, read)
    cpu.hook_add(UC_HOOK_MEM_WRITE, write)
    cpu.hook_add(UC_HOOK_CODE, instruction)
    cpu.emu_start(0x52000, 0x100000, count=20000)
    result = struct.unpack('<16I', cpu.mem_read(0x52800, 64))
    assert len(stopped) == 1 and result[5] == status, (name, result)
    if command == 2 and valid and status == 0:
        assert stopped == [wait_address] and result[6] == 4
        assert cpu.reg_read(UC_ARM_REG_CPSR) & 0x1c0 == 0x1c0
        assert registers[MODE] & 0xf1 == 0x41 and registers[FLAG] == 0xfa50392f
        assert sum(address == FEED for address, _ in writes) == 1
    else:
        assert stopped == [0x60000]
        assert all(cpu.reg_read(reg) == value for reg, value in saved.items()), name
        if status in (0, 2, 3):
            assert registers == before, (name, registers, before)
    if not valid or command not in (1, 2) or status == 2:
        assert not writes
    if fault == (MODE, before[MODE]):
        assert registers[MODE] & 1 and registers[FLAG] == 0xfa50392f
        assert result[8:10] == (1, 1)
    assert cpu.mem_read(0x52ff0, 16) == b'A' * 16 and cpu.mem_read(0x54000, 16) == b'B' * 16
    return {'case': name, 'status': result[5], 'stage': result[6], 'writes': len(writes)}

cases = [check('enable and disarm'), check('expiry reaches interrupt-masked wait', command=2),
         check('invalid authorization', valid=False), check('invalid command', command=3),
         check('high recovery input', initial={INPUT: 0x101}, status=2),
         check('active watchdog', initial={MODE: 0xa3}, status=2),
         check('existing redirect', initial={FLAG: 0xfa50392f}, status=2),
         check('alternate dispatch', initial={ALT: 0xefe8}, status=2),
         check('redirect write ignored', fault=(FLAG, 0xfa50392f), status=3),
         check('RTC request ignored', fault=(RTC, 0x5aa5a55a), status=3),
         check('reset configuration ignored', fault=(CONFIG, 0x21), status=3),
         check('reset output ignored', fault=(OUTPUT, 2), status=3),
         check('enable ignored', fault=(MODE, 0x43), status=3),
         check('disable ignored retains recovery', fault=(MODE, 0xa2), status=4),
         check('redirect restoration ignored', fault=(FLAG, 0), status=4)]
print(json.dumps({'image_sha256': hashlib.sha256(image).hexdigest(), 'cases': cases,
                  'limitation': 'Instruction and MMIO model only; no hardware reset, elapsed time, or register retention.'}, indent=2))
