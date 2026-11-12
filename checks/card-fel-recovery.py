#!/usr/bin/env python3
"""Execute the immutable ARM32 SRAM gate; retain only RTC evidence.

No reset dispatch or DRAM receipt is modeled. Execution starts at the FEL exe
address and stops at the ROM branch or the original cold boot0 reset stub.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import struct
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE
from unicorn import arm_const as arm

ROOT=Path(__file__).resolve().parents[1]
CHECK_INPUTS=['boot/a733/card-fel-gate.S','apps/card-fel-recovery/entry.S',
 'apps/card-fel-recovery/link.ld','include/a733_card_fel_gate.h','tools/card_recovery.py',
 'tools/card-fel-image.py','tools/a733-crumbs.py','patches/sunxi-a733-sram.patch']
GATE_RUNS=[['--image','{elf:card-fel-recovery}','--output','{output}']]


def module(name,path):
    spec=importlib.util.spec_from_file_location(name,ROOT/path)
    value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value)
    return value


card=module('direct_card_recovery','tools/card_recovery.py')
crumbs=module('direct_gate_crumbs','tools/a733-crumbs.py')
FAULT_WORDS={name:crumbs.RTC+crumbs.WORDS[name]for name in ('esr','elr','far')}
SENTINELS=dict(zip(FAULT_WORDS,(0x11111111,0x22222222,0x33333333)))
# Measured FEL caller: docs/boot.md; the patched MMIO helper clears I before exe.
FEL_SCTLR=0x00c51838


class Machine:
    def __init__(self,raw,base=card.GATE,variant='',sctlr=FEL_SCTLR & ~0x1000):
        self.raw,self.base,self.variant,self.sctlr=raw,base,variant,sctlr
        self.regs={card.RTC_TAG:0x5c0d0123,card.RTC_FLAG:card.FEL_FLAG,
                   0x02050010:1,0x02050014:0x41,0x02050018:2}
        self.regs.update({FAULT_WORDS[name]:value for name,value in SENTINELS.items()})
        self.events,self.terminal=[],None
        self.decoder=Cs(CS_ARCH_ARM,CS_MODE_ARM)

    def run(self,helper=False):
        cpu=Uc(UC_ARCH_ARM,UC_MODE_ARM)
        for base in sorted({self.base & ~4095,0,0x47000,0x70000}):
            cpu.mem_map(base,4096)
        cpu.mem_write(self.base,self.raw)
        cpu.reg_write(arm.UC_ARM_REG_CPSR,0x60000153)
        cpu.reg_write(arm.UC_ARM_REG_SP,0xf3400)  # no stack is mapped or used
        cpu.reg_write(arm.UC_ARM_REG_LR,0x70000)
        def read(cpu,offset,size,base):
            assert size==4
            value=self.regs.get(base+offset,0)
            self.events.append(('read',base+offset,value))
            return value
        def write(cpu,offset,size,value,base):
            assert size==4
            address=base+offset
            self.events.append(('write',address,value))
            if address==0x02050014:
                assert value==0x16aa0000
                if self.variant=='stuck-disarm':return
                value &= 0xffff
            if address==card.RTC_FLAG and self.variant=='stuck-clear':return
            self.regs[address]=value
        for base in (0x02050000,0x07050000,0x07090000):
            cpu.mmio_map(base,4096,read,base,write,base)
        def code(cpu,address,size,unused):
            targets={0x70000:'helper-return'}if helper else {0x20:'rom',0x47b00:'cold'}
            if address in targets:
                self.terminal=targets[address];cpu.emu_stop();return
            assert cpu.reg_read(arm.UC_ARM_REG_SP)==0xf3400
            ins=next(self.decoder.disasm(bytes(cpu.mem_read(address,4)),address))
            if ins.mnemonic=='mrc':
                assert 'c1, c0' in ins.op_str
                reg=re.search(r'\br\d+\b',ins.op_str)[0]
                cpu.reg_write(getattr(arm,'UC_ARM_REG_'+reg.upper()),self.sctlr)
                cpu.reg_write(arm.UC_ARM_REG_PC,address+4)
            elif ins.mnemonic=='mcr':
                reg=re.search(r'\br\d+\b',ins.op_str)[0]
                if 'c1, c0' in ins.op_str:
                    self.sctlr=cpu.reg_read(getattr(arm,'UC_ARM_REG_'+reg.upper()))
                else:
                    assert 'c7, c5' in ins.op_str
                    self.events.append(('cache',ins.op_str))
                cpu.reg_write(arm.UC_ARM_REG_PC,address+4)
            elif ins.mnemonic=='wfi':
                self.terminal='park';cpu.emu_stop()
            elif ins.mnemonic in ('dsb','isb'):
                self.events.append((ins.mnemonic,))
        cpu.hook_add(UC_HOOK_CODE,code)
        cpu.emu_start(self.base,0,count=10000)
        assert self.terminal is not None
        self.after=bytes(cpu.mem_read(self.base,len(self.raw)))
        return self.terminal


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    info=card.inspect(args.image)
    raw=next(data for address,data in info['uploads']if address==card.GATE)
    assert len(raw)==info['profile']['gate_bytes']<=512
    assert not any(address>=0x40000000 for address,data in info['uploads'])
    cases=[]
    for base in (card.GATE,0x83000):
        for state in (0,FEL_SCTLR):
            m=Machine(raw,base=base,sctlr=state)
            assert m.run()=='rom' and m.after==raw
            assert m.regs[card.RTC_FLAG]==0 and m.regs[0x02050014]==0
            assert not m.sctlr & 0x1005
            assert all(m.regs[address]==SENTINELS[name]for name,address in FAULT_WORDS.items())
            progress=[value for kind,address,value in (e for e in m.events if e[0]=='write')if address==card.RTC_LAST]
            assert progress==[card.RTC_STAGE<<16|n for n in range(1,6)]
            writes=[e for e in m.events if e[0]=='write']
            assert all(e[1]in (card.RTC_LAST,card.RTC_FLAG,0x02050014)for e in writes)
            disarm=next(i for i,e in enumerate(m.events)if e[0]=='write'and e[1]==0x02050014)
            clear=next(i for i,e in enumerate(m.events)if e[0]=='write'and e[1]==card.RTC_FLAG)
            assert disarm<clear
            assert any(e[0]=='cache'for e in m.events[:disarm])
            for i,e in enumerate(m.events):
                if e[0]=='write'and e[1]==card.RTC_LAST:
                    assert m.events[i+1][0]=='dsb'
            words={key:m.regs.get(crumbs.RTC+offset,0)for key,offset in crumbs.WORDS.items()}
            assert crumbs.decode(words,0x123)['last']['gate_progress']=='before ROM branch'
            cases.append('direct-ARM32-%x-SCTLR-%x'%(base,state))
    for name,address,value,expected in (
            ('MMU',None,1,0x81),('D-cache',None,4,0x81),
            ('redirect-C0',0x070501c0,1,0x82),('redirect-C4',0x070501c4,1,0x82),
            ('redirect-D4',0x070501d4,1,0x82),('watchdog-mode',0x02050014,0x31,0x83),
            ('watchdog-config',0x02050010,2,0x83),('watchdog-output',0x02050018,1,0x83)):
        m=Machine(raw)
        if address is None:m.sctlr |= value
        else:m.regs[address]=value
        assert m.run()=='park' and m.regs[card.RTC_LAST]==card.RTC_STAGE<<16|expected
        assert m.regs[card.RTC_FLAG]==card.FEL_FLAG
        assert not any(e[0]=='write'and e[1]==0x02050014 for e in m.events)
        cases.append('refuses-'+name)
    for mode in (0,1,0xb1):
        m=Machine(raw);m.regs[0x02050014]=mode
        assert m.run()=='rom' and m.regs[card.RTC_FLAG]==0
        cases.append('recognized-watchdog-%x'%mode)
    for variant,last,terminal in (('stuck-disarm',0x86,'park'),('stuck-clear',0x84,'rom')):
        m=Machine(raw,variant=variant)
        assert m.run()==terminal and m.regs[card.RTC_LAST]==card.RTC_STAGE<<16|last
        assert m.regs[card.RTC_FLAG]==card.FEL_FLAG
        assert (m.regs[0x02050014]==0)==(variant=='stuck-clear')
        cases.append(variant)
    for text,last,terminal in (('r2, [r5]',0x86,'park'),('r1, [r0]',0x84,'rom')):
        instructions=list(Cs(CS_ARCH_ARM,CS_MODE_ARM).disasm(raw,card.GATE))
        stores=[i for i in instructions if i.mnemonic=='str'and i.op_str==text]
        assert len(stores)==1
        changed=bytearray(raw);struct.pack_into('<I',changed,stores[0].address-card.GATE,0xe320f000)
        m=Machine(bytes(changed));assert m.run()==terminal
        assert m.regs[card.RTC_LAST]==card.RTC_STAGE<<16|last and m.regs[card.RTC_FLAG]==card.FEL_FLAG
        cases.append('mutation-omitted-'+text)
    m=Machine(raw);m.regs[card.RTC_FLAG]=0;m.regs[0x02050014]=0;m.regs[card.RTC_LAST]=0x01115000
    assert m.run()=='cold' and not any(e[0]=='write' for e in m.events)
    assert m.regs[card.RTC_LAST]==0x01115000
    cases.append('cold-card-path-writes-nothing-and-enters-original-boot0')
    # The runner's last transaction before exe is an RTC MMIO command. In the
    # reviewed tool every MMIO command first runs aw_disable_icache, so execute
    # that helper's instructions from its pinned source and then the gate.
    source_path=Path.home()/'.cache/sunstoneos/build/sunxi-a733/fel_lib.c'
    assert hashlib.sha256(source_path.read_bytes()).hexdigest()== \
        'dc73cba1e4acb3b6edb4ab034cc491ce68f3e1979e45b21433198b04f7017d05'
    source=source_path.read_text()
    section=source.split('static void aw_disable_icache(',1)[1].split('void aw_fel_write(',1)[0]
    helper=struct.pack('<6I',*[int(word,16)for word in re.findall(r'htole32\((0x[0-9a-f]+)\)',section)])
    assert 'aw_disable_icache(dev)' in source.split('void aw_fel_write(',1)[1]
    assert re.search(r'\.icache_fix\s*=\s*true',(ROOT/'patches/sunxi-a733-sram.patch').read_text())
    m=Machine(helper,base=0x71000,sctlr=FEL_SCTLR)
    assert m.run(helper=True)=='helper-return' and not m.sctlr&0x1000
    assert any(e[0]=='cache' for e in m.events)
    gate=Machine(raw,sctlr=m.sctlr);assert gate.run()=='rom'
    assert hashlib.sha256(gate.after).hexdigest()==info['profile']['gate_sha256']
    cases.append('tool-cache-helper-then-fresh-SRAM-exe')
    package=module('direct_gate_package','tools/card-fel-image.py')
    original=(Path.home()/'.cache/sunstoneos/sources/board-boot/boot0_sdcard_orangepizero3w.fex').read_bytes()
    patched=package.first_stage(original,raw)
    assert len(patched)==246272 and struct.unpack_from('<I',patched,12)[0]==package.checksum(patched)
    restored=bytearray(patched[:len(original)]);restored[:4]=original[:4];restored[12:20]=original[12:20]
    assert restored==original
    assert patched[len(original):len(original)+len(raw)]==raw
    cases.append('card-package-and-SRAM-proof-use-identical-gate-bytes')
    rom=(Path.home()/'.cache/sunstoneos/evidence/rom-copy-20261016-084055/physical-rom.bin').read_bytes()
    assert hashlib.sha256(rom).hexdigest()=='a8e31821e8a1611d75867825914c58373c732742cb8f2235e8168c1d7e93b5aa'
    for length,expected in ((0x3c200,True),(0xa7800,True),(0x3c201,False),(0xa7a00,False)):
        cpu=Uc(UC_ARCH_ARM,UC_MODE_ARM);cpu.mem_map(0,0x20000);cpu.mem_write(0,rom)
        result=[]
        def stop(cpu,address,size,unused):
            if address in (0xae78,0xae94):result.append(address==0xae94);cpu.emu_stop()
        cpu.hook_add(UC_HOOK_CODE,stop);cpu.reg_write(arm.UC_ARM_REG_R6,length)
        cpu.emu_start(0xae58,0,count=100)
        assert result==[expected]
        cases.append('captured-ROM-length-'+hex(length))
    (args.output/'result.json').write_text(json.dumps({'passed':True,'physical_access':False,
        'image_sha256':hashlib.sha256(args.image.read_bytes()).hexdigest(),'gate_sha256':info['profile']['gate_sha256'],
        'cases':cases,'limits':['FEL transport is modeled in board-runner.py; ROM USB dispatch and electrical retention require the one physical proof.']},indent=2)+'\n')
    print('PASS direct ARM32 SRAM gate: %d cases'%len(cases))


if __name__=='__main__':main()
