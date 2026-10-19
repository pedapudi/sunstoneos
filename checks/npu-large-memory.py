#!/usr/bin/env python3
"""Check the large NPU image's actual EL3 table construction without hardware.

System-register reads describe an admitted boot CPU with translation disabled.
Unicorn executes the linked table builder; it does not model physical caches,
translation walks, device addressing, scheduling or accelerator operation.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn import arm64_const as regs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    raw = args.image.read_bytes()
    with args.image.open('rb') as stream:
        elf = ELFFile(stream)
        assert elf['e_machine'] == 'EM_AARCH64'
        symbols = {s.name: (int(s['st_value']), int(s['st_size']))
                   for s in elf.get_section_by_name('.symtab').iter_symbols() if s.name}
        segments = [(int(s['p_vaddr']), s.data()) for s in elf.iter_segments()
                    if s['p_type'] == 'PT_LOAD']
    address = lambda name: symbols[name][0]
    expected = {'__npu_graph_input_start': 0x41400000,
                '__npu_graph_input_end': 0x41480000,
                '__npu_graph_output_start': 0x41480000,
                '__npu_graph_output_end': 0x414c0000,
                '__device_pool_start': 0x41600000,
                '__device_pool_end': 0x41a00000}
    assert all(address(name) == value for name, value in expected.items())
    assert address('__bss_end') <= address('__exception_stack_top') <= 0x41400000
    assert address('__heap_end') == 0x414c0000
    assert 0x421f0000 <= address('__tables_start') < address('__tables_end') <= 0x42200000
    table_names = ['a733_cached_el3_l1', 'a733_cached_el3_l2', 'a733_cached_el3_l3',
                   'a733_cached_el1_l1', 'a733_cached_el1_l2']
    tables = [address(name) for name in table_names]
    assert len(set(tables)) == 5 and all(not value % 4096 for value in tables)
    assert all(address('__tables_start') <= value < address('__tables_end') for value in tables)
    cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    cpu.mem_map(0x41000000, 0x1200000)
    for start, data in segments:
        if data:
            cpu.mem_write(start, data)
    for table in tables:
        cpu.mem_write(table, b'\xa5' * 4096)
    entry, size = symbols['a733_cached_memory_prepare']
    system = {'currentel': 12, 'sctlr_el3': 0, 'sctlr_el1': 0,
              'daif': 0x3c0, 'mpidr_el1': 0x80000000}
    reads = {}
    for instruction in Cs(CS_ARCH_ARM64, CS_MODE_ARM).disasm(bytes(cpu.mem_read(entry, size)), entry):
        if instruction.mnemonic == 'mrs':
            register, field = instruction.op_str.lower().split(', ')
            assert field in system and register.startswith('x')
            reads[instruction.address] = (getattr(regs, 'UC_ARM64_REG_' + register.upper()), field)
    assert {field for _, field in reads.values()} == set(system)
    observed = []
    def instruction_hook(machine, pc, size, unused):
        if pc in reads:
            register, field = reads[pc]
            observed.append(field)
            machine.reg_write(register, system[field])
            machine.reg_write(regs.UC_ARM64_REG_PC, pc + 4)
    stack = address('__monitor_stack_top')
    def write_hook(machine, access, destination, size, value, unused):
        assert (stack - 16384 <= destination and destination + size <= stack) or any(
            table <= destination and destination + size <= table + 4096 for table in tables), hex(destination)
    cpu.hook_add(UC_HOOK_CODE, instruction_hook)
    cpu.hook_add(UC_HOOK_MEM_WRITE, write_hook)
    cpu.reg_write(regs.UC_ARM64_REG_SP, stack)
    cpu.reg_write(regs.UC_ARM64_REG_LR, 0x41000000)
    cpu.emu_start(entry, 0x41000000, count=200000)
    assert cpu.reg_read(regs.UC_ARM64_REG_PC) == 0x41000000
    assert cpu.reg_read(regs.UC_ARM64_REG_X0) == 1
    assert len(observed) == 5
    word = lambda at: struct.unpack('<Q', cpu.mem_read(at, 8))[0]
    pages = list(range(0x41400000, 0x414c0000, 4096)) + list(range(0x41600000, 0x41a00000, 4096))
    for level in (1, 3):
        root = address(f'a733_cached_el{level}_l1')
        for page in pages:
            first = word(root + ((page >> 30) & 511) * 8)
            assert first & 3 == 3
            second = word((first & 0x0000fffffffff000) + ((page >> 21) & 511) * 8)
            assert second & 3 == 1 and (second >> 2) & 7 == 1
            assert (second >> 8) & 3 == 3 and second & (1 << 10)
            assert ((second & 0x0000ffffffe00000) | (page & 0x1fffff)) == page
            assert (second >> 6) & 3 == 1 and second & (1 << 54)
            assert bool(second & (1 << 5)) == (level == 3)
    report = {'passed': True, 'image_sha256': hashlib.sha256(raw).hexdigest(),
              'linked_geometry': expected, 'table_pages': tables,
              'checked_pages_per_regime': len(pages), 'modeled_system_registers': system,
              'physical_access': False, 'scope': __doc__.strip()}
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS linked large-NPU geometry and EL3 table construction')


if __name__ == '__main__':
    main()
