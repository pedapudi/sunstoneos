#!/usr/bin/env python3
"""Execute the immutable ARM32 SRAM gate under both WDT0 policies and every entry state.

Entry states: the vendor FEL request, a watchdog return (cold-boot marker
set) and a cold power-on (marker zero). No reset dispatch or DRAM receipt is
modeled. Execution stops at the ROM branch, a park or boot0's reset stub.
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
chain=module('direct_gate_chain','tools/boot_chain.py')
FAULT_WORDS={name:crumbs.RTC+crumbs.WORDS[name]for name in ('esr','elr','far')}
SENTINELS=dict(zip(FAULT_WORDS,(0x11111111,0x22222222,0x33333333)))
# Measured FEL caller: docs/boot.md; the patched MMIO helper clears I before exe.
FEL_SCTLR=0x00c51838
WDT_RESTART=0x0205000c


MARKER=card.RTC_MARKER
KEYED=0x16aa16aa
STAGE=card.RTC_STAGE<<16
WARM,COLD,UNMARKED,ONCE=0x4000,0x8000,0x2000,0x1000
BOOT_ONCE=crumbs.BOOT_ONCE
IMAGE_CRUMB=0x01115000          # an image's own LAST breadcrumb, which a warm entry keeps
WDT_MODE,WDT_CONFIG,WDT_OUTPUT=0x02050014,0x02050010,0x02050018


class Machine:
    """Entry state: the request (default), or no request with the marker zero
    (cold power-on) or set (watchdog return)."""
    def __init__(self,raw,base=card.GATE,variant='',sctlr=FEL_SCTLR & ~0x1000,entry='request'):
        self.raw,self.base,self.variant,self.sctlr=raw,base,variant,sctlr
        self.regs={card.RTC_TAG:0x5c0d0123,card.RTC_FLAG:card.FEL_FLAG if entry=='request' else 0,
                   card.RTC_LAST:IMAGE_CRUMB,MARKER:KEYED if entry=='warm' else 0,
                   WDT_CONFIG:1,WDT_MODE:0x41,WDT_OUTPUT:2}
        self.regs.update({FAULT_WORDS[name]:value for name,value in SENTINELS.items()})
        self.events,self.terminal,self.restarts=[],None,0
        self.decoder=Cs(CS_ARCH_ARM,CS_MODE_ARM)

    def run(self,helper=False):
        cpu=Uc(UC_ARCH_ARM,UC_MODE_ARM)
        for base in sorted({self.base & ~4095,(self.base+len(self.raw)-1)&~4095,0,0x47000,0x70000}):
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
            if address in (WDT_MODE,WDT_CONFIG,WDT_OUTPUT):
                assert value>>16==0x16aa  # A733 manual 8.3.6.5-8.3.6.7
                if address==WDT_MODE:
                    assert value&0xffff in (0,card.GATE_WATCHDOG)
                    if self.variant=='stuck-mode':return
                value &= 0xffff
            if address==WDT_RESTART:
                assert value==0x14af and self.regs[WDT_MODE]&1  # A733 manual 8.3.6.4
                self.restarts+=1;return
            if address==MARKER:
                # A733 manual 9.1.7.20: only a keyed write takes effect.
                if self.variant=='stuck-marker' or value>>16!=0x16aa:return
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

    def writes(self):
        return [e for e in self.events if e[0]=='write']

    def untouched(self):
        """The image's breadcrumb and fault words survive."""
        return all(self.regs[address]==SENTINELS[key]for key,address in FAULT_WORDS.items())


def store(code,text,before=None):
    """The first instruction 'str <text>' in the gate (before the cold path when given)."""
    found=[i for i in Cs(CS_ARCH_ARM,CS_MODE_ARM).disasm(code,card.GATE)
           if i.mnemonic=='str' and i.op_str==text and (before is None or i.address<before)]
    assert found
    return found[0].address-card.GATE


def without(raw,offset):
    changed=bytearray(raw);struct.pack_into('<I',changed,offset,0xe320f000)  # NOP
    return bytes(changed)


def request_cases(name,raw,policy,code,cases):
    for base in (card.GATE,0x83000):
        for state in (0,FEL_SCTLR):
            m=Machine(raw,base=base,sctlr=state)
            assert m.run()=='rom' and m.after==raw
            assert m.regs[card.RTC_FLAG]==0 and m.regs[WDT_MODE]==policy and m.regs[MARKER]==0
            assert m.restarts==(policy!=0)
            assert not m.sctlr & 0x1005 and m.untouched()
            writes=m.writes()
            progress=[value for kind,address,value in writes if address==card.RTC_LAST]
            assert progress==[STAGE|n for n in range(1,6)]
            assert all(e[1]in (card.RTC_LAST,card.RTC_FLAG,WDT_MODE,WDT_RESTART)for e in writes)
            order=[e[1] for e in writes if e[1] in (WDT_MODE,WDT_RESTART,card.RTC_FLAG)]
            assert order==[*([WDT_MODE] if policy else []),WDT_MODE,*([WDT_RESTART] if policy else []),card.RTC_FLAG]
            # A re-arm starts from mode 0; a disarm is a single write.
            modes=[value for kind,address,value in writes if address==WDT_MODE]
            assert modes==([0x16aa0000,0x16aa0000|policy] if policy else [0x16aa0000]),modes
            mode=next(i for i,e in enumerate(m.events)if e[0]=='write'and e[1]==WDT_MODE)
            assert any(e[0]=='cache'for e in m.events[:mode])
            for i,e in enumerate(m.events):
                if e[0]=='write'and e[1]==card.RTC_LAST:
                    assert m.events[i+1][0]=='dsb'
            words={key:m.regs.get(crumbs.RTC+offset,0)for key,offset in crumbs.WORDS.items()}
            assert crumbs.decode(words,0x123)['last']['gate_progress']=='before ROM branch'
            cases.append('%s-request-ARM32-%x-SCTLR-%x'%(name,base,state))
    # Translation, caches or an armed ROM redirect make ROM 0x20 unsafe: park.
    for case,address,value,expected in (
            ('MMU',None,1,0x81),('D-cache',None,4,0x81),
            ('redirect-C0',0x070501c0,1,0x82),('redirect-C4',0x070501c4,1,0x82),
            ('redirect-D4',0x070501d4,1,0x82)):
        m=Machine(raw)
        if address is None:m.sctlr |= value
        else:m.regs[address]=value
        assert m.run()=='park' and m.regs[card.RTC_LAST]==STAGE|expected
        assert m.regs[card.RTC_FLAG]==card.FEL_FLAG and m.restarts==0
        assert not any(e[1]==WDT_MODE for e in m.writes())
        cases.append(name+'-request-parks-on-'+case)
    # Unmeasured WDT0 configuration or output: a disarm needs neither and
    # proceeds; an arm is refused, recorded and the request still selects FEL.
    for case,address,value in (('watchdog-mode',WDT_MODE,0x31),('watchdog-config',WDT_CONFIG,2),
                               ('watchdog-output',WDT_OUTPUT,1)):
        m=Machine(raw);m.regs[address]=value
        assert m.run()=='rom' and m.regs[card.RTC_FLAG]==0
        if policy==0:
            assert m.regs[WDT_MODE]==0 and m.regs[card.RTC_LAST]==STAGE|5
        else:
            assert m.regs[card.RTC_LAST]==STAGE|0x83 and m.restarts==0
            assert not any(e[1]==WDT_MODE for e in m.writes())
        cases.append(name+'-request-'+case+'-enters-FEL')
    # The previous gate refused and parked here: WDT0 reads 1 after a watchdog
    # reset and its configuration and output after that reset are unmeasured.
    m=Machine(raw);m.regs.update({WDT_MODE:1,WDT_CONFIG:0,WDT_OUTPUT:0x1f})
    assert m.run()=='rom' and m.regs[card.RTC_FLAG]==0
    assert m.regs[card.RTC_LAST]==(STAGE|5 if policy==0 else STAGE|0x83)
    assert m.regs[WDT_MODE]==(0 if policy==0 else 1)
    cases.append(name+'-request-after-watchdog-reset-unmeasured-registers-enters-FEL')
    for mode in (0,1,0x41,0xb1):
        m=Machine(raw);m.regs[WDT_MODE]=mode
        assert m.run()=='rom' and m.regs[card.RTC_FLAG]==0 and m.regs[WDT_MODE]==policy
        cases.append(name+'-request-recognized-watchdog-%x'%mode)
    m=Machine(raw);m.regs[WDT_MODE]=0;m.regs[WDT_CONFIG]=2
    assert m.run()=='rom' and m.regs[card.RTC_FLAG]==0
    assert m.regs[card.RTC_LAST]==(STAGE|5 if policy==0 else STAGE|0x83)
    cases.append(name+'-request-WDT0-off-configuration')
    for variant,last,flag in (('stuck-mode',0x86,0),('stuck-clear',0x84,card.FEL_FLAG)):
        m=Machine(raw,variant=variant)
        assert m.run()=='rom' and m.regs[card.RTC_LAST]==STAGE|last and m.regs[card.RTC_FLAG]==flag
        assert m.regs[WDT_MODE]==(policy if variant=='stuck-clear' else 0x41)
        cases.append(name+'-request-'+variant)
    cold=next(i.address for i in Cs(CS_ARCH_ARM,CS_MODE_ARM).disasm(code,card.GATE)
              if i.mnemonic=='ldr' and i.op_str.startswith('r2, [pc') and
              struct.unpack_from('<I',code,((i.address+8)-card.GATE)+int(i.op_str.split('#')[1].rstrip(']'),0))[0]==KEYED)
    for text,last,flag in (('r2, [r5]',0x86,0),('r8, [r0]',0x84,card.FEL_FLAG)):
        m=Machine(without(raw,store(code,text,cold)));assert m.run()=='rom'
        assert m.regs[card.RTC_LAST]==STAGE|last and m.regs[card.RTC_FLAG]==flag
        cases.append(name+'-mutation-omitted-'+text)
    return cold


def warm_cases(name,raw,policy,code,cold,cases):
    # A watchdog return through the card: marker set, no request, WDT0 mode 1,
    # configuration and output unmeasured.
    for config,output in ((1,2),(0,0x1f),(1,0x1f)):
        m=Machine(raw,entry='warm');m.regs.update({WDT_MODE:1,WDT_CONFIG:config,WDT_OUTPUT:output})
        assert m.run()=='rom' and m.regs[card.RTC_LAST]==IMAGE_CRUMB and m.untouched()
        assert m.regs[MARKER]==KEYED and not any(e[1]==MARKER for e in m.writes())
        refused=policy!=0 and (config,output)!=(1,2)
        assert m.regs[card.RTC_FLAG]==STAGE|WARM|(0x300 if refused else 0)|1
        assert m.regs[WDT_MODE]==(1 if refused else policy) and m.restarts==(policy!=0 and not refused)
        assert all(e[1] in (card.RTC_FLAG,WDT_MODE,WDT_RESTART) for e in m.writes())
        cases.append('%s-watchdog-return-config-%x-output-%x-enters-FEL'%(name,config,output))
    # The earlier cold record's flag survives; an unrelated GP2 value does not.
    for before,kept in ((STAGE|COLD|0x41,COLD),(0x12348000,0),(STAGE|WARM|COLD|1,COLD)):
        m=Machine(raw,entry='warm');m.regs[card.RTC_FLAG]=before;m.regs[WDT_MODE]=1
        assert m.run()=='rom' and m.regs[card.RTC_FLAG]==STAGE|WARM|kept|1
        cases.append('%s-warm-keeps-cold-flag-from-%x'%(name,before))
    # Any nonzero marker other than the keyed value also selects FEL.
    m=Machine(raw,entry='warm');m.regs[MARKER]=0x00001234
    assert m.run()=='rom' and m.regs[card.RTC_FLAG]>>16==card.RTC_STAGE and m.regs[card.RTC_FLAG]&WARM
    cases.append(name+'-unknown-marker-enters-FEL')
    for case,address,value,expected in (('MMU',None,1,0x81),('redirect-D4',0x070501d4,1,0x82)):
        m=Machine(raw,entry='warm')
        if address is None:m.sctlr |= value
        else:m.regs[address]=value
        assert m.run()=='park' and m.regs[card.RTC_FLAG]==STAGE|expected and m.regs[card.RTC_LAST]==IMAGE_CRUMB
        cases.append(name+'-warm-parks-on-'+case)
    m=Machine(raw,entry='warm',variant='stuck-clear')
    assert m.run()=='rom' and m.regs[card.RTC_LAST]==IMAGE_CRUMB
    cases.append(name+'-warm-record-unconfirmed-enters-FEL')


def cold_cases(name,raw,cases):
    # Cold power-on: the marker reads zero.
    for mode in (0,1):
        m=Machine(raw,entry='cold');m.regs[WDT_MODE]=mode
        assert m.run()=='cold' and m.regs[MARKER]==KEYED
        assert m.regs[card.RTC_FLAG]==STAGE|COLD|mode and m.regs[card.RTC_LAST]==IMAGE_CRUMB and m.untouched()
        assert (m.regs[WDT_OUTPUT],m.regs[WDT_CONFIG],m.regs[WDT_MODE],m.restarts)==(2,1,0xb1,1)
        order=[e[1] for e in m.writes()]
        assert order==[MARKER,card.RTC_FLAG,WDT_MODE,WDT_OUTPUT,WDT_CONFIG,WDT_MODE,WDT_RESTART]
        assert [e[2] for e in m.writes() if e[1]==WDT_MODE]==[0x16aa0000,0x16aa0000|card.GATE_WATCHDOG]
        assert m.sctlr==FEL_SCTLR & ~0x1000  # boot0's reset stub sets its own SCTLR
        cases.append('%s-cold-marks-arms-16s-and-enters-boot0-from-mode-%x'%(name,mode))
    # A marker that does not read back cannot record the epoch: FEL.
    m=Machine(raw,entry='cold',variant='stuck-marker')
    assert m.run()=='rom' and m.regs[MARKER]==0
    assert m.regs[card.RTC_FLAG]>>8&0xff==(COLD|WARM|UNMARKED)>>8
    cases.append(name+'-cold-unconfirmed-marker-enters-FEL')
    # The next watchdog reset after a cold boot reaches the warm path.
    m=Machine(raw,entry='cold');m.run()
    n=Machine(raw,entry='warm');n.regs.update({card.RTC_FLAG:m.regs[card.RTC_FLAG],MARKER:m.regs[MARKER],WDT_MODE:1})
    assert n.run()=='rom' and n.regs[card.RTC_FLAG]&(COLD|WARM)==COLD|WARM
    cases.append(name+'-cold-then-watchdog-reset-enters-FEL')


def boot_once_cases(name,raw,cases):
    """The runner's boot-once value: one cold path with the marker set, consumed before boot0 runs."""
    for mode in (0x41,0xb1,1):
        m=Machine(raw,entry='warm');m.regs.update({card.RTC_FLAG:BOOT_ONCE,WDT_MODE:mode})
        assert m.run()=='cold' and m.regs[MARKER]==KEYED and m.untouched()
        assert m.regs[card.RTC_FLAG]==STAGE|COLD|ONCE|mode and m.regs[card.RTC_LAST]==IMAGE_CRUMB
        assert (m.regs[WDT_OUTPUT],m.regs[WDT_CONFIG],m.regs[WDT_MODE],m.restarts)==(2,1,0xb1,1)
        writes=[e[1] for e in m.writes()]
        assert writes==[MARKER,card.RTC_FLAG,WDT_MODE,WDT_OUTPUT,WDT_CONFIG,WDT_MODE,WDT_RESTART]
        # WDT0 still reads the mode armed before the reset; the gate disarms it before the arming.
        assert [e[2] for e in m.writes() if e[1]==WDT_MODE]==[0x16aa0000,0x16aa0000|card.GATE_WATCHDOG]
        # The arming the chain table records for boot0 and stage 2 (tools/boot_chain.py) is the one this code makes.
        for stage in (s for s in chain.STAGES if s['name'] in ('boot0','stage2')):
            assert chain.armings([e[1:] for e in m.writes()],mode)==[(stage['arming'],stage['mode'])],(stage['name'],mode)
        # The record is read back after its write and before WDT0 is armed and boot0 entered.
        flag=next(i for i,e in enumerate(m.events) if e[0]=='write' and e[1]==card.RTC_FLAG)
        assert any(e[0]=='read' and e[1]==card.RTC_FLAG for e in m.events[flag:])
        cases.append('%s-boot-once-from-mode-%x-takes-the-cold-path-once'%(name,mode))
        # Consume-once: the next reset, whatever WDT0 mode the run left, finds the cold record and enters FEL.
        for after in (0x41,0xb1,1,0):
            n=Machine(raw,entry='warm');n.regs.update({card.RTC_FLAG:m.regs[card.RTC_FLAG],MARKER:m.regs[MARKER],WDT_MODE:after})
            assert n.run()=='rom' and n.regs[card.RTC_FLAG]>>12&0xf==(COLD|WARM|0)>>12 and not any(e[1]==MARKER for e in n.writes())
            # Stage 2 clears GP2; that reset also enters FEL.
            o=Machine(raw,entry='warm');o.regs.update({card.RTC_FLAG:0,MARKER:KEYED,WDT_MODE:after})
            assert o.run()=='rom'
        cases.append('%s-boot-once-from-mode-%x-consumed-later-resets-enter-FEL'%(name,mode))
    # A GP2 that does not take the cold record keeps the boot-once value: the gate must not run boot0.
    m=Machine(raw,entry='warm',variant='stuck-clear');m.regs.update({card.RTC_FLAG:BOOT_ONCE})
    assert m.run()=='rom' and m.regs[card.RTC_FLAG]==BOOT_ONCE  # FEL, never boot0's stub
    cases.append(name+'-boot-once-unconsumed-enters-FEL')
    # Neighbouring values are not the boot-once value.
    for value in (BOOT_ONCE^1,BOOT_ONCE^0x80000000,STAGE|ONCE):
        m=Machine(raw,entry='warm');m.regs.update({card.RTC_FLAG:value})
        assert m.run()=='rom'
    cases.append(name+'-near-boot-once-values-enter-FEL')
    # A cold power-on with a stale boot-once value: one cold path, recorded as both.
    m=Machine(raw,entry='cold');m.regs.update({card.RTC_FLAG:BOOT_ONCE,WDT_MODE:0})
    assert m.run()=='cold' and m.regs[card.RTC_FLAG]==STAGE|COLD|ONCE
    cases.append(name+'-cold-power-on-with-boot-once')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    info=card.inspect(args.image)
    assert info['profile']['entry']==card.ENTRY_REQUEST
    upload=next(data for address,data in info['uploads']if address==card.GATE)
    length=info['profile']['gate_bytes']
    code=upload[:length]
    assert len(upload)==length+4<=1024 and struct.unpack_from('<I',upload,length)[0]==info['profile']['gate_watchdog']==card.GATE_WATCHDOG
    assert not any(address>=0x40000000 for address,data in info['uploads'])
    # The warm-entry proof image carries the same gate bytes and policy word.
    warm_image=args.image.with_name('a733-card-fel-warm.elf')
    warm=card.inspect(warm_image)
    assert warm['profile']['entry']==card.ENTRY_WARM and warm['profile']['gate_sha256']==info['profile']['gate_sha256']
    assert next(data for address,data in warm['uploads'] if address==card.GATE)==upload
    # Identical code; only the appended WDT0 policy word differs.
    images={0:code+struct.pack('<I',0),card.GATE_WATCHDOG:upload}
    cases=[]
    for policy,raw in images.items():
        name='disarm' if policy==0 else 'rearm-%x'%policy
        cold=request_cases(name,raw,policy,code,cases)
        warm_cases(name,raw,policy,code,cold,cases)
        cold_cases(name,raw,cases)
        boot_once_cases(name,raw,cases)
    # The warm proof as the runner sets it: WDT0 armed at 0x41, marker set.
    m=Machine(upload,entry='warm');m.regs[WDT_MODE]=0x41
    assert m.run()=='rom' and m.regs[WDT_MODE]==0xb1 and m.restarts==1
    assert m.regs[card.RTC_FLAG]==card.expected_flag({'profile':warm['profile'],'watchdog':{'mode':0x41}})
    assert m.regs[card.RTC_LAST]==IMAGE_CRUMB
    cases.append('warm-proof-records-entry-mode-and-rearms')
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
    gate=Machine(upload,sctlr=m.sctlr);assert gate.run()=='rom'
    assert hashlib.sha256(gate.after[:length]).hexdigest()==info['profile']['gate_sha256']
    cases.append('tool-cache-helper-then-fresh-SRAM-exe')
    package=module('direct_gate_package','tools/card-fel-image.py')
    original=(Path.home()/'.cache/sunstoneos/sources/board-boot/boot0_sdcard_orangepizero3w.fex').read_bytes()
    patched=package.first_stage(original,code)
    assert len(patched)==len(original)+1024 and struct.unpack_from('<I',patched,12)[0]==package.checksum(patched)
    restored=bytearray(patched[:len(original)]);restored[:4]=original[:4];restored[12:20]=original[12:20]
    assert restored==original
    sector=patched[len(original):]
    assert len(sector)==1024 and sector[:length]==code and struct.unpack_from('<I',sector,length)[0]==0
    assert not any(sector[length+4:])
    m=Machine(sector,base=0x83000);assert m.run()=='rom' and m.regs[WDT_MODE]==0 and m.restarts==0
    m=Machine(sector,base=0x83000,entry='warm');m.regs[WDT_MODE]=1
    assert m.run()=='rom' and m.regs[WDT_MODE]==0 and m.regs[card.RTC_FLAG]==STAGE|WARM|1
    m=Machine(sector,base=0x83000,entry='cold');assert m.run()=='cold' and m.regs[MARKER]==KEYED
    cases.append('card-package-sectors-at-0x83000-request-warm-and-cold')
    rom=(Path.home()/'.cache/sunstoneos/evidence/rom-copy-20261016-084055/physical-rom.bin').read_bytes()
    assert hashlib.sha256(rom).hexdigest()=='a8e31821e8a1611d75867825914c58373c732742cb8f2235e8168c1d7e93b5aa'
    for length,expected in ((0x3c200,True),(0x3c400,True),(0xa7800,True),(0x3c201,False),(0xa7a00,False)):
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
