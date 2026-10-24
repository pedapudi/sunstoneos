#!/usr/bin/env python3
"""Run the linked vInterruptCore of a cached eight-core image under Unicorn.

The SMP port sends an SGI in line from EL1 code and keeps SVC 117 for EL0
tasks (docs/designs/cpu-a76-dvfs.md). This check executes the function from a
linked image with modeled TPIDRRO_EL0 values and interrupt-nesting counts and
records, for each case, whether it took the supervisor call or wrote
ICC_SGI1R_EL1, the value written and the barriers around the write:

  - an EL0 task while another core is inside an interrupt handler: SVC;
  - an EL0 task on a core whose own nesting count is nonzero, as in a
    handler that interrupted it: DSB ISHST, the write, no ISB;
  - an EL1 task: DSB ISHST, the write, ISB;
  - a core that has not resumed a task (zero TCB field): SVC.

Every case must target the requested core with the image's own SGI value.
The model has no GIC; it proves the instruction path only.

  checks/smp-sgi-send.py --image a733-smp-cached-diagnostic.elf --output DIR
"""
import argparse
import json
from pathlib import Path
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_SP, UC_ARM64_REG_LR, UC_ARM64_REG_PC

STACK, RETURN = 0x7f000000, 0x7e000000
TCB = 0x41234560


def load(path):
    elf = ELFFile(open(path, 'rb'))
    symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols() if s.name}
    u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    for segment in elf.iter_segments():
        if segment['p_type'] != 'PT_LOAD' or not segment['p_memsz']:
            continue
        base = segment['p_paddr'] & ~0xfff
        size = (segment['p_paddr'] + segment['p_memsz'] - base + 0xfff) & ~0xfff
        try:
            u.mem_map(base, size)
        except Exception:  # Adjacent segments share a page that is already mapped.
            pass
        u.mem_write(segment['p_paddr'], segment.data())
    u.mem_map(STACK - 0x10000, 0x10000)
    u.mem_map(RETURN, 0x1000)
    return u, symbols


def run(u, symbols, register, nestings, target):
    for core in range(8):
        u.mem_write(symbols['ullPortInterruptNestings'] + 8 * core, nestings[core].to_bytes(8, 'little'))
    u.reg_write(UC_ARM64_REG_X0, 0)
    u.reg_write(UC_ARM64_REG_X1, target)
    u.reg_write(UC_ARM64_REG_SP, STACK - 0x100)
    u.reg_write(UC_ARM64_REG_LR, RETURN)
    disassembler = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    trace = []

    def code(uc, address, size, unused):
        instruction = next(disassembler.disasm(bytes(uc.mem_read(address, size)), address))
        text = (instruction.mnemonic + ' ' + instruction.op_str).strip()
        if address == RETURN:
            uc.emu_stop()
            return
        if text.startswith('mrs') and 'tpidrro_el0' in text:
            uc.reg_write(UC_ARM64_REG_X0 + int(instruction.op_str.split(',')[0][1:]), register)
        elif text.startswith('msr') and 'icc_sgi1r_el1' in text:
            source = instruction.op_str.split(',')[1].strip()
            trace.append(('sgi', uc.reg_read(UC_ARM64_REG_X0 + int(source[1:]))))
        elif instruction.mnemonic in ('dsb', 'isb'):
            trace.append((text,))
        elif instruction.mnemonic == 'svc':
            trace.append(('svc', uc.reg_read(UC_ARM64_REG_X0), int(instruction.op_str[1:], 0)))
            uc.emu_stop()
            return
        else:
            return
        uc.reg_write(UC_ARM64_REG_PC, address + 4)
    hook = u.hook_add(UC_HOOK_CODE, code)
    u.emu_start(symbols['vInterruptCore'], RETURN, count=4000)
    u.hook_del(hook)
    return trace


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--image', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    u, symbols = load(a.image)
    # MPIDR Aff1 is the A733 core number: SGI1R target list bit 0 in Aff1 = core.
    expected = {target: (target << 16) | 1 for target in range(8)}
    el0 = lambda core: TCB | core << 48
    el1 = lambda core: TCB | core << 48 | 1 << 56
    cases = {
        'el0-task-other-core-in-handler': (el0(3), [1, 0, 0, 0, 0, 0, 0, 0], 6),
        'handler-interrupting-el0-task': (el0(3), [0, 0, 0, 1, 0, 0, 0, 0], 6),
        'el1-task': (el1(6), [0] * 8, 0),
        'core-without-resumed-task': (0, [1] * 8, 7),
    }
    results = {}
    for name, (register, nestings, target) in cases.items():
        trace = run(u, symbols, register, nestings, target)
        results[name] = [list(t) for t in trace]
        value = expected[target]
        if name in ('el0-task-other-core-in-handler', 'core-without-resumed-task'):
            assert trace == [('svc', value, 117)], (name, trace)
        elif name == 'handler-interrupting-el0-task':
            assert trace == [('dsb ishst',), ('sgi', value)], (name, trace)
        else:
            assert trace == [('dsb ishst',), ('sgi', value), ('isb',)], (name, trace)
    (a.output / 'result.json').write_text(json.dumps({'passed': True, 'image': str(a.image), 'cases': results,
        'scope': 'Instruction path of the linked function under Unicorn; no GIC or SGI delivery.'}, indent=2) + '\n')
    print('PASS in-line SGI send: %d cases' % len(cases))


if __name__ == '__main__':
    main()
