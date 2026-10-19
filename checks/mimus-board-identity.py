#!/usr/bin/env python3
"""Check the linked Mimus workload gate without executing shared CPU startup.

Exception level, cache state, session validation and cache publication are
modeled. Only the private identity hook and its workload branch execute.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn import arm64_const as reg

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--image', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
a = parser.parse_args()
with a.image.open('rb') as source:
    elf = ELFFile(source)
    segments = [(s['p_vaddr'], s.data()) for s in elf.iter_segments()
                if s['p_type'] == 'PT_LOAD']
    symbols = {}
    for s in elf.get_section_by_name('.symtab').iter_symbols():
        if s['st_value']:
            symbols.setdefault(s.name, []).append((s['st_value'], s['st_size']))
    offsets = set()
    for cu in elf.get_dwarf_info().iter_CUs():
        for die in cu.iter_DIEs():
            if die.tag != 'DW_TAG_structure_type' or die.attributes.get('DW_AT_name', None) is None:
                continue
            if die.attributes['DW_AT_name'].value != b'a733_board_session':
                continue
            for member in die.iter_children():
                if member.attributes.get('DW_AT_name', None) is not None and member.attributes['DW_AT_name'].value == b'workload':
                    offsets.add(int(member.attributes['DW_AT_data_member_location'].value))
assert len(offsets) == 1, offsets
workload_offset = offsets.pop()
def symbol(name):
    assert len(symbols[name]) == 1, name
    return symbols[name][0]
entry, length = symbol('board_identity_prepare')
expected = [(0x44000000, 7501968, 100000), (0x44800000, 6560592, 100000),
            (0x45000000, 114924, 100000)]
results = []
for workload in (0, 4, 1, 2, 6, 8):
    cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    cpu.mem_map(0x41000000, 0x7000000)
    cpu.mem_map(0x50000000, 0x100000)
    for address, data in segments:
        cpu.mem_write(address, data)
    cpu.mem_write(0x50001000 + workload_offset, struct.pack('<I', workload))
    reads = {}
    for insn in Cs(CS_ARCH_ARM64, CS_MODE_ARM).disasm(bytes(cpu.mem_read(entry, length)), entry):
        if insn.mnemonic == 'mrs':
            target, name = insn.op_str.lower().split(', ')
            assert name in ('currentel', 'sctlr_el3'), name
            reads[insn.address] = (getattr(reg, 'UC_ARM64_REG_' + target.upper()),
                                   12 if name == 'currentel' else 0)
    assert len(reads) == 2
    calls = []
    returned = []
    def ret(value):
        cpu.reg_write(reg.UC_ARM64_REG_X0, value)
        cpu.reg_write(reg.UC_ARM64_REG_PC, cpu.reg_read(reg.UC_ARM64_REG_LR))
    def hook(machine, address, size, unused):
        if address == 0x500ff000:
            returned.append(cpu.reg_read(reg.UC_ARM64_REG_X0)); cpu.emu_stop()
        elif address in reads:
            target, value = reads[address]
            cpu.reg_write(target, value); cpu.reg_write(reg.UC_ARM64_REG_PC, address + 4)
        elif address == symbol('board_session_snapshot')[0]: ret(0x50001000)
        elif address == symbol('a733_smp_counter')[0]: ret(1000)
        elif address == symbol('a733_board_session_valid')[0]: ret(1)
        elif address == symbol('a733_cpu_coherence_publish')[0]:
            calls.append(tuple(cpu.reg_read(reg.UC_ARM64_REG_X0 + n) for n in range(3)))
            ret(1)
    cpu.hook_add(UC_HOOK_CODE, hook)
    cpu.reg_write(reg.UC_ARM64_REG_X0, 100000)
    cpu.reg_write(reg.UC_ARM64_REG_SP, 0x500f0000)
    cpu.reg_write(reg.UC_ARM64_REG_LR, 0x500ff000)
    cpu.emu_start(entry, 0, count=10000)
    accepted = workload in (0, 4)
    assert returned == [int(accepted)], (workload, returned)
    assert calls == (expected if accepted else []), (workload, calls)
    results.append({'workload': workload, 'accepted': accepted, 'publication_calls': calls})
a.output.parent.mkdir(parents=True, exist_ok=True)
a.output.write_text(json.dumps({'passed': True, 'image_sha256': hashlib.sha256(a.image.read_bytes()).hexdigest(),
    'cases': results, 'scope': __doc__, 'physical_access': False}, indent=2) + '\n')
print('PASS private identity gate: storage and keyboard/storage admitted; other workloads refused.')
