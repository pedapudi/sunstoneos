#!/usr/bin/env python3
"""Execute cycle-counter ownership, nested cause hooks and benchmark boundaries."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
from capstone import Cs,CS_ARCH_ARM64,CS_MODE_ARM
from elftools.elf.elffile import ELFFile
from unicorn import UC_HOOK_CODE,UC_HOOK_MEM_WRITE
from unicorn import arm64_const as R
from harness import machine,environment,LLD
ROOT=Path(__file__).resolve().parents[1]
CHECK_INPUTS=['kernel/scheduler/latency_cause.c','kernel/scheduler/latency_cause.h',
              'kernel/scheduler/freertos.c','kernel/scheduler/freertos_control.c',
              'kernel/scheduler/owner.c','runtime/latency_irq.c',
              'checks/scheduler-cause-host','checks/scheduler-pmu-support.c']
RETURN,STACK,OUTPUT,SCRATCH=0x47fff000,0x47f00000,0x43010000,0x43000000
CC=1<<31
class Model:
    def __init__(self,image,skip_mask=False,mismatch=False):
        with image.open('rb') as f:
            e=ELFFile(f);self.parts=[(p['p_vaddr'],p.data()) for p in e.iter_segments() if p['p_type']=='PT_LOAD']
            self.sym={s.name:s['st_value'] for s in e.get_section_by_name('.symtab').iter_symbols() if s['st_shndx']!='SHN_UNDEF'}
            self.size={s.name:s['st_size'] for s in e.get_section_by_name('.symtab').iter_symbols() if s['st_shndx']!='SHN_UNDEF'}
        self.uc=machine();self.uc.mem_map(0x40000000,0x08000000)
        for p,data in self.parts:
            if data and not 0x40000000<=p<0x48000000:
                low=p&~4095;high=(p+len(data)+4095)&~4095;self.uc.mem_map(low,high-low)
            if data:self.uc.mem_write(p,data)
        self.uc.reg_write(R.UC_ARM64_REG_SP,STACK)
        self.core=0;self.el1=True;self.clock=1000;self.skip_mask=skip_mask;self.mismatch=mismatch
        self.regs={'pmcr_el0':6<<11,'pmccntr_el0':0,'pmcntenset_el0':0,'pmintenset_el1':0,
                   'pmuserenr_el0':0,'pmccfiltr_el0':0,'pmovsclr_el0':0,'daif':0,
                   'midr_el1':0x410fd050,'mpidr_el1':0x80000000,'icc_ctlr_el1':0x40,'icc_pmr_el1':0xf0}
        self.cache={};self.ops=[];self.writes=[];self.measuring=False;self.samples=[];self.violation=None
        self.dis=Cs(CS_ARCH_ARM64,CS_MODE_ARM)
        self.uc.hook_add(UC_HOOK_CODE,self.code);self.uc.hook_add(UC_HOOK_MEM_WRITE,self.write)
    def readq(self,address,n=1):return struct.unpack('<'+'Q'*n,self.uc.mem_read(address,n*8))
    def row(self,core):return self.readq(self.sym['report']+64+core*144,18)
    def write(self,uc,access,address,size,value,unused):
        self.writes.append((uc.reg_read(R.UC_ARM64_REG_PC),address,size,value,self.measuring))
        if self.measuring and address not in [SCRATCH,self.sym['bench_store']]:
            self.violation=('recorder store between benchmark timestamps',hex(address));uc.emu_stop()
    def code(self,uc,pc,size,unused):
        if pc==RETURN:uc.emu_stop();return
        if pc not in self.cache:self.cache[pc]=next(self.dis.disasm(bytes(uc.mem_read(pc,4)),pc))
        i=self.cache[pc];self.ops.append((pc,i.mnemonic,i.op_str));self.clock+=1
        if self.regs['pmcntenset_el0']&CC:self.regs['pmccntr_el0']+=1
        leaf=self.sym['ss_latency_cause_bench_leaf']<=pc<self.sym['ss_latency_cause_bench_leaf']+self.size['ss_latency_cause_bench_leaf']
        if i.mnemonic=='mrs':
            dst,key=i.op_str.split(', ')
            if key=='tpidrro_el0':value=0x43000000|(self.core<<48)|((1<<56) if self.el1 else 0)
            elif key=='tpidr_el1':assert self.el1;value=self.core
            elif key=='cntpct_el0':value=self.clock
            elif key=='pmccntr_el0':
                assert self.el1 or self.regs['pmuserenr_el0']&4
                value=self.regs[key]
                if leaf:
                    if not self.regs['daif']&0x80:self.violation=('benchmark admits IRQ',hex(pc))
                    self.measuring=not self.measuring;self.samples.append((pc,self.measuring))
            else:
                assert self.el1,('privileged read at EL0',key)
                value=self.regs.get(key);assert value is not None,(key,hex(pc))
                if self.mismatch and key=='pmccfiltr_el0':value=1
                if key=='mpidr_el1':value|=self.core<<8
            uc.reg_write(getattr(R,'UC_ARM64_REG_'+dst.upper()),value);uc.reg_write(R.UC_ARM64_REG_PC,pc+4)
        elif i.mnemonic=='msr':
            assert self.el1
            key,src=i.op_str.split(', ')
            value=int(src[1:],0) if src.startswith('#') else (0 if src=='xzr' else uc.reg_read(getattr(R,'UC_ARM64_REG_'+src.upper())))
            if key=='pmcntenset_el0':self.regs[key]|=value
            elif key=='pmcntenclr_el0':self.regs['pmcntenset_el0']&=~value
            elif key=='pmintenclr_el1':self.regs['pmintenset_el1']&=~value
            elif key=='pmovsclr_el0':self.regs[key]&=~value
            elif key.lower()=='daifset':
                if not(self.skip_mask and leaf):self.regs['daif']|=value<<6
            elif key=='icc_sgi1r_el1':pass
            else:self.regs[key]=value
            uc.reg_write(R.UC_ARM64_REG_PC,pc+4)
    def call(self,name,*args,allow_violation=False):
        for n,v in enumerate(args):self.uc.reg_write(R.UC_ARM64_REG_X0+n,v&((1<<64)-1))
        self.uc.reg_write(R.UC_ARM64_REG_LR,RETURN)
        self.uc.emu_start(self.sym[name],RETURN,count=1000000)
        if not allow_violation:assert self.violation is None,self.violation
        return self.uc.reg_read(R.UC_ARM64_REG_X0)
    def setup(self):
        self.call('ss_latency_cause_arm');self.call('ss_latency_cause_irq_begin',0,30)
        assert self.call('ss_latency_cause_core_ready',0)==1

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
    flags=['/usr/bin/clang-21','--target=aarch64-none-elf','-march=armv8.1-a+lse','-mgeneral-regs-only',
           '-ffreestanding','-fno-builtin','-fno-stack-protector','-O2','-g','-Wall','-Wextra','-Werror',
           '-DSUNSTONE_LATENCY_CAUSE=1','-DSUNSTONE_LATENCY=1','-DSUNSTONE_SCHED_LATENCY_TRACE=1',
           '-I'+str(ROOT/'checks/scheduler-cause-host'),'-I'+str(ROOT/'kernel/scheduler')]
    objects=[]
    for name in ('kernel/scheduler/latency_cause.c','checks/scheduler-pmu-support.c'):
        obj=out/(Path(name).stem+'.o');objects.append(obj)
        run=subprocess.run(flags+['-c',str(ROOT/name),'-o',str(obj)],text=True,capture_output=True,env=environment())
        (out/(obj.stem+'.log')).write_text(run.stdout+run.stderr);assert not run.returncode,run.stderr
    image=out/'cause.elf'
    subprocess.run([str(LLD),'-Ttext=0x41000000','-Tdata=0x41100000','-Tbss=0x41200000','-e','ss_latency_cause_arm',*map(str,objects),'-o',str(image)],check=True,env=environment())
    results=[]
    m=Model(image);m.call('ss_latency_cause_irq_begin',0,30);assert not m.call('ss_latency_cause_core_ready',0)
    m.call('ss_latency_cause_arm');m.el1=False;before=len(m.ops);m.call('ss_latency_cause_irq_begin',0,30)
    assert not any(i[1]=='mrs' and i[2].split(', ')[1] not in ['tpidrro_el0'] for i in m.ops[before:])
    m.el1=True;m.call('ss_latency_cause_irq_begin',1,30);assert not m.call('ss_latency_cause_core_ready',1)
    results.append('unarmed, EL0 and wrong-core IRQs cannot configure a PMU')
    m=Model(image);m.setup();assert m.row(0)[3]&0x4f==0x41 and m.row(0)[4]==4 and m.row(0)[6]==CC
    cc=m.regs['pmccntr_el0'];m.call('ss_latency_cause_irq_end',0);m.call('ss_latency_cause_irq_begin',0,30)
    assert m.regs['pmccntr_el0']>cc
    m=Model(image,mismatch=True)
    m.call('ss_latency_cause_arm');m.call('ss_latency_cause_irq_begin',0,30)
    assert not m.call('ss_latency_cause_core_ready',0) and m.row(0)[0]==4
    results.append('CR-only EL0 access, 64-bit full-rate free-running CC and mismatch rejection')
    m=Model(image);m.setup();m.call('ss_latency_cause_tick_begin');m.call('ss_latency_cause_release_begin',2,0)
    m.call('ss_latency_cause_sgi_begin',2);m.call('ss_latency_cause_sgi_begin',2);m.call('ss_latency_cause_sgi_sent',2)
    m.call('ss_latency_cause_sgi_end',2);m.call('ss_latency_cause_sgi_end',2)
    m.call('ss_latency_cause_mask_tail_begin');m.call('ss_latency_cause_mask_tail_end');m.call('ss_latency_cause_release_end',2)
    m.call('ss_latency_cause_release_begin',3,0);m.call('ss_latency_cause_irq_begin',0,0)
    m.call('ss_latency_cause_release_begin',3,0);m.call('ss_latency_cause_release_end',3)
    m.call('ss_latency_cause_sgi_begin',4);m.call('ss_latency_cause_sgi_sent',4);m.call('ss_latency_cause_sgi_end',4)
    m.call('ss_latency_cause_irq_end',0);m.call('ss_latency_cause_release_end',3)
    for target in range(8):
        if target not in [2,3]:
            m.call('ss_latency_cause_release_begin',target,0);m.call('ss_latency_cause_release_end',target)
        m.call('ss_latency_cause_state_begin',target);m.call('ss_latency_cause_state_end',target,1,10,20,1)
    m.call('ss_latency_cause_tick_end')
    tick=m.readq(m.sym['report']+5056,8);assert tick[5]==255 and tick[6]==255 and tick[7]==1,tick
    assert m.readq(m.sym['report']+1216+2*32,4)[0]==1
    assert m.readq(m.sym['report']+1216+4*32,4)[0]==1
    assert m.readq(m.sym['report']+3264+2*112,14)[12]>0
    previous_tick=m.readq(m.sym['report']+5056,120)
    m.call('ss_latency_cause_tick_begin');m.call('ss_latency_cause_tick_end')
    assert m.readq(m.sym['report']+5056,120)==previous_tick
    results.append('nested same-target child cannot close parent; SGIs and mask tail separated; partial tick cannot replace all-eight maximum')
    m=Model(image);m.setup();boundaries=[]
    for case in range(15):
        start=len(m.ops);m.samples=[];m.writes=[]
        value=m.call('ss_latency_cause_bench_leaf',case,SCRATCH,0x10001,0xf0)
        assert value>0 and len(m.samples)==2 and not m.measuring and not m.regs['daif']&0x80
        body=[x for x in m.ops[start:] if m.samples[0][0]<x[0]<m.samples[1][0]]
        stores=[x for x in m.writes if x[4]]
        assert len(stores)==(1 if case in [4,5,6] else 0)
        if case in [7,8,9,11,12,13]:assert any(x[1]=='msr' and x[2].startswith('icc_sgi1r') for x in body)
        if case in [8,9]:
            sgi=next(n for n,x in enumerate(body) if x[1]=='msr');dsb=next(n for n,x in enumerate(body) if x[1]=='dsb')
            assert not any(x[1]=='isb' for x in body[sgi+1:dsb])
        if case in [12,13]:
            sgi=next(n for n,x in enumerate(body) if x[1]=='msr');dsb=next(n for n,x in enumerate(body) if x[1]=='dsb')
            assert any(x[1]=='isb' for x in body[sgi+1:dsb])
        boundaries.append({'case':case,'begin_pc':hex(m.samples[0][0]),'end_pc':hex(m.samples[1][0]),'cycles_model':value,'stores_in_body':len(stores)})
    results.append('all 15 leaf bodies retain register timestamps; only declared Normal-store arms write between them')
    bad=Model(image,skip_mask=True);bad.setup();bad.call('ss_latency_cause_bench_leaf',8,SCRATCH,0x10001,0xf0,allow_violation=True)
    assert bad.violation and bad.violation[0]=='benchmark admits IRQ'
    results.append('omitting benchmark DAIF mask fails forced IRQ negative')
    m=Model(image);m.setup();m.call('ss_latency_cause_bench_request');wait_tested=False
    bench=m.sym['report']+6016
    for iteration in range(970):
        if m.call('ss_latency_cause_bench_done'):break
        m.call('ss_latency_cause_tick_begin');m.call('ss_latency_cause_tick_end')
        prefix=m.readq(bench,11)
        if prefix[6]:
            if not wait_tested:
                before=m.readq(bench+88,15)
                m.call('ss_latency_cause_tick_begin');m.call('ss_latency_cause_tick_end')
                assert m.readq(bench+88,15)==before and m.readq(bench,11)[5]>0
                wait_tested=True
            ack=m.sym['report']+64+144+13*8
            m.uc.mem_write(ack,struct.pack('<Q',m.readq(ack)[0]+1))
    assert m.call('ss_latency_cause_bench_done') and wait_tested
    prefix=m.readq(bench,11)
    assert prefix[3:5]==(384,384) and prefix[6]==0 and prefix[7]==64 and 0<prefix[8]<=prefix[9]<=prefix[10],prefix
    assert m.readq(bench+88,15)==(64,)*15
    results.append('960 quiet samples, 384 SGI acknowledgments, no sample while pending, and real empty C-hook calibration')
    m=Model(image);m.setup();m.el1=False
    assert not m.call('ss_latency_cause_snapshot',OUTPUT,13904)
    assert m.readq(OUTPUT,8)[7]==2
    m.call('ss_latency_cause_freeze',1);assert m.call('ss_latency_cause_core_ready',0)
    for c in range(8):m.core=c;m.call('ss_latency_cause_freeze',c)
    assert m.call('ss_latency_cause_snapshot',OUTPUT,13904)==1
    assert m.readq(OUTPUT,8)[:7]==(0x5353434155534531,1,13904,8,2,15,64)
    results.append('EL0-safe own-core freeze, wrong-core rejection and all-core snapshot acknowledgments')
    r={'passed':True,'cases':results,'leaf_boundaries':boundaries,'image':str(image),'image_sha256':hashlib.sha256(image.read_bytes()).hexdigest(),'physical_access':False}
    (out/'report.json').write_text(json.dumps(r,indent=2)+'\n');print(json.dumps(r,indent=2))
if __name__=='__main__':main()
