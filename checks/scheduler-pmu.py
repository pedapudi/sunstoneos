#!/usr/bin/env python3
"""Execute the scheduler PMU boundary code and reject stores while counting."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import struct

from capstone import Cs,CS_ARCH_ARM64,CS_MODE_ARM
from elftools.elf.elffile import ELFFile
from unicorn import UC_HOOK_CODE,UC_HOOK_MEM_WRITE
from unicorn import arm64_const as R
from harness import machine,environment,LLD

ROOT=Path(__file__).resolve().parents[1]
CHECK_INPUTS=['kernel/scheduler/latency_pmu.c','kernel/scheduler/latency_pmu.h',
              'kernel/scheduler/freertos.c','kernel/scheduler/freertos_control.c',
              'checks/scheduler-pmu-support.c','checks/scheduler-pmu-host']
RETURN,STACK,OUTPUT=0x47fff000,0x47f00000,0x43010000
ENABLE=0x8000003f
EVENTS=[[3,1,0x17,0x2a,0x34,0x35],[3,1,0x17,0x24,0x34,0x35]]

class Model:
    def __init__(self,image,n=6,unsupported=(),mismatch=False,skip_freeze=False,skip_begin_mask=False,
                 zero_cycles=False,zero_events=False):
        with image.open('rb') as f:
            e=ELFFile(f)
            self.parts=[(p['p_vaddr'],p.data()) for p in e.iter_segments() if p['p_type']=='PT_LOAD']
            self.sym={s.name:s['st_value'] for s in e.get_section_by_name('.symtab').iter_symbols() if s['st_shndx']!='SHN_UNDEF'}
            self.sizes={s.name:s['st_size'] for s in e.get_section_by_name('.symtab').iter_symbols() if s['st_shndx']!='SHN_UNDEF'}
        self.uc=machine();self.uc.mem_map(0x40000000,0x08000000)
        for address,data in self.parts:
            if data and not 0x40000000<=address<0x48000000:
                low=address&~4095;high=(address+len(data)+4095)&~4095
                self.uc.mem_map(low,high-low)
            if data:self.uc.mem_write(address,data)
        self.uc.reg_write(R.UC_ARM64_REG_SP,STACK)
        self.core,self.el1,self.clock=0,True,1000
        ceid=0
        for event in set(EVENTS[0]+EVENTS[1])-set(unsupported):ceid|=1<<event
        self.regs={'pmcr_el0':n<<11,'pmceid0_el0':ceid&0xffffffff,'pmceid1_el0':ceid>>32,
                   'pmccfiltr_el0':0,'pmuserenr_el0':0,'pmcntenset_el0':0,'pmintenset_el1':0,
                   'pmovsclr_el0':0,'pmccntr_el0':0,'midr_el1':0x410fd050,'cntfrq_el0':24000000,'daif':0}
        self.mismatch,self.skip_freeze=mismatch,skip_freeze
        self.skip_begin_mask=skip_begin_mask
        self.zero_cycles,self.zero_events=zero_cycles,zero_events
        self.irq_attempts=0;self.admission_violation=None
        self.enabled=False;self.writes=[];self.ops=[];self.cache={};self.violation=None
        self.dis=Cs(CS_ARCH_ARM64,CS_MODE_ARM)
        self.uc.hook_add(UC_HOOK_CODE,self.code)
        self.uc.hook_add(UC_HOOK_MEM_WRITE,self.write)
    def write(self,uc,access,address,size,value,unused):
        self.writes.append((address,size,value,self.enabled))
        if self.enabled:
            self.violation=(uc.reg_read(R.UC_ARM64_REG_PC),address,size)
            uc.emu_stop()
        state=self.sym['ss_sched_pmu_state']+self.core*128
        if address==state+8:
            self.irq_attempts+=1
            if not self.regs['daif']&0x80:
                self.admission_violation=uc.reg_read(R.UC_ARM64_REG_PC)
                uc.emu_stop()
    def code(self,uc,pc,size,unused):
        if pc==RETURN:uc.emu_stop();return
        if pc not in self.cache:self.cache[pc]=next(self.dis.disasm(bytes(uc.mem_read(pc,4)),pc))
        i=self.cache[pc];self.ops.append((pc,i.mnemonic,i.op_str,self.enabled))
        if self.enabled:
            self.regs['pmccntr_el0']+=1
            for counter in range(6):
                name=f'pmevcntr{counter}_el0'
                if not self.zero_events:self.regs[name]=(self.regs.get(name,0)+counter+1)&0xffffffff
        self.clock+=1
        if i.mnemonic=='mrs':
            dst,key=i.op_str.split(', ')
            if key=='tpidrro_el0':value=0x43000000|(self.core<<48)|((1<<56) if self.el1 else 0)
            elif key=='tpidr_el1':
                assert self.el1,'EL0 read privileged core register';value=self.core
            elif key=='cntpct_el0':value=self.clock
            else:
                assert self.el1,('EL0 privileged PMU read',key)
                value=self.regs.get(key)
                assert value is not None,(key,hex(pc))
                if self.mismatch and key=='pmevtyper3_el0':value^=1
                if self.zero_cycles and key=='pmccntr_el0':value=0
            uc.reg_write(getattr(R,'UC_ARM64_REG_'+dst.upper()),value)
            uc.reg_write(R.UC_ARM64_REG_PC,pc+4)
        elif i.mnemonic=='msr':
            assert self.el1,'EL0 privileged write'
            key,src=i.op_str.split(', ')
            value=int(src[1:],0) if src.startswith('#') else (0 if src=='xzr' else uc.reg_read(getattr(R,'UC_ARM64_REG_'+src.upper())))
            if key=='pmcntenset_el0':self.regs[key]|=value;self.enabled=bool(self.regs[key]&ENABLE)
            elif key=='pmcntenclr_el0':
                if not self.skip_freeze:self.regs['pmcntenset_el0']&=~value;self.enabled=bool(self.regs['pmcntenset_el0']&ENABLE)
            elif key=='pmintenclr_el1':self.regs['pmintenset_el1']&=~value
            elif key=='pmovsclr_el0':self.regs[key]&=~value
            elif key.lower()=='daifset':
                entry=self.sym['ss_sched_pmu_begin']
                if not(self.skip_begin_mask and entry<=pc<entry+self.sizes['ss_sched_pmu_begin']):
                    self.regs['daif']|=value<<6
            else:self.regs[key]=value
            uc.reg_write(R.UC_ARM64_REG_PC,pc+4)
    def call(self,name,*args,allow_violation=False):
        for index,value in enumerate(args):self.uc.reg_write(R.UC_ARM64_REG_X0+index,value&((1<<64)-1))
        self.uc.reg_write(R.UC_ARM64_REG_LR,RETURN)
        self.uc.emu_start(self.sym[name],RETURN,count=250000)
        if not allow_violation:assert self.violation is None,('recorder store while enabled',self.violation)
        value=self.uc.reg_read(R.UC_ARM64_REG_X0)
        return value-(1<<64) if value>>63 else value
    def setup(self,kind=0):
        self.call('ss_sched_pmu_arm');assert self.call('ss_sched_pmu_prepare',self.core,kind)==0
    def row(self,kind,bank,core=0):
        address=self.sym['report']+64+core*1456+96+(kind*2+bank)*136
        return struct.unpack('<17Q',self.uc.mem_read(address,136))
    def assert_refusal(self,name,args,status):
        before=len(self.writes);enabled=self.enabled
        registers={k:v for k,v in self.regs.items() if not k.startswith('pmevcntr') and k!='pmccntr_el0'}
        assert self.call(name,*args)==status
        after={k:v for k,v in self.regs.items() if not k.startswith('pmevcntr') and k!='pmccntr_el0'}
        assert len(self.writes)==before and self.enabled==enabled and after==registers

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    output=a.output.resolve();output.mkdir(parents=True,exist_ok=True)
    flags=['/usr/bin/clang-21','--target=aarch64-none-elf','-march=armv8.1-a+lse','-mgeneral-regs-only',
           '-ffreestanding','-fno-builtin','-fno-stack-protector','-O2','-g','-Wall','-Wextra','-Werror',
           '-DSUNSTONE_SCHED_PMU=1','-DSUNSTONE_LATENCY=1','-DSUNSTONE_SCHED_LATENCY_TRACE=1',
           '-I'+str(ROOT/'checks/scheduler-pmu-host'),'-I'+str(ROOT/'kernel/scheduler')]
    objects=[]
    for name in ['kernel/scheduler/latency_pmu.c','checks/scheduler-pmu-support.c']:
        obj=output/(Path(name).stem+'.o');objects.append(obj)
        r=subprocess.run(flags+['-c',str(ROOT/name),'-o',str(obj)],text=True,capture_output=True,env=environment())
        (output/(obj.stem+'.log')).write_text(r.stdout+r.stderr);assert r.returncode==0,r.stderr
    image=output/'pmu.elf'
    subprocess.run([str(LLD),'-Ttext=0x41000000','-Tdata=0x41100000','-Tbss=0x41200000',
                    '-e','ss_sched_pmu_arm',*map(str,objects),'-o',str(image)],check=True,env=environment())
    results=[]
    m=Model(image);m.setup();assert m.row(4,0)[0]==1 and not m.enabled
    cookie=m.call('ss_sched_pmu_begin',0,0,6,1);assert cookie>0 and m.enabled
    m.assert_refusal('ss_sched_pmu_begin',(0,0,6,1),-4)
    m.assert_refusal('ss_sched_pmu_end',(0,-4),6)
    m.assert_refusal('ss_sched_pmu_end',(0,cookie+1),6)
    m.assert_refusal('ss_sched_pmu_prepare',(0,0),4)
    m.assert_refusal('ss_sched_pmu_end',(1,cookie),3)
    assert m.call('ss_sched_pmu_end',0,cookie)==0 and not m.enabled
    row=m.row(0,0);assert row[0]==1 and row[2]==0 and row[5]>0 and row[12]==63 and row[15]==7
    assert all(row[6:12]);results.append('accepted, nested, same-kind rejected child, wrong cookie and wrong core')
    m=Model(image);m.setup();parent,child=0x43000040,0x43000041
    cookie=m.call('ss_sched_pmu_begin_scope',0,0,6,1,parent);assert cookie>0 and m.enabled
    m.assert_refusal('ss_sched_pmu_begin_scope',(0,0,6,1,child),-4)
    m.assert_refusal('ss_sched_pmu_end_scope',(0,child),6)
    m.assert_refusal('ss_sched_pmu_end_scope',(1,parent),3)
    assert m.call('ss_sched_pmu_end_scope',0,parent)==0 and not m.enabled
    results.append('same-kind rejected child scope cannot stop its parent')
    m=Model(image);m.el1=False
    m.assert_refusal('ss_sched_pmu_begin',(0,0,0,0),-2)
    m.assert_refusal('ss_sched_pmu_prepare',(0,0),2)
    assert all(not (op[1]=='mrs' and op[2].split(', ')[1]!='tpidrro_el0') for op in m.ops)
    results.append('EL0 entry reads no privileged system register')
    m=Model(image);m.setup(3)
    for bank in [0,1]:
        assert m.call('ss_sched_pmu_prepare',0,3)==0
        cookie=m.call('ss_sched_pmu_begin',0,3,0xffffffff,0)
        assert cookie>0 and m.call('ss_sched_pmu_end',0,cookie)==0
        assert m.row(3,bank)[0]==1 and m.row(4,bank)[0]==1
    results.append('tick banks and separate raw blank windows')
    m=Model(image,unsupported=[0x2a]);m.setup()
    cookie=m.call('ss_sched_pmu_begin',0,0,6,0);assert m.call('ss_sched_pmu_end',0,cookie)==0
    row=m.row(0,0);assert row[12]==55 and row[9]==0
    results.append('unsupported event is flagged and its sample word is zero')
    m=Model(image);m.setup();cookie=m.call('ss_sched_pmu_begin',0,2,4,0)
    assert cookie==-9
    assert m.call('ss_sched_pmu_prepare',0,2)==0
    cookie=m.call('ss_sched_pmu_begin',0,2,4,0)
    m.regs['pmovsclr_el0']=0x80000004
    assert m.call('ss_sched_pmu_end',0,cookie)==11 and m.row(2,0)[13]==0x80000004
    results.append('raw event and cycle overflow retained as unusable')
    for n,status in [(0,7),(5,7)]:
        m=Model(image,n=n);m.call('ss_sched_pmu_arm');assert m.call('ss_sched_pmu_prepare',0,0)==status
    m=Model(image,mismatch=True);m.call('ss_sched_pmu_arm');assert m.call('ss_sched_pmu_prepare',0,0)==8
    results.append('insufficient counters and readback mismatch refuse capture')
    m=Model(image,zero_cycles=True);m.call('ss_sched_pmu_arm');assert m.call('ss_sched_pmu_prepare',0,0)==8
    m=Model(image,zero_events=True);m.setup();cookie=m.call('ss_sched_pmu_begin',0,0,6,0)
    assert m.call('ss_sched_pmu_end',0,cookie)==0 and not any(m.row(0,0)[6:12])
    results.append('zero cycles refuse capture while zero refill counts remain valid')
    m=Model(image);m.setup();m.call('ss_sched_pmu_snapshot',OUTPUT)
    assert struct.unpack('<8Q',m.uc.mem_read(OUTPUT,64))[7]==14
    m=Model(image);m.setup();m.el1=False
    for core in range(8):m.core=core;assert m.call('ss_sched_pmu_freeze',core)==0
    m.call('ss_sched_pmu_snapshot',OUTPUT)
    words=struct.unpack('<1464Q',m.uc.mem_read(OUTPUT,11712));assert words[7]==6
    completed=words[8+9]
    assert completed==sum(words[8+12+k*17] for k in range(10))
    results.append('snapshot requires all freeze acknowledgments and completed row accounting')
    for sequence in [(1<<63)-1,(1<<64)-1]:
        m=Model(image);m.setup();m.uc.mem_write(m.sym['ss_sched_pmu_state']+8,struct.pack('<Q',sequence))
        assert m.call('ss_sched_pmu_begin',0,0,6,0)==-8
        assert m.call('ss_sched_pmu_begin',0,0,6,0)==-8
    results.append('cookie exhaustion latches refusal before reuse')
    m=Model(image);m.setup();cookie=m.call('ss_sched_pmu_begin',0,0,6,0)
    assert m.irq_attempts and m.admission_violation is None and not m.regs['daif']&0x80
    assert m.call('ss_sched_pmu_end',0,cookie)==0
    m=Model(image,skip_begin_mask=True);m.setup();m.call('ss_sched_pmu_begin',0,0,6,0)
    assert m.admission_violation,'IRQ admission negative must reach an unreserved metadata write'
    results.append('IRQ attempt before cookie publication is excluded; missing mask fails its negative')
    m=Model(image);m.setup();cookie=m.call('ss_sched_pmu_begin',0,0,6,0)
    m.skip_freeze=True;m.call('ss_sched_pmu_end',0,cookie,allow_violation=True)
    assert m.violation,'negative control must detect post-boundary record stores'
    results.append('omitted end freeze fails the recorder-store negative control')
    report={'passed':True,'cases':results,'image_sha256':hashlib.sha256(image.read_bytes()).hexdigest(),
            'structural_failure_pc':hex(m.violation[0]),'physical_access':False}
    (output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))

if __name__=='__main__':main()
