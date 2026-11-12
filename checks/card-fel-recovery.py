#!/usr/bin/env python3
"""Execute the linked A64 launcher and exact A32 first-stage FEL gate.

Counter, cache controls, MMIO completion and CPU-local reset are modeled.
Execution stops at the captured-ROM entry; no USB device or card is opened.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import struct
from capstone import Cs, CS_ARCH_ARM, CS_ARCH_ARM64, CS_MODE_ARM
from unicorn import Uc, UC_ARCH_ARM, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn import arm_const as arm, arm64_const as a64
from harness import Image, refused

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['boot/a733/card-fel-gate.S', 'apps/card-fel-recovery/entry.S',
                'apps/card-fel-recovery/profile.c', 'tools/card_recovery.py', 'tools/card-fel-image.py']
GATE_RUNS = [['--image', '{elf:card-fel-recovery}', '--output', '{output}']]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    result = importlib.util.module_from_spec(spec); spec.loader.exec_module(result)
    return result


card = module('linked_card_recovery', 'tools/card_recovery.py')
crumbs = module('card_recovery_crumb_layout', 'tools/a733-crumbs.py')
FAULT_WORDS = {name: crumbs.RTC+crumbs.WORDS[name] for name in ('esr','elr','far')}
FAULT_SENTINELS = dict(zip(FAULT_WORDS, (0x11111111,0x22222222,0x33333333)))


class Machine:
    def __init__(self, image, info, nonce, variant='', policy=0x100):
        self.image, self.profile, self.nonce = image, info['profile'], nonce
        self.variant, self.events, self.terminal = variant, [], None
        self.regs = {card.RTC_FLAG: card.FEL_FLAG, 0x02050010: 1, 0x02050014: 0x41, 0x02050018: 2}
        self.regs.update({FAULT_WORDS[name]:value for name,value in FAULT_SENTINELS.items()})
        for core in range(1,8):
            self.regs[0x07050200 + core*4] = 2
            self.regs[0x07052000 + core*0x1000] = policy
            self.regs[0x07052008 + core*0x1000] = policy
        self.system = {'currentel': 12, 'mpidr_el1': 0xc1000000, 'sctlr_el3': 0}
        self.counter, self.sctlr = 0x123456789ab, 0x1000
        self.cs64, self.cs32 = Cs(CS_ARCH_ARM64, CS_MODE_ARM), Cs(CS_ARCH_ARM, CS_MODE_ARM)

    def mmio(self, cpu):
        def read(cpu, offset, size, base):
            assert size == 4
            self.events.append(('read', base+offset, self.regs.get(base+offset,0)))
            return self.regs.get(base+offset,0)
        def write(cpu, offset, size, value, base):
            assert size == 4
            address = base+offset
            self.events.append(('write',address,value))
            if address == 0x02050014:
                assert value == 0x16aa0000
                if self.variant == 'stuck-disarm':return
                value &= 0xffff
            if address == card.RTC_FLAG and self.variant == 'stuck-clear':return
            self.regs[address] = value
        for base in [0x02050000,0x07050000,0x07090000,0x08001000] + list(range(0x07053000,0x0705a000,0x1000)):
            cpu.mmio_map(base,0x1000,read,base,write,base)

    def launcher(self, mutate=None):
        cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        cpu.mem_map(0x41000000,0x10000);cpu.mem_map(card.REPORT,4096)
        for address,raw in self.image.parts:
            if raw:cpu.mem_write(address,raw)
        request = card.REQUEST_RECORD.pack(card.REQUEST_MAGIC,1,self.nonce,0x41)
        cpu.mem_write(card.REQUEST,request)
        if mutate:mutate(cpu,self)
        self.mmio(cpu)
        def code(cpu,address,size,unused):
            ins = next(self.cs64.disasm(bytes(cpu.mem_read(address,4)),address))
            if ins.mnemonic == 'mrs':
                target,name = ins.op_str.split(', ')
                cpu.reg_write(getattr(a64,'UC_ARM64_REG_'+target.upper()),self.system[name])
                cpu.reg_write(a64.UC_ARM64_REG_PC,address+4)
            elif ins.mnemonic == 'msr':
                if ins.op_str.startswith('rmr_el3'):
                    assert cpu.reg_read(a64.UC_ARM64_REG_X0) == 2
                    assert self.regs[0x08001004] == card.GATE and self.regs[0x08001008] == 0
                    self.terminal = 'reset-to-gate';cpu.emu_stop()
                else:
                    assert ins.op_str.startswith('daifset')
                    cpu.reg_write(a64.UC_ARM64_REG_PC,address+4)
            elif ins.mnemonic == 'wfi':
                self.terminal = 'park';cpu.emu_stop()
        cpu.hook_add(UC_HOOK_CODE,code)
        cpu.emu_start(card.ENTRY,0,count=10000)
        assert self.terminal is not None
        self.report = bytes(cpu.mem_read(card.REPORT,64))
        self.gate = bytes(cpu.mem_read(card.GATE,self.profile['gate_bytes']))
        return self.terminal

    def gate_run(self, gate=None, base=card.GATE):
        cpu = Uc(UC_ARCH_ARM,UC_MODE_ARM)
        cpu.mem_map(base & ~4095,4096);cpu.mem_map(0,4096);cpu.mem_map(0x47000,4096)
        cpu.mem_write(base,gate or self.gate)
        self.mmio(cpu)
        self.terminal = None
        def code(cpu,address,size,unused):
            if address in (0x20,0x47b00):
                self.terminal = 'rom' if address == 0x20 else 'cold';cpu.emu_stop();return
            ins = next(self.cs32.disasm(bytes(cpu.mem_read(address,4)),address))
            if ins.mnemonic == 'mrc':
                assert 'c1, c0' in ins.op_str
                reg = re.search(r'\br\d+\b',ins.op_str)[0]
                cpu.reg_write(getattr(arm,'UC_ARM_REG_'+reg.upper()),self.sctlr)
                cpu.reg_write(arm.UC_ARM_REG_PC,address+4)
            elif ins.mnemonic == 'mrrc':
                cpu.reg_write(arm.UC_ARM_REG_R2,self.counter & 0xffffffff)
                cpu.reg_write(arm.UC_ARM_REG_R3,self.counter >> 32)
                cpu.reg_write(arm.UC_ARM_REG_PC,address+4)
            elif ins.mnemonic == 'mcr':
                if 'c1, c0' in ins.op_str:
                    self.sctlr = cpu.reg_read(arm.UC_ARM_REG_R1)
                else:
                    assert 'c7, c5' in ins.op_str
                    self.events.append(('cache',ins.op_str))
                cpu.reg_write(arm.UC_ARM_REG_PC,address+4)
            elif ins.mnemonic == 'wfi':
                self.terminal = 'park';cpu.emu_stop()
        cpu.hook_add(UC_HOOK_CODE,code)
        cpu.emu_start(base,0,count=10000)
        assert self.terminal is not None
        raw = bytes(cpu.mem_read(base,self.profile['gate_bytes']))
        self.state = struct.unpack_from('<I',raw,self.profile['state_offset'])[0]
        self.gate_after = raw
        return self.terminal


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args = parser.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    image, info = Image(args.image), card.inspect(args.image)
    cases = []
    for policy in (0,0x100):
        m = Machine(image,info,0x123456789abc,policy=policy)
        assert m.launcher() == 'reset-to-gate'
        assert card.REPORT_RECORD.unpack(m.report) == (card.REPORT_MAGIC,1,64,m.nonce,1,card.FEL_FLAG,0x41,12)
        assert m.gate_run() == 'rom' and m.state == card.GATE_COMPLETE
        assert m.regs[card.RTC_FLAG] == 0 and m.regs[0x02050014] == 0
        for name,address in FAULT_WORDS.items():
            assert m.regs[address]==FAULT_SENTINELS[name], 'Retained '+name.upper()+' changed'
        assert m.regs[0x07090104] == 0x05030000
        assert m.regs.get(crumbs.RTC+crumbs.WORDS['time'],0) == (m.counter >> 10) & 0x1fffffff
        assert not m.sctlr & 0x1000
        writes = [e for e in m.events if e[0]=='write']
        assert next(i for i,e in enumerate(writes)if e[1]==0x02050014) < next(i for i,e in enumerate(writes)if e[1]==card.RTC_FLAG)
        assert all(e[1] in (0x08001004,0x08001008,0x07090100,0x07090104,
                           crumbs.RTC+crumbs.WORDS['time'],card.RTC_FLAG,0x02050014)for e in writes)
        cases.append('linked-launcher-and-gate-policy-%x'%policy)
    for name,edit in (
        ('wrong-EL',lambda c,m:m.system.update(currentel=4)),
        ('translation-on',lambda c,m:m.system.update(sctlr_el3=1)),
        ('data-cache-on',lambda c,m:m.system.update(sctlr_el3=4)),
        ('instruction-cache-on',lambda c,m:m.system.update(sctlr_el3=0x1000)),
        ('missing-request',lambda c,m:m.regs.update({card.RTC_FLAG:0})),
        ('wrong-watchdog',lambda c,m:m.regs.update({0x02050014:0xb1})),
        ('redirect',lambda c,m:m.regs.update({0x070501d4:0xefe8})),
        ('core-ON',lambda c,m:m.regs.update({0x07050204:3})),
        ('hotplug',lambda c,m:m.regs.update({0x07050224:1})),
        ('policy-mismatch',lambda c,m:m.regs.update({0x07053008:0})),
        ('zero-nonce',lambda c,m:c.mem_write(card.REQUEST+16,bytes(8)))):
        m = Machine(image,info,1)
        assert m.launcher(edit)=='park'
        writes=[e for e in m.events if e[0]=='write']
        assert all(e[1] in (0x07090100,0x07090104) for e in writes),name
        if writes:
            assert m.regs[0x07090104]>>16==0x0c01 and m.regs[0x07090104]&0xffff,name
        cases.append('launcher-refuses-'+name)
    for variant in ('stuck-disarm','stuck-clear','lost-request','late-redirect','unknown-watchdog'):
        m = Machine(image,info,2,variant=variant)
        assert m.launcher()=='reset-to-gate'
        if variant=='lost-request':m.regs[card.RTC_FLAG]=0
        if variant=='late-redirect':m.regs[0x070501d4]=0xefe8
        if variant=='unknown-watchdog':m.regs[0x02050014]=0x31
        result=m.gate_run()
        if variant=='stuck-clear':
            assert result=='rom' and m.state==0 and m.regs[0x02050014]==0
        else:
            assert result=='park' and m.state==0
            assert m.regs[card.RTC_FLAG]==(0 if variant=='lost-request' else card.FEL_FLAG)
        cases.append('gate-'+variant)
    for text,expected in (('r1, [r0]','rom'),('r2, [r5]','park')):
        m=Machine(image,info,4);assert m.launcher()=='reset-to-gate'
        instructions=list(m.cs32.disasm(m.gate,card.GATE))
        stores=[i for i in instructions if i.mnemonic=='str' and i.op_str==text]
        assert len(stores)==1
        changed=bytearray(m.gate);offset=stores[0].address-card.GATE
        struct.pack_into('<I',changed,offset,0xe320f000)  # ARM NOP: omit the actual store.
        assert m.gate_run(bytes(changed))==expected and m.state==0
        assert m.regs[card.RTC_FLAG]==card.FEL_FLAG
        cases.append('mutation-omitted-'+('request-clear'if text.startswith('r1')else'watchdog-disarm'))
    raw=next(data[card.GATE-address:card.GATE-address+info['profile']['gate_bytes']]
             for address,data in image.parts if address<=card.GATE<address+len(data))
    for flag,expected in ((0,'cold'),(card.FEL_FLAG,'rom')):
        m=Machine(image,info,3);m.regs[card.RTC_FLAG]=flag;m.regs[0x02050014]=0
        m.regs[0x07090104]=0x01115000
        assert m.gate_run(raw,base=0x83000)==expected
        assert m.regs[0x07090104]==0x01115000
        assert all(m.regs[address]==FAULT_SENTINELS[name]for name,address in FAULT_WORDS.items())
        cases.append('relocated-production-'+expected)
    spec=module('card_fel_package','tools/card-fel-image.py')
    boot=Path.home()/'.cache/sunstoneos/sources/board-boot/boot0_sdcard_orangepizero3w.fex'
    original=boot.read_bytes();patched=spec.first_stage(original,raw)
    assert len(patched)==246272 and struct.unpack_from('<I',patched,12)[0]==spec.checksum(patched)
    branch=struct.unpack_from('<I',patched)[0]
    assert spec.BASE+8+(branch&0xffffff)*4==0x83000
    restored=bytearray(patched[:len(original)]);restored[:4]=original[:4];restored[12:20]=original[12:20]
    assert restored==original
    refused(lambda:spec.first_stage(original[:-4],raw),'Pinned boot0')
    cases.append('pinned-first-stage-only-header-and-append-changes')
    # Execute the actual captured-ROM size predicate, including its real BL.
    # This establishes neither SD transfer nor the separate checksum path.
    rom=(Path.home()/'.cache/sunstoneos/evidence/rom-copy-20261016-084055/physical-rom.bin').read_bytes()
    assert hashlib.sha256(rom).hexdigest()=='a8e31821e8a1611d75867825914c58373c732742cb8f2235e8168c1d7e93b5aa'
    for length,expected in ((0x3c000,True),(0x3c200,True),(0xa7800,True),(0x3c201,False),(0xa7a00,False)):
        cpu=Uc(UC_ARCH_ARM,UC_MODE_ARM);cpu.mem_map(0,0x20000);cpu.mem_write(0,rom)
        result=[]
        def stop(cpu,address,size,unused):
            if address in (0xae78,0xae94):result.append(address==0xae94);cpu.emu_stop()
        cpu.hook_add(UC_HOOK_CODE,stop);cpu.reg_write(arm.UC_ARM_REG_R6,length)
        cpu.emu_start(0xae58,0,count=100)
        assert result==[expected],hex(length)
        cases.append('captured-ROM-length-'+hex(length))
    (args.output/'result.json').write_text(json.dumps({'passed':True,'physical_access':False,
        'image_sha256':hashlib.sha256(args.image.read_bytes()).hexdigest(),'cases':cases,
        'limits':['No ROM FEL dispatch, electrical retention, storage loading or physical reset is modeled.']},indent=2)+'\n')
    print('PASS linked card FEL recovery: %d cases'%len(cases))


if __name__=='__main__':main()
