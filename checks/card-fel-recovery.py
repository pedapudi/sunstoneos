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
                'apps/card-fel-recovery/profile.c', 'apps/card-fel-recovery/admission.c',
                'apps/card-fel-recovery/refusal.h', 'include/a733_cpu_off.h',
                'include/a733_cpu_reset.h', 'checks/a733-cpu-reset.py',
                'platform/a733/cpu_start.c', 'tools/card_recovery.py', 'tools/card-fel-image.py']
GATE_RUNS = [['--image', '{elf:card-fel-recovery}', '--output', '{output}']]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    result = importlib.util.module_from_spec(spec); spec.loader.exec_module(result)
    return result


card = module('linked_card_recovery', 'tools/card_recovery.py')
reset_check = module('card_recovery_reset_wait', 'checks/a733-cpu-reset.py')
crumbs = module('card_recovery_crumb_layout', 'tools/a733-crumbs.py')
FAULT_WORDS = {name: crumbs.RTC+crumbs.WORDS[name] for name in ('esr','elr','far')}
FAULT_SENTINELS = dict(zip(FAULT_WORDS, (0x11111111,0x22222222,0x33333333)))


class Machine:
    def __init__(self, image, info, nonce, variant='', policy=0x100):
        self.image, self.profile, self.nonce = image, info['profile'], nonce
        self.variant, self.events, self.terminal = variant, [], None
        self.stack_used = 0
        self.reset_pending,self.reset_wfi_count,self.reset_events_index = False,0,None
        self.regs = {card.RTC_FLAG: card.FEL_FLAG, 0x02050010: 1, 0x02050014: 0x41, 0x02050018: 2}
        self.regs.update({FAULT_WORDS[name]:value for name,value in FAULT_SENTINELS.items()})
        for core in range(1,8):
            # cpu_start.c admits FEL dynamic OFF with clear hotplug control;
            # bit 1 is required only for stage-2 static OFF (docs/cpu-start.md).
            self.regs[0x07050200 + core*4] = 2 if policy == 0 else 0
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
            sp = cpu.reg_read(a64.UC_ARM64_REG_SP)
            if sp:
                low,high = self.image.address('a733_card_fel_stack_bottom'),self.image.address('a733_card_fel_stack_top')
                assert low <= sp <= high, ('launcher stack',hex(sp),hex(low),hex(high))
                self.stack_used = max(self.stack_used,high-sp)
            if self.variant == 'stuck-publication' and ins.mnemonic == 'ldr' and \
                    ins.op_str == 'w0, [x5]' and cpu.reg_read(a64.UC_ARM64_REG_X5) == card.GATE+self.profile['test_offset']:
                cpu.mem_write(card.GATE+self.profile['test_offset'],bytes(4))
            if ins.mnemonic == 'mrs':
                target,name = ins.op_str.split(', ')
                value = self.counter if name == 'cntpct_el0' else self.system[name]
                cpu.reg_write(getattr(a64,'UC_ARM64_REG_'+target.upper()),value)
                cpu.reg_write(a64.UC_ARM64_REG_PC,address+4)
            elif ins.mnemonic == 'msr':
                if ins.op_str.startswith('rmr_el3'):
                    assert cpu.reg_read(a64.UC_ARM64_REG_X0) == 2
                    assert self.regs[0x08001004] == card.GATE and self.regs[0x08001008] == 0
                    self.system['rmr_el3'] = 2
                    self.reset_pending = True
                    self.reset_wfi_address = address+8
                    self.reset_events_index = len(self.events)
                    cpu.reg_write(a64.UC_ARM64_REG_PC,address+4)
                else:
                    assert ins.op_str.startswith('daifset')
                    cpu.reg_write(a64.UC_ARM64_REG_PC,address+4)
            elif ins.mnemonic == 'wfi':
                if self.reset_pending:
                    assert address==self.reset_wfi_address, 'reset must reach immediate WFI'
                    self.reset_wfi_count += 1
                    if self.variant != 'no-reset':
                        self.terminal='reset-to-gate';cpu.emu_stop()
                    elif self.reset_wfi_count==2:
                        self.terminal='reset-pending';cpu.emu_stop()
                    else:
                        cpu.reg_write(a64.UC_ARM64_REG_PC,address+4)
                else:
                    self.terminal = 'park';cpu.emu_stop()
            elif ins.mnemonic == 'dsb':
                self.events.append(('barrier',))
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
    reset_check.reset_wait(image.parts)
    cases,stack_used = [],0
    for policy in (0,0x100):
        m = Machine(image,info,0x123456789abc,policy=policy)
        assert m.launcher() == 'reset-to-gate', ('OFF admission',hex(policy),m.events)
        stack_used=max(stack_used,m.stack_used)
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
    m=Machine(image,info,7)
    for c in range(1,8):m.regs[0x07050200+c*4]=0x42
    assert m.launcher()=='reset-to-gate'
    cases.append('FEL-OFF-preserves-unrelated-control-bits')
    failures=[]
    for name,code,register,sample in (
            ('EL',1,'currentel',4),('MPIDR',2,'mpidr_el1',0xc1000101),
            ('translation',3,'sctlr_el3',0x10001),('data-cache',4,'sctlr_el3',0x10004),
            ('instruction-cache',5,'sctlr_el3',0x1000)):
        failures.append((name,code,0,sample,lambda c,m,r=register,v=sample:m.system.update({r:v}),''))
    for name,code,offset,sample in (
            ('request-magic',6,0,0x1234567800000000),('request-version',7,8,2),
            ('zero-nonce',8,16,0),('request-watchdog',9,24,0x100000041)):
        failures.append((name,code,card.REQUEST+offset,sample,
                         lambda c,m,o=offset,v=sample:c.mem_write(card.REQUEST+o,struct.pack('<Q',v)),''))
    for name,code,address,sample in (
            ('watchdog-config',10,0x02050010,0x12340002),('watchdog-mode',11,0x02050014,0x123400b1),
            ('watchdog-output',12,0x02050018,0x12340003),('RTC-request',13,card.RTC_FLAG,0),
            ('redirect-C0',14,0x070501c0,0x10),('redirect-C4',15,0x070501c4,0x20),
            ('redirect-D4',16,0x070501d4,0xefe8)):
        failures.append((name,code,address,sample,lambda c,m,a=address,v=sample:m.regs.update({a:v}),''))
    failures += [('publication',17,card.GATE+info['profile']['test_offset'],0,None,'stuck-publication')]
    for core in range(1,8):
        for name,base,address,sample in (
                ('ON',0x100,0x07052008+core*0x1000,0x108),
                ('control-request',0x200,0x07050200+core*4,0x4001),
                ('mode-request',0x300,0x07050220+core*4,0x8001),
                ('policy',0x400,0x07052000+core*0x1000,0x1234),
                ('policy-status',0x500,0x07052008+core*0x1000,0),
                ('static-wake-hold',0x600,0x07050200+core*4,0)):
            def edit(c,m,a=address,v=sample,n=name,core=core):
                m.regs[a]=v
                if n=='static-wake-hold':
                    m.regs[0x07052000+core*0x1000]=0
                    m.regs[0x07052008+core*0x1000]=0
            failures.append((name+'-core%d'%core,base+core,address,sample,edit,''))
    for name,code,address,sample,edit,variant in failures:
        m = Machine(image,info,1,variant=variant)
        assert m.launcher(edit)=='park'
        stack_used=max(stack_used,m.stack_used)
        writes=[e for e in m.events if e[0]=='write']
        allowed=(0x07090100,0x07090104,0x0709010c,0x07090114,0x07090118,0x0709011c)
        assert all(e[1] in allowed for e in writes),name
        assert m.regs[0x07090104]==0x0c010000|code,name
        assert m.regs[0x07090114]==sample&0xffffffff,name
        assert m.regs[0x07090118]==sample>>32,name
        assert m.regs[0x0709011c]==address|0x80000000,name
        publish=max(i for i,e in enumerate(m.events)if e[0]=='write' and e[1]==0x07090104)
        payload=max(i for i,e in enumerate(m.events)if e[0]=='write' and e[1] in (0x07090114,0x07090118,0x0709011c))
        assert any(e[0]=='barrier'for e in m.events[payload+1:publish]),name
        words={key:m.regs.get(crumbs.RTC+offset,0)for key,offset in crumbs.WORDS.items()}
        decoded=crumbs.decode(words,0 if name=='zero-nonce'else m.nonce)
        witness=decoded['last']['launcher_refusal']
        assert witness['sample']==hex(sample) and witness['address']==hex(address),name
        assert witness['guard']!='unknown' and 'sampled '+hex(sample) in decoded['summary'],name
        assert 'fault' not in decoded and 'board_failure' not in decoded,name
        cases.append('launcher-refuses-'+name)
    m=Machine(image,info,1)
    fault_magic=crumbs.header()[0]['FAULT_MAGIC']
    m.regs[crumbs.RTC+crumbs.WORDS['fault']]=fault_magic<<24|3<<16
    assert m.launcher(lambda c,m:m.regs.update({0x07053008:0x108}))=='park'
    assert all(m.regs[address]==FAULT_SENTINELS[name]for name,address in FAULT_WORDS.items())
    words={key:m.regs.get(crumbs.RTC+offset,0)for key,offset in crumbs.WORDS.items()}
    decoded=crumbs.decode(words,m.nonce)
    assert 'fault' in decoded and 'launcher_refusal' not in decoded['last']
    cases.append('launcher-retains-existing-fault')
    words['fault']=0;words['far']=0;words['elr']=0;words['esr']=0
    assert 'launcher_refusal' not in crumbs.decode(words,m.nonce)['last']
    cases.append('legacy-refusal-has-no-invented-sample')
    m=Machine(image,info,1,variant='no-reset')
    assert m.launcher()=='reset-pending' and m.reset_wfi_count==2
    assert not any(e[0]=='write'for e in m.events[m.reset_events_index:])
    assert m.regs[0x07090104]==0x0c010000
    assert all(m.regs[address]==FAULT_SENTINELS[name]for name,address in FAULT_WORDS.items())
    assert m.regs[card.RTC_FLAG]==card.FEL_FLAG and m.regs[0x02050014]==0x41
    cases.append('pending-reset-WFI-loop-writes-no-RTC-refusal')
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
        'image_sha256':hashlib.sha256(args.image.read_bytes()).hexdigest(),'cases':cases,'stack_high_water_bytes':stack_used,
        'limits':['No ROM FEL dispatch, electrical retention, storage loading or physical reset is modeled.']},indent=2)+'\n')
    print('PASS linked card FEL recovery: %d cases'%len(cases))


if __name__=='__main__':main()
