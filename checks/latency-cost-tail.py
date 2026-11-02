#!/usr/bin/env python3
"""Count native recorder instructions outside the timed C IRQ handler.

Unicorn executes the linked instruction paths; system register accesses are
modeled and instruction counts are not physical cycles or time estimates.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn import arm64_const as regs

SCRATCH, BYTES = 0x81000000, 0x10000
RETURN = SCRATCH + 0x100


class Image:
    def __init__(self, path):
        self.path = path
        self.sha256 = hashlib.sha256(path.read_bytes()).hexdigest()
        with path.open('rb') as f:
            elf = ELFFile(f)
            self.segments = [(int(s['p_vaddr']), int(s['p_memsz']), s.data())
                             for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']
            self.symbols = {s.name: int(s['st_value']) for s in elf.get_section_by_name('.symtab').iter_symbols()
                            if s.name and s['st_shndx'] != 'SHN_UNDEF'}
            text = elf.get_section_by_name('.text')
            # Literal pools can terminate a whole-section disassembly. Decode
            # each aligned A64 word independently so later port code remains.
            data, base = text.data(), int(text['sh_addr'])
            decoder = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
            self.instructions = {}
            for offset in range(0, len(data), 4):
                instruction = next(decoder.disasm(data[offset:offset + 4], base + offset), None)
                if instruction is not None:
                    self.instructions[instruction.address] = instruction


class Machine:
    def __init__(self, image, core=0):
        self.image, self.core = image, core
        self.cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        ranges = []
        for start, size, _ in image.segments:
            if size:
                ranges.append((start & ~4095, (start + size + 4095) & ~4095))
        merged = []
        for low, high in sorted(ranges):
            if merged and low <= merged[-1][1]:
                merged[-1][1] = max(merged[-1][1], high)
            else:
                merged.append([low, high])
        for low, high in merged:
            assert high <= SCRATCH or low >= SCRATCH + BYTES
            self.cpu.mem_map(low, high - low)
        for address, _, data in image.segments:
            self.cpu.mem_write(address, data)
        self.cpu.mem_map(SCRATCH, BYTES)
        self.cpu.reg_write(regs.UC_ARM64_REG_SP, SCRATCH + BYTES - 4096)
        self.cpu.reg_write(regs.UC_ARM64_REG_LR, RETURN)
        self.trace, self.stopped, self.stop_address = [], False, None
        self.cpu.hook_add(UC_HOOK_CODE, self.code)

    def code(self, cpu, address, size, unused):
        if address in (RETURN, self.stop_address):
            self.stopped = True
            cpu.emu_stop()
            return
        instruction = self.image.instructions[address]
        self.trace.append(dict(address=address, mnemonic=instruction.mnemonic, operands=instruction.op_str))
        if instruction.mnemonic == 'mrs':
            target, system = instruction.op_str.split(', ')
            assert system == 'tpidr_el1', system
            cpu.reg_write(getattr(regs, 'UC_ARM64_REG_' + target.upper()), self.core)
            cpu.reg_write(regs.UC_ARM64_REG_PC, address + 4)
        elif instruction.mnemonic == 'msr':
            # No MMIO/system state operation occurs in this host-only model.
            cpu.reg_write(regs.UC_ARM64_REG_PC, address + 4)
        elif instruction.mnemonic == 'eret':
            self.stopped = True
            cpu.emu_stop()

    def run(self, start, stop=None):
        self.stop_address = stop
        self.cpu.emu_start(start, 0, count=2000)
        assert self.stopped, 'Execution budget exhausted'
        return dict(instructions=len(self.trace), trace=self.trace)


def restore_block(image):
    irq = image.symbols['FreeRTOS_IRQ_Handler']
    exit_irq = image.symbols['Exit_IRQ_No_Context_Switch']
    close = image.symbols['ss_latency_mask_close']
    calls = [a for a, i in image.instructions.items() if irq < a < exit_irq and
             i.mnemonic == 'bl' and int(i.op_str.lstrip('#'), 16) == close]
    assert len(calls) == 1, calls
    call = calls[0]
    start, stop = call - 16, call + 12
    block = [image.instructions[a] for a in range(start, stop, 4)]
    assert [i.mnemonic for i in block] == ['cbnz', 'stp', 'stp', 'mrs', 'bl', 'ldp', 'ldp']
    assert image.instructions[stop].mnemonic == 'msr' and 'icc_pmr_el1' in image.instructions[stop].op_str
    assert int(block[0].op_str.split('#')[1], 16) == stop
    return start, stop


def no_switch_tail(image):
    irq, exit_irq = (image.symbols[n] for n in ('FreeRTOS_IRQ_Handler', 'Exit_IRQ_No_Context_Switch'))
    candidates = [a for a, i in image.instructions.items() if irq < a < exit_irq and
                  i.mnemonic == 'msr' and i.op_str == 'daifset, #2']
    assert len(candidates) == 1, candidates
    machine = Machine(image)
    sp = machine.cpu.reg_read(regs.UC_ARM64_REG_SP)
    # At this point the C caller's scratch registers were restored. The stack
    # holds IRQ ID/old nesting, then nesting/address, then SPSR/ELR and saved GP.
    machine.cpu.mem_write(sp, struct.pack('<4Q', 30, 0, 0, image.symbols['ullPortInterruptNestings']))
    result = machine.run(candidates[0])
    assert any(t['address'] == exit_irq for t in result['trace'])
    assert all(t['mnemonic'] != 'bl' for t in result['trace'])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--enabled', type=Path, required=True)
    parser.add_argument('--disabled', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    on, off = Image(args.enabled), Image(args.disabled)
    assert 'ss_latency_mask_close' not in off.symbols
    start, stop = restore_block(on)
    rows = []
    for core in range(8):
        helper = Machine(on, core)
        helper.cpu.reg_write(regs.UC_ARM64_REG_X0, core)
        inactive = helper.run(on.symbols['ss_latency_mask_close'])
        machine = Machine(on, core)
        machine.cpu.reg_write(regs.UC_ARM64_REG_X3, 0)
        unmasked = machine.run(start, stop)
        assert unmasked['instructions'] == 7 + inactive['instructions']
        machine = Machine(on, core)
        machine.cpu.reg_write(regs.UC_ARM64_REG_X3, 1)
        nested = machine.run(start, stop)
        assert inactive['instructions'] == 9 and unmasked['instructions'] == 16 and nested['instructions'] == 1
        rows.append(dict(core=core, mask_close_inactive=inactive, restore_inactive=unmasked, restore_nested=nested))
    tails = dict(enabled=no_switch_tail(on), disabled=no_switch_tail(off))
    assert tails['enabled']['instructions'] == tails['disabled']['instructions']
    assert [t['mnemonic'] for t in tails['enabled']['trace']] == [t['mnemonic'] for t in tails['disabled']['trace']]
    result = dict(passed=True, evidence='Linked native instructions executed under Unicorn; no cycle or board-time estimate',
        images=[dict(path=str(i.path), sha256=i.sha256) for i in (on, off)],
        restore_block=dict(start=start, stop=stop), cores=rows, no_switch_irq_tails=tails,
        no_switch_tail_added_instructions=0,
        limitations='A switched IRQ also executes the instrumented vTaskSwitchContext lock paths before this restore block. The inactive recorder case assumes mask_active=0. Active mask intervals take additional recording branches. No per-IRQ switch classification was captured by the board benchmark.')
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print('PASS: no-switch IRQ tail adds0 instructions; switched restore adds16 with inactive mask recorder; nested restore adds1; no time conversion')


if __name__ == '__main__':
    main()
