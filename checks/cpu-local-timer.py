#!/usr/bin/env python3
"""Execute the private timer hook, SVC106 restoration and real IRQ frames.

Reuse the cached CPU instruction model; peripherals and time remain modeled.
The scheduler and physical delivery are outside this focused check's scope.
"""
import argparse
import hashlib
import importlib.util
import json
import struct
from pathlib import Path
from unicorn import UC_MEM_WRITE
from unicorn.arm64_const import (UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_LR,
    UC_ARM64_REG_Q0, UC_ARM64_REG_FPCR, UC_ARM64_REG_FPSR, UC_ARM64_REG_NZCV)

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('cached_cpu',ROOT/'checks/a733-smp-cached.py')
cached=importlib.util.module_from_spec(spec);spec.loader.exec_module(cached)
SENTINEL=0x41fff000
DEADLINE=24000000

class Timer(cached.Machine):
    def __init__(self,image,core):
        super().__init__(image)
        self.core=core;self.stop=None;self.events=[];self.expected=None
        self.secure_pmr=0xff;self.drop=None;self.expire_on_enable=True
        self.tick_calls=0;self.mmio_writes=[];self.timer_reads=0
        self.executed=0;self.irq_begin=None;self.arm_begin=None;self.arm_return=None;self.arm_instructions=None;self.after_disable=[]
        r=self.regs[core];u=self.cpus[core]
        r.update(currentel=4,spsel=1,daif=0x3c0,tpidr_el1=core,
            vbar_el1=self.symbols['a733_cached_el1_vectors'],icc_pmr_el1=0xf0,
            icc_igrpen1_el1=1,sp_el1=0x41f80000,scr_el3=0x531)
        u.reg_write(UC_ARM64_REG_SP,r['sp_el1'])
        for offset in (0x10200,0x1041e):self.mmio[self.rdist[core]+offset]=0
        self.mmio[self.rdist[core]+0x10100]=1
        self.putq(self.symbols['a733_cached_runtime']+core*128,7,DEADLINE,480000000,self.rdist[core])
        self.putq(self.symbols['diagnostic_deadline'],480000000)
        self.putq(self.symbols['xTickCount'],123)
        self.putq(self.symbols['next_tick'],456)
        self.putq(self.symbols['a733_smp_core_results']+32,32)

    def putq(self,address,*values):self.cpus[self.core].mem_write(address,struct.pack('<'+'Q'*len(values),*values))
    def snapshot(self):
        u=self.cpus[self.core]
        return [u.reg_read(x) for x in cached.GPR]+[u.reg_read(UC_ARM64_REG_SP)]+[
            u.reg_read(UC_ARM64_REG_Q0+i) for i in range(32)]+[
            u.reg_read(x) for x in (UC_ARM64_REG_FPCR,UC_ARM64_REG_FPSR,UC_ARM64_REG_NZCV)]
    def memory(self,u,access,address,size,value,c):
        if address<0x41000000:
            if access==UC_MEM_WRITE:
                self.mmio_writes.append((address,size,value))
                if address==self.rdist[c]+0x1041e:
                    value&=0xf0
                    if self.drop=='priority':return
                if address==self.rdist[c]+0x10100 and self.drop=='enable':return
            return super().memory(u,access,address,size,value,c)
        return super().memory(u,access,address,size,value,c)

    def exception(self,c,level,pc,esr=0,irq=False):
        if irq:self.irq_begin=self.executed
        return super().exception(c,level,pc,esr,irq)

    def instruction(self,u,a,size,c):
        r=self.regs[c]
        ins=next(self.decoder.disasm(bytes(u.mem_read(a,4)),a));args=ins.op_str.split(', ')
        if r['cntp_ctl_el0']&3==1 and self.clock>=r['cntp_cval_el0']:
            self.pending[c].add(30)
        if a==self.symbols['a733_smp_exit_privileged']:
            self.stop='retained';self.events.append(('retained',self.reg(c,'x0'),self.reg(c,'x1')));u.emu_stop();return
        if a==self.symbols['FreeRTOS_Tick_Handler']:self.tick_calls+=1
        if a==SENTINEL:
            if self.expected is None:
                self.expected=self.snapshot();self.events.append(('restored-task',r['daif']))
            if not (self.pending[c] and r['icc_pmr_el1']>0xe0 and not r['daif']&128):
                self.stop='task';u.emu_stop();return
        deliver=(r['currentel']<=4 and not r['daif']&128 and r['icc_igrpen1_el1']&1 and
                 r['icc_pmr_el1']>0xe0 and self.pending[c] and self.irq_active[c] is None)
        if not deliver:self.executed+=1
        if a==self.symbols['a733_qualification_timer_arm']:self.arm_begin=self.executed-1
        if a==self.arm_return:self.arm_instructions=self.executed-1-self.arm_begin
        if self.events and self.events[-1]==('timer-control',0):self.after_disable.append(ins.mnemonic)
        if ins.mnemonic=='mrs' and args[1]=='cntpct_el0':self.clock+=24
        if ins.mnemonic=='mrs' and args[1]=='cntp_ctl_el0':
            self.timer_reads+=1
            v=r['cntp_ctl_el0']
            if v&1 and self.clock>=r['cntp_cval_el0']:v|=4
            self.put(c,args[0],v);u.reg_write(UC_ARM64_REG_PC,a+4);return
        if ins.mnemonic=='msr' and args[0]=='cntp_ctl_el0':
            v=self.reg(c,args[1]);self.events.append(('timer-control',v))
            if not v&1:self.pending[c].discard(30);self.after_disable=[]
            if v&1:
                assert struct.unpack('<I',self.read(self.symbols['timer_state']+c*4,4))[0]==1
                assert r['daif']&128
                if self.expire_on_enable:self.clock=r['cntp_cval_el0']
        if ins.mnemonic=='msr' and args[0]=='icc_pmr_el1':
            if self.secure_pmr>=128:r['icc_pmr_el1']=self.reg(c,args[1])&0xf0
            else:r['icc_pmr_el1']=0
            u.reg_write(UC_ARM64_REG_PC,a+4);return
        if ins.mnemonic=='msr' and args[0]=='icc_eoir1_el1':
            assert not r['cntp_ctl_el0']&1
            assert self.events[-1]==('timer-control',0)
            assert 'dsb' in self.after_disable and 'isb' in self.after_disable
            assert self.after_disable.index('dsb')<self.after_disable.index('isb')
            self.events.append(('eoi',self.reg(c,args[1])))
        if ins.mnemonic=='svc' and int(args[0][1:],0)==106:
            assert r['daif']&128 and self.irq_active[c] is None
            assert 30 in self.pending[c]
            self.events.append(('svc106-pending-masked',c))
        super().instruction(u,a,size,c)

    def execute(self,address,budget=10000):
        u=self.cpus[self.core];self.stop=None
        u.reg_write(UC_ARM64_REG_PC,address)
        u.emu_start(address,0x42200000,count=budget)
        assert self.stop,('instruction budget',hex(u.reg_read(UC_ARM64_REG_PC)))
        return self.stop

    def frame(self):
        c=self.core;u=self.cpus[c]
        # Use the actual port initializer, then add a saved FP context if the
        # port's initial task convention omits it. Restore all through SVC106.
        for i,v in enumerate((0x41f40000,SENTINEL,0x12340000+c)):self.put(c,'x'+str(i),v)
        u.reg_write(UC_ARM64_REG_LR,SENTINEL)
        assert self.execute(self.symbols['pxPortInitialiseStack'])=='task'
        self.expected=None;self.events.clear()
        frame=self.reg(c,'x0')
        if self.q(frame)==0:
            frame-=528;self.putq(frame,1,0)
        assert self.q(frame)==1
        u.mem_write(frame+16,struct.pack('<II',0x15,0x800000)+bytes(8))
        qvalues=[((0x12340000+c+i)<<96)|(0x56789000+i) for i in range(32)]
        for pair in range(16):
            start=30-pair*2
            u.mem_write(frame+32+pair*32,b''.join(qvalues[i].to_bytes(16,'little') for i in (start,start+1)))
        self.putq(self.symbols['pxCurrentTCBs']+c*8,0x41f60000)
        self.putq(0x41f60000,frame)
        self.regs[c]['sp_el0']=0x41f40000
        self.put(c,'x19',c);self.put(c,'x0',c)
        # Resolve the actual compiled continuation BL, not a synthetic SVC.
        lo=self.symbols['a733_cached_el1'];target=self.symbols['a733_qualification_timer_arm']
        calls=[i.address for i in self.decoder.disasm(self.read(lo,1024),lo)
               if i.mnemonic=='bl' and int(i.op_str.lstrip('#'),0)==target]
        assert len(calls)==1
        self.arm_return=calls[0]+4
        return calls[0],qvalues

def main():
    ap=argparse.ArgumentParser();ap.add_argument('elf',type=Path);ap.add_argument('--out',type=Path,required=True)
    args=ap.parse_args();args.out.mkdir(parents=True,exist_ok=True)
    rows=[]
    for c in range(1,8):
        m=Timer(args.elf,c);start,qvalues=m.frame()
        assert m.execute(start)=='task'
        assert m.expected==m.snapshot(),('IRQ frame changed',c)
        assert [m.cpus[c].reg_read(UC_ARM64_REG_Q0+i) for i in range(32)]==qvalues
        assert m.q(m.symbols['a733_smp_core_results']+c*128+32)==1
        assert m.regs[c]['tpidr_el1']==c and m.irq_active[c] is None
        assert m.q(m.symbols['xTickCount'])==123 and m.q(m.symbols['next_tick'])==456
        assert m.q(m.symbols['a733_smp_core_results']+32)==32 and m.tick_calls==0
        assert m.events==[('timer-control',1),('svc106-pending-masked',c),('restored-task',0),('timer-control',0),('eoi',30)]
        assert m.mmio[m.rdist[c]+0x10100]==0x40000001
        assert len(m.mmio_writes)==2 and m.timer_reads==2
        rows.append(dict(case='core-'+str(c),arm_instructions=m.arm_instructions,irq_instructions=m.executed-m.irq_begin,events=m.events,mmio_writes=m.mmio_writes))
    for case in ('unmasked-arm','enabled-timer','pending-ppi','active-ppi','wrong-trigger',
                 'deadline','rwp-deadline','drop-enable','drop-priority','unarmed-irq','duplicate-irq','not-fired-irq','masked-irq','already-counted-irq','secure-pmr'):
        m=Timer(args.elf,6);start,_=m.frame();r=m.regs[6];g=m.rdist[6]
        if case=='unmasked-arm':r['daif']=0
        elif case=='enabled-timer':r['cntp_ctl_el0']=1
        elif case=='pending-ppi':m.mmio[g+0x10200]=1<<30
        elif case=='active-ppi':m.mmio[g+0x10300]=1<<30
        elif case=='wrong-trigger':m.mmio[g+0x10c04]=2<<28
        elif case=='deadline':m.clock=DEADLINE
        elif case=='rwp-deadline':m.mmio[g]=8;m.clock=DEADLINE-240
        elif case.startswith('drop-'):m.drop=case[5:]
        elif case=='secure-pmr':m.secure_pmr=0x70
        elif case.endswith('-irq'):
            assert m.execute(start)=='task'
            m.expected=None;m.events.clear();m.pending[6].add(30)
            r['cntp_ctl_el0']=1;r['cntp_cval_el0']=m.clock
            if case!='duplicate-irq':
                m.cpus[6].mem_write(m.symbols['timer_state']+24,struct.pack('<I',0 if case=='unarmed-irq' else 1))
                m.putq(m.symbols['a733_smp_core_results']+6*128+32,1 if case=='already-counted-irq' else 0)
            if case=='not-fired-irq':r['cntp_cval_el0']=m.clock+24000
            elif case=='masked-irq':r['cntp_ctl_el0']=3
            start=SENTINEL
        outcome=m.execute(start)
        if case=='secure-pmr':
            assert outcome=='task' and m.q(m.symbols['a733_smp_core_results']+6*128+32)==0
            assert not any(x[0]=='eoi' for x in m.events)
        else:assert outcome=='retained',case
        rows.append(dict(case=case,outcome=outcome,events=m.events))
    sources=[Path(__file__),ROOT/'checks/a733-smp-cached.py']
    result=dict(scope='actual linked arm, SVC106, IRQ frame and callback with modeled timer/GIC; no physical qualification',
        elf_sha256=hashlib.sha256(args.elf.read_bytes()).hexdigest(),cases=rows,
        sources={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sources})
    (args.out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(f'{len(rows)} focused timer instruction cases passed')
if __name__=='__main__':main()
