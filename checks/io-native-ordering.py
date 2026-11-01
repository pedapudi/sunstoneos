#!/usr/bin/env python3
"""Execute legacy TWI/watchdog/redirect accesses with delayed transactions.

The actual compiled DSB instructions drain a constructed architectural store
queue. Peripheral TWI flag/STOP completion is modeled separately. This checks
barrier placement without claiming physical mapping or endpoint acceptance.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

from harness import CLANG, LLD, environment
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_SP, UC_ARM64_REG_LR, UC_ARM64_REG_PC
from register_latency import PostedWrites, RegisterLatency

ROOT = Path(__file__).resolve().parents[1]
TWI = 0x07083000
CTRL, STATUS, DATA = TWI + 12, TWI + 16, TWI + 8
WDT, FEL = 0x02050000, 0x07090108
FLAG, ENTRY, ALT = 0x070501c0, 0x070501c4, 0x070501d4
MAGIC = 0xfa50392f
BASE, RAM, RETURN = 0x100000, 0x200000, 0x300000
UNITS = ['drivers/i2c/a733_i2c.c', 'drivers/watchdog/a733_watchdog.c',
         'drivers/watchdog/a733_rom_return.c']


class Machine:
    def __init__(self, image, symbols, arbitration=False, stuck_stop=False):
        self.cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        for a, size in [(BASE, 0x10000), (RAM, 0x10000), (RETURN, 0x1000),
                        (WDT, 0x1000), (0x07010000, 0x1000), (0x07025000, 0x1000),
                        (0x07050000, 0x1000), (TWI, 0x1000), (0x07090000, 0x1000)]:
            self.cpu.mem_map(a, size)
        self.cpu.mem_write(BASE, image)
        self.symbols = symbols
        self.posted = PostedWrites({0x0701019c: 0x10001, 0x07025000: 0x22,
            CTRL: 0x40, STATUS: 0xf8, TWI + 0x1c: 0, TWI + 0x20: 0x30,
            WDT + 0x14: 0xa2, WDT + 0x10: 0x22, WDT + 0x18: 7,
            FEL: 0x12345678, FLAG: 0, ENTRY: 0x12345678, ALT: 0})
        self.arbitration, self.stuck_stop = arbitration, stuck_stop
        self.twi_event = None
        self.phase = self.counter = self.barriers = self.restarts = 0
        self.writes = []
        self.returned = False
        self.cpu.hook_add(UC_HOOK_MEM_READ, self.read)
        self.cpu.hook_add(UC_HOOK_MEM_WRITE, self.write)
        self.cpu.hook_add(UC_HOOK_CODE, self.instruction)

    def mmio(self, address):
        return address >= 0x02000000

    def read(self, uc, access, address, size, value, unused):
        if not self.mmio(address):
            return
        assert size == 4 and address in self.posted.words, ('read', hex(address), size)
        assert not self.posted.pending, 'dependent MMIO read before DSB completion'
        if address == CTRL and self.twi_event and self.twi_event.pending:
            self.posted.words[CTRL] = self.twi_event.apply(self.counter, self.posted.words[CTRL])
            if not self.twi_event.pending:
                if self.twi_event.mask == 8:
                    self.posted.words[STATUS] = 0x38 if self.arbitration else [8, 0x18, 0x28, 0x10, 0x40, 0x58][self.phase - 1]
                    if self.phase == 6:
                        self.posted.words[DATA] = 0x5a
                else:
                    self.posted.words[STATUS] = 0xf8
        uc.mem_write(address, struct.pack('<I', self.posted.read(address)))

    def write(self, uc, access, address, size, value, unused):
        if not self.mmio(address):
            return
        assert size == 4, ('write', hex(address), size)
        assert not self.posted.pending, 'dependent MMIO write before DSB completion'
        self.posted.write(address, value)
        self.writes.append((address, value))

    def complete(self):
        for address, value in self.posted.complete():
            if WDT + 0xc <= address <= WDT + 0x18:
                if address == WDT + 0xc:
                    assert value == 0x14af
                    assert self.posted.words[WDT + 0x14] & 1
                    assert self.posted.words[WDT + 0x10] & 3 == 1
                    assert self.posted.words[WDT + 0x18] == 2
                    self.restarts += 1
                else:
                    assert value >> 16 == 0x16aa
                    self.posted.words[address] = value & 0xffff
            elif address == FLAG:
                assert value != MAGIC or self.posted.words[ENTRY] == 0x20
            elif address == ENTRY:
                assert self.posted.words[FLAG] == 0, 'entry changed while redirect active'
            elif address == CTRL:
                # DSB completes the transaction, not the TWI command. Its flag
                # and STOP remain pending for 30us of independent fixture time.
                self.posted.words[CTRL] = value & ~8
                if value & 0x10:
                    self.twi_event = RegisterLatency(self.counter, 24 * 30, 0x10, 0, stuck=self.stuck_stop)
                elif self.arbitration and self.phase:
                    assert not value & 0x30
                elif value != 0x40:
                    self.phase += 1
                    assert self.phase <= 6
                    self.twi_event = RegisterLatency(self.counter, 24 * 30, 8, 8)

    def instruction(self, uc, address, size, unused):
        # Constructed execution budget: one instruction consumes 24 counter
        # ticks (1us). Counter reads and MMIO reads do not advance time.
        self.counter += 24
        if address == RETURN:
            assert not self.posted.pending, 'return before DSB completion'
            self.returned = True
            uc.emu_stop()
            return
        word = struct.unpack('<I', uc.mem_read(address, 4))[0]
        if word == 0xd5033f9f:  # DSB SY, actual native instruction
            self.barriers += 1
            self.complete()
        elif word & 0xffffffe0 == 0xd53be020:  # MRS Xt,CNTPCT_EL0
            uc.reg_write(UC_ARM64_REG_X0 + (word & 31), self.counter)
            uc.reg_write(UC_ARM64_REG_PC, address + 4)

    def call(self, name, *args):
        self.returned = False
        self.cpu.reg_write(UC_ARM64_REG_SP, RAM + 0xf000)
        self.cpu.reg_write(UC_ARM64_REG_LR, RETURN)
        for i, value in enumerate(args):
            self.cpu.reg_write(UC_ARM64_REG_X0 + i, value)
        self.cpu.emu_start(self.symbols[name], RETURN + 4, count=100000)
        assert self.returned, ('function did not return', name)
        return self.cpu.reg_read(UC_ARM64_REG_X0)


def exercise(image, symbols, case):
    m = Machine(image, symbols, case == 'arbitration', case == 'stuck-stop')
    before = dict(m.posted.words)
    if case in ('twi', 'arbitration', 'stuck-stop'):
        assert m.call('a733_i2c_open', RAM) == 0
        m.cpu.mem_write(RAM + 0x100, b'\xa5')
        result = m.call('a733_i2c_read8', RAM, 0x36, 0x10, RAM + 0x100)
        assert result == {'twi': 0, 'arbitration': 4, 'stuck-stop': 5}[case], (case, result, m.phase, m.counter, m.posted.words)
        if case == 'twi':
            assert m.cpu.mem_read(RAM + 0x100, 1) == b'\x5a'
            assert m.phase == 6 and not m.twi_event.pending
            m.call('a733_i2c_close', RAM)
            assert m.posted.words[CTRL] == 0x40
        else:
            assert m.cpu.mem_read(RAM + 0x100, 1) == b'\xa5'
            assert not struct.unpack('<I', m.cpu.mem_read(RAM + 16, 4))[0]
            if case == 'arbitration':
                assert not any(value & 0x10 for address, value in m.writes if address == CTRL)
    elif case == 'watchdog':
        assert m.call('a733_watchdog_arm', RAM, 6) == 1
        assert m.restarts == 1
        m.call('a733_watchdog_feed', RAM)
        assert m.restarts == 2
        assert m.call('a733_watchdog_disarm', RAM) == 1
        assert all(m.posted.words[a] == before[a] for a in (WDT + 0x10, WDT + 0x14, WDT + 0x18, FEL))
    elif case == 'redirect':
        assert m.call('a733_rom_return_prepare', RAM) == 1
        assert m.posted.words[FLAG] == MAGIC and m.posted.words[ENTRY] == 0x20
        assert m.call('a733_rom_return_restore', RAM) == 1
        assert all(m.posted.words[a] == before[a] for a in (FLAG, ENTRY, ALT))
    return dict(case=case, barriers=m.barriers, writes=len(m.writes), counter=m.counter)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    cc, ld = CLANG, LLD
    assert cc.is_file() and ld.is_file(), (cc, ld)
    commands = []
    def run(command, log):
        command = [str(x) for x in command]
        commands.append(command)
        result = subprocess.run(command, cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=environment())
        (out / log).write_text(result.stdout)
        assert result.returncode == 0, result.stdout
        return result.stdout
    objects = []
    for index, source in enumerate(UNITS):
        obj = out / f'{index}.o'
        run([cc, '--target=aarch64-none-elf', '-O2', '-ffreestanding', '-fno-builtin', '-Wall', '-Wextra', '-Werror',
             '-Iinclude', '-Iplatform/a733', '-MD', '-MF', out / f'{index}.d', '-c', source, '-o', obj], f'compile-{index}.log')
        objects.append(obj)
    elf, binary = out / 'io.elf', out / 'io.bin'
    linker = out / 'io.ld'
    linker.write_text('SECTIONS { . = 0x100000; .text : { *(.text*) } .rodata : { *(.rodata*) } .data : { *(.data*) } .bss : { *(.bss*) } }\n')
    run([ld, '-T', linker, '--entry=a733_watchdog_arm', *objects, '-o', elf], 'link.log')
    with elf.open('rb') as stream:
        image = ELFFile(stream)
        parts = [(p['p_vaddr'], p.data()) for p in image.iter_segments() if p['p_type'] == 'PT_LOAD' and p['p_vaddr'] >= BASE]
        symbols = {p.name: p['st_value'] for p in image.get_section_by_name('.symtab').iter_symbols() if p['st_shndx'] != 'SHN_UNDEF'}
    contents = bytearray(max(address + len(data) for address, data in parts) - BASE)
    for address, data in parts:
        contents[address - BASE:address - BASE + len(data)] = data
    binary.write_bytes(contents)
    (out / 'symbols.json').write_text(json.dumps(symbols, indent=2) + '\n')
    cases = [exercise(binary.read_bytes(), symbols, case) for case in ('twi', 'arbitration', 'stuck-stop', 'watchdog', 'redirect')]
    mutations = []
    dsb, nop = struct.pack('<I', 0xd5033f9f), struct.pack('<I', 0xd503201f)
    mutant = binary.read_bytes().replace(dsb, nop)
    assert mutant != binary.read_bytes()
    (out / 'io-no-dsb.bin').write_bytes(mutant)
    for case in ('twi', 'watchdog', 'redirect'):
        try:
            exercise(mutant, symbols, case)
        except AssertionError as error:
            assert 'before DSB completion' in str(error), str(error)
            mutations.append(dict(case=case, failure=str(error)))
        else:
            raise AssertionError(('missing-DSB mutation survived', case))
    (out / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
    paths = {Path(__file__).resolve(), ROOT / 'checks/register_latency.py', ROOT / 'checks/harness.py',
             cc.resolve(), ld.resolve(), binary, elf, out / 'io-no-dsb.bin', linker}
    for depfile in out.glob('*.d'):
        paths.update((ROOT / p).resolve() for p in depfile.read_text().replace('\\\n', ' ').split(':', 1)[1].split())
    (out / 'inputs.json').write_text(json.dumps([dict(path=str(p), sha256=hashlib.sha256(p.read_bytes()).hexdigest()) for p in sorted(paths)], indent=2) + '\n')
    (out / 'results.json').write_text(json.dumps(dict(cases=cases, mutations=mutations, physical_access=False,
        claim='architectural store completion under modeled DSB; endpoint effects remain separate'), indent=2) + '\n')
    print('PASS compiled I/O DSB ordering, delayed TWI status, retention and three missing-DSB mutations')


if __name__ == '__main__':
    main()
