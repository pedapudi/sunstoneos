#!/usr/bin/env python3
"""Execute the exact SRAM launcher with modeled MMIO and physical-counter reads."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm_const import UC_CPU_ARM_CORTEX_A15, UC_ARM_REG_R0, UC_ARM_REG_PC, UC_ARM_REG_CPSR, UC_ARM_REG_SP, UC_ARM_REG_LR

FLAG, ENTRY, ALT, INPUT = 0x070501c0, 0x070501c4, 0x070501d4, 0x03000024
CONFIG, MODE, OUTPUT, FEED, RTC = 0x02050010, 0x02050014, 0x02050018, 0x0205000c, 0x07090108
RVBAR, CONTRACT, REPORT = 0x08001004, 0x42004000, 0x52d00


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--focus-refusal', action='store_true')
    args = parser.parse_args(); bundle = args.bundle
    image = (bundle / 'launch.bin').read_bytes()
    prepared = json.loads((bundle / 'prepared.json').read_text())
    with (bundle / 'launch.elf').open('rb') as stream:
        elf = ELFFile(stream)
        park = next(s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols() if s.name == 'a733_smp_launch_park')
    assert 0 < len(image) <= 0xc00

    def check(name, reason=0, *, initial=None, ignored=None, sysreg=None, mutation=None, late=None, start=100000000, thumb_return=False):
        cpu = Uc(UC_ARCH_ARM, UC_MODE_ARM); cpu.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_A15)
        for base, size in [(0x50000, 0x30000), (0x41000000, 0x200000), (0x42004000, 0x1000),
                           (0x02050000, 0x1000), (0x03000000, 0x1000), (0x07050000, 0x1000),
                           (0x07090000, 0x1000), (0x08001000, 0x1000)]: cpu.mem_map(base, size)
        cpu.mem_write(0x52000, image); cpu.mem_write(0x52c00, (bundle / 'request.bin').read_bytes())
        cpu.mem_write(CONTRACT, (bundle / 'contract.bin').read_bytes())
        for segment in prepared['segments']: cpu.mem_write(segment['address'], (bundle / segment['name']).read_bytes())
        cpu.mem_write(0x52ff0, b'A' * 16)
        if mutation:
            address, value = mutation; cpu.mem_write(address, struct.pack('<Q', value))
        registers = {FLAG:0, ENTRY:0x12340000, ALT:0, INPUT:0, CONFIG:0x22, MODE:0xa2, OUTPUT:7, RTC:0, RVBAR:0, RVBAR+4:1}
        registers.update(initial or {})
        system = {0xee110f10:0, 0xee110f11:0, 0xee100fb0:0x81000000, 0xee1e0f10:24000000, 0xee1c0f50:0}
        system.update(sysreg or {})
        writes, stopped, ticks = [], [], []
        steps = 0
        saved = {UC_ARM_REG_R0+i:0x12340000+i*0x101 for i in range(13)}
        saved.update({UC_ARM_REG_SP:0x70000, UC_ARM_REG_LR:0x60000+int(thumb_return)})
        cpu.reg_write(UC_ARM_REG_CPSR, 0xa80f0053)
        saved[UC_ARM_REG_CPSR] = cpu.reg_read(UC_ARM_REG_CPSR)
        for register,value in saved.items(): cpu.reg_write(register,value)

        def read(uc, access, address, size, value, unused):
            if address < 0x02000000 or address >= 0x41000000: return
            assert address in registers and size == 4, (name, hex(address), size)
            uc.mem_write(address, struct.pack('<I', registers[address]))

        def write(uc, access, address, size, value, unused):
            if address < 0x02000000:
                assert REPORT <= address < REPORT+128 or 0x53000 <= address < 0x54000 or 0x70000-60 <= address < 0x70000, (name, hex(address))
                return
            if address >= 0x41000000:
                assert CONTRACT <= address < CONTRACT+8 or CONTRACT+80 <= address < CONTRACT+112, (name, hex(address))
                if address == CONTRACT:
                    assert registers[MODE] == 0x41 and ticks and len(ticks) >= 3
                    values = struct.unpack('<4Q', uc.mem_read(CONTRACT+80, 32))
                    assert values == (ticks[0], ticks[0]+24000000, ticks[0]+48000000, ticks[0]+96000000)
                return
            assert address in (FLAG, ENTRY, CONFIG, MODE, OUTPUT, FEED, RTC, RVBAR, RVBAR+4) and size == 4
            writes.append((address, value))
            if address == FEED:
                assert value == 0x14af; return
            if address in (CONFIG, MODE, OUTPUT):
                assert value >> 16 == 0x16aa; value &= 0xffff
            if address == MODE:
                assert value == 0x41 and registers[FLAG] == 0xfa50392f and registers[ENTRY] == 0x20
                assert registers[RTC] == 0x5aa5a55a and registers[CONFIG] == 1 and registers[OUTPUT] == 2
            if address in (RVBAR, RVBAR+4):
                assert struct.unpack('<Q', uc.mem_read(CONTRACT, 8))[0] == 0x41533733534d5031
            if address != ignored: registers[address] = value

        def instruction(uc, address, size, unused):
            nonlocal steps
            steps += 1
            if address == 0x60000:
                stopped.append('return'); uc.emu_stop(); return
            if address == park:
                stopped.append('park'); uc.emu_stop(); return
            word = struct.unpack('<I', uc.mem_read(address, 4))[0]
            if word & 0xffff0fff in system:
                uc.reg_write(UC_ARM_REG_R0 + ((word >> 12) & 15), system[word & 0xffff0fff])
                uc.reg_write(UC_ARM_REG_PC, address+4)
            elif word & 0xfff00fff == 0xec500f0e:
                value = start + steps if not ticks else ticks[0] + 100*len(ticks)
                if late and len(ticks) == late[0]: value = ticks[0] + late[1]
                ticks.append(value)
                uc.reg_write(UC_ARM_REG_R0 + ((word >> 12)&15), value & 0xffffffff)
                uc.reg_write(UC_ARM_REG_R0 + ((word >> 16)&15), value >> 32)
                uc.reg_write(UC_ARM_REG_PC,address+4)
            elif word == 0xee0c0f50:
                assert uc.reg_read(UC_ARM_REG_R0) == 3 and registers[RVBAR] == 0x41000000 and registers[RVBAR+4] == 0
                assert len(ticks) == 5 and ticks[-1]-ticks[0] < 24000
                assert uc.reg_read(UC_ARM_REG_CPSR) & 0x1c0 == 0x1c0
                stopped.append('rmr'); uc.emu_stop()

        cpu.hook_add(UC_HOOK_MEM_READ, read); cpu.hook_add(UC_HOOK_MEM_WRITE, write); cpu.hook_add(UC_HOOK_CODE, instruction)
        cpu.emu_start(0x52000, 0x52c00, count=20000)
        report = struct.unpack('<3Q', cpu.mem_read(REPORT, 24))
        clean_return = reason in (1,2,3,4,5,6,13)
        expected = 'return' if clean_return else 'rmr' if reason == 12 else 'park'
        assert stopped == [expected] and report[0] == reason, (name, stopped, report, ticks)
        assert sum(address == FEED for address, _ in writes) <= 1
        assert cpu.mem_read(0x52ff0, 16) == b'A'*16
        if reason in (0, 1, 2, 3, 4, 5, 6): assert not writes, (name, writes)
        if reason == 12: assert report[1:3] == (ticks[0], ticks[2])
        if clean_return:
            for register,value in saved.items():
                if register == UC_ARM_REG_CPSR: value = (value & ~0x20) | (int(thumb_return)<<5)
                assert cpu.reg_read(register) == value, (name,register,hex(cpu.reg_read(register)),hex(value))
            full = struct.unpack('<16Q',cpu.mem_read(REPORT,128))
            request = struct.unpack('<16Q',cpu.mem_read(0x52c00,128))
            assert full[3:7] == (request[3],request[4],request[6],0x4153373352454631) and not any(full[7:])
            assert not writes and report[1:3] == (0,0)
        return {'case':name, 'reason':reason, 'stop':expected, 'mmio_writes':len(writes), 'counter_reads':ticks}

    cases = []
    if not args.focus_refusal: cases.append(check('bounded arm publication and fixed transition',12))
    for name, key, value in [('cache enabled',0xee110f10,4), ('nonsecure state',0xee110f11,1),
                             ('wrong CPU',0xee100fb0,0x81000100)]:
        cases.append(check(name,sysreg={key:value}))
    cases.append(check('counter frequency refusal restores FEL state',13,sysreg={0xee1e0f10:19200000}))
    cases.append(check('Thumb FEL return preserves caller registers and flags',1,mutation=(0x52c00,0),thumb_return=True))
    for name, address, value, reason in [('request identity',0x52c00,0,1), ('generation inverse',0x52c28,0,2),
                                        ('contract already published',CONTRACT,1,3), ('contract reserved field',CONTRACT+216,1,3),
                                        ('image identity mismatch',CONTRACT+48,0,4)]:
        cases.append(check(name,reason,mutation=(address,value)))
    for name, address, value, reason in [('physical input high',INPUT,0x100,5), ('watchdog enabled',MODE,0x41,0),
                                        ('wrong watchdog source',CONFIG,0x100,6), ('active redirect',FLAG,1,6), ('alternate dispatch',ALT,1,6)]:
        cases.append(check(name,reason,initial={address:value}))
    for name, address, reason in [('redirect entry ignored',ENTRY,7), ('redirect ignored',FLAG,7),
                                  ('RTC flag ignored',RTC,7), ('output ignored',OUTPUT,7), ('configuration ignored',CONFIG,7),
                                  ('enable ignored',MODE,8), ('RVBAR ignored',RVBAR,11), ('RVBAR high ignored',RVBAR+4,11)]:
        if not args.focus_refusal or address == ENTRY: cases.append(check(name,reason,ignored=address))
    for name, index, elapsed in [('arm budget expired',1,24000), ('publication budget expired',3,24000),
                                 ('transition budget expired',4,24000), ('counter went backward',1,-1)]:
        if not args.focus_refusal: cases.append(check(name,9,late=(index,elapsed)))
    if not args.focus_refusal: cases.append(check('counter overflow refused',8,start=2**64-1000000))
    result = {'physical_access':False,'image_sha256':hashlib.sha256(image).hexdigest(),
              'check_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(), 'cases':cases,
              'limits':'MMIO and counter model only. No oscillator, reset, interconnect liveness, retention, AArch64 entry, or scheduler qualification.'}
    args.output.write_text(json.dumps(result,indent=2)+'\n'); print(f'{len(cases)} launcher instruction cases passed.')


if __name__ == '__main__': main()
