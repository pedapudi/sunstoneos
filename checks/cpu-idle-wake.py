#!/usr/bin/env python3
"""Execute the private CPU idle phase with actual task creation and scheduling.

Start from admitted EL1/runtime fixtures. Reuse prior startup/coherence/table
evidence, execute the workload prefix once and checkpoint at the first idle-stage
entry. Each race resumes there and completes remaining work. No physical access.
"""
import argparse
import ctypes
import hashlib
import importlib.util
import json
import pickle
import struct
from pathlib import Path
from unicorn.arm64_const import UC_ARM64_REG_PC, UC_ARM64_REG_SP

ROOT=Path(__file__).resolve().parents[1]
def load(name,path):
    spec=importlib.util.spec_from_file_location(name,ROOT/path)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module
cached=load('cached','checks/a733-smp-cached.py')
idle=load('idle','checks/cpu-power-states.py')
EPOCH=7

class Model(cached.Machine):
    def __init__(self,image):
        super().__init__(image)
        self.pending=[idle.Pending(self,c) for c in range(8)]
        self.waiting=[False]*8;self.sgis=[0]*8;self.taken=[{} for _ in range(8)]
        self.wfi=[0]*8;self.terminal=set();self.outcome=None;self.quantum=0
        self.checkpoint=True;self.mode='normal';self.injected=False;self.held=False
        self.boundaries=[];self.events=[];self.suspended=[]
        address=self.symbols['a733_qualification_idle']
        self.wfi_address=next(i.address for i in self.decoder.disasm(self.read(address,256),address) if i.mnemonic=='wfi')
        for c,u in enumerate(self.cpus):
            r=self.regs[c];r.update(currentel=4,spsel=1,daif=0x3c0,
                sctlr_el1=0x30d51805,scr_el3=0x531,tpidr_el1=c,
                vbar_el1=self.symbols['a733_cached_el1_vectors'],
                icc_pmr_el1=0xf0,icc_igrpen1_el1=1,icc_ctlr_el1=0,
                sp_el1=self.symbols['a733_cpu_el1_stacks']+(c+1)*32768)
            u.reg_write(UC_ARM64_REG_SP,r['sp_el1'])
            u.reg_write(UC_ARM64_REG_PC,self.symbols['a733_cached_el1'])
            for i,value in enumerate((c,EPOCH,24000000,480000000,self.rdist[c])):u.reg_write(cached.GPR[i],value)
            self.active[c]=True
            self.mmio[self.rdist[c]+0x10100]=1
            self.mmio[self.rdist[c]+0x10200]=0
            self.mmio[self.rdist[c]+0x1041e]=0

    def instruction(self,u,a,size,c):
        r=self.regs[c]
        if a==self.symbols['a733_smp_pre_scheduler']:
            self.boundaries.append((c,hex(a)));self.put(c,'x0',1)
            u.reg_write(UC_ARM64_REG_PC,self.reg(c,'x30'));return
        if a==self.symbols['a733_qualification_idle_stage'] and self.checkpoint:
            self.checkpoint=False;self.outcome='checkpoint';u.emu_stop();return
        if a==self.symbols['vTaskSuspend']:
            assert self.reg(c,'x0')==self.q(self.symbols['backgrounds']+c*8)
            assert self.reg(c,'x0')!=self.q(self.symbols['pxCurrentTCBs']+c*8)
            self.suspended.append(c)
        if a==self.symbols['a733_qualification_terminal_el0']:
            self.terminal.add(c);self.active[c]=False;u.emu_stop();return
        if a in (self.symbols['platform_panic'],self.symbols['a733_cached_smc']):
            self.events.append(('retained',c,hex(a),self.reg(c,'x0'),self.reg(c,'x1')))
            self.outcome='retained';u.emu_stop();return
        if self.mode=='notify-before-block' and c==3 and a==self.symbols['ulTaskGenericNotifyTake'] and not self.injected:
            self.held=True;self.injected=True;self.events.append(('paused-before-notification-take',c));u.emu_stop();return
        if a==self.wfi_address and c==3 and self.q(self.symbols['ready']+3*8)==EPOCH and self.mode=='irq-before-wfi' and not self.injected:
            self.pending[c].add(0);self.injected=True;self.events.append(('irq-before-wfi',c))
        if a==self.wfi_address+4 and c==3 and self.wfi[c] and self.mode=='irq-after-wfi' and not self.injected:
            self.pending[c].add(0);self.injected=True;self.events.append(('irq-after-wfi',c))
        if self.mode=='phase-deadline' and self.q(self.symbols['phase_deadline']) and not self.injected:
            self.clock=self.q(self.symbols['phase_deadline']);self.injected=True
        deliver=(r['currentel']<=4 and not r['daif']&128 and r['icc_igrpen1_el1']&1 and
                 r['icc_pmr_el1']>0xe0 and self.pending[c] and self.irq_active[c] is None)
        if a not in self.decoded:self.decoded[a]=next(self.decoder.disasm(bytes(u.mem_read(a,4)),a))
        ins=self.decoded[a];parts=ins.op_str.split(', ')
        if not deliver and ins.mnemonic=='wfi':
            assert r['currentel']==0 and r['sctlr_el1']&(1<<16)
            assert a==self.wfi_address
            self.wfi[c]+=1
            if not self.pending[c]:self.waiting[c]=True
        if not deliver and ins.mnemonic=='mrs' and parts[1]=='cntp_ctl_el0':
            value=r['cntp_ctl_el0']
            if value&1 and self.clock>=r['cntp_cval_el0']:value|=4
            self.put(c,parts[0],value);u.reg_write(UC_ARM64_REG_PC,a+4);return
        if not deliver and ins.mnemonic=='msr':
            value=self.reg(c,parts[1])
            if parts[0]=='cntp_ctl_el0' and not value&1:self.pending[c].discard(30)
            if parts[0]=='icc_pmr_el1':
                r['icc_pmr_el1']=value&0xf0;u.reg_write(UC_ARM64_REG_PC,a+4);return
        super().instruction(u,a,size,c)

    def run(self):
        self.outcome=None
        for _ in range(20000):
            for index in range(8):
                c=(index+self.quantum)%8;u=self.cpus[c];r=self.regs[c]
                if not self.active[c]:continue
                if c==3 and self.held:
                    if not self.q(self.symbols['a733_qualification_idle_wakes']):continue
                    self.held=False;self.events.append(('notification-already-published',c))
                if r['cntp_ctl_el0']&3==1 and self.clock>=r['cntp_cval_el0']:self.pending[c].add(30)
                if self.waiting[c]:continue
                try:u.emu_start(u.reg_read(UC_ARM64_REG_PC),0x42200000,count=(211,347,509,673,887)[(self.quantum+c)%5])
                except Exception as error:raise RuntimeError((c,hex(u.reg_read(UC_ARM64_REG_PC)),self.clock,str(error))) from error
                if self.outcome:return self.outcome
            if len(self.terminal)==8:return 'terminal'
            self.quantum+=1;self.clock+=1000
        raise AssertionError(('instruction budget',self.clock,[hex(u.reg_read(UC_ARM64_REG_PC)) for u in self.cpus]))

    def save(self,path):
        state={k:v for k,v in self.__dict__.items() if k not in ('cpus','ram','ram_pointer','decoder','decoded','pending')}
        state['ram_bytes']=bytes(self.ram);state['contexts']=[u.context_save() for u in self.cpus]
        state['pending_values']=[set(p) for p in self.pending]
        path.write_bytes(pickle.dumps(state))
    def restore(self,path):
        state=pickle.loads(path.read_bytes());ctypes.memmove(self.ram_pointer,state.pop('ram_bytes'),0x1200000)
        contexts=state.pop('contexts');pending=state.pop('pending_values');self.__dict__.update(state)
        self.pending=[idle.Pending(self,c) for c in range(8)]
        for c in range(8):self.pending[c].update(pending[c]);self.cpus[c].context_restore(contexts[c])

def main():
    parser=argparse.ArgumentParser();parser.add_argument('elf',type=Path);parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--resume',action='store_true');args=parser.parse_args();args.out.mkdir(parents=True,exist_ok=True)
    checkpoint=args.out/'before-idle.pkl';digest=hashlib.sha256(args.elf.read_bytes()).hexdigest()
    model_sources=[Path(__file__),ROOT/'checks/a733-smp-cached.py',ROOT/'checks/cpu-power-states.py']
    model_hashes={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in model_sources}
    if args.resume:
        previous=json.loads((args.out/'checkpoint.json').read_text())
        assert previous['elf_sha256']==digest and previous['model_sources']==model_hashes
    else:
        m=Model(args.elf);assert m.run()=='checkpoint',m.events
        m.save(checkpoint);(args.out/'checkpoint.json').write_text(json.dumps({'elf_sha256':digest,'steps':m.steps,'clock':m.clock,'model_sources':model_hashes})+'\n')
    cases=[]
    for mode in ('normal','notify-before-block','irq-before-wfi','irq-after-wfi','phase-deadline'):
        m=Model(args.elf);m.restore(checkpoint);m.mode=mode
        result=m.run();assert result==('retained' if mode=='phase-deadline' else 'terminal'),(mode,result,m.events)
        if mode!='normal':assert m.injected,mode
        if result=='terminal':
            assert all(m.wfi),m.wfi
            assert sorted(m.suspended)==list(range(8)),m.suspended
            for c in range(8):
                assert m.q(m.symbols['pxCurrentTCBs']+c*8)==m.q(m.symbols['workers']+c*8)
                assert m.q(m.symbols['a733_qualification_idle_completed']+c*8)>0
                assert m.q(m.symbols['a733_qualification_idle_wakes']+c*8)==(7 if c else 6)
                rounds,bg,sgis,failed,ticks=struct.unpack('<5Q',m.read(m.symbols['a733_smp_core_results']+c*128,40))
                assert rounds==32 and bg and sgis and not failed and (ticks==1 if c else ticks>=32)
        cases.append({'case':mode,'outcome':result,'clock':m.clock,'phase_deadline':m.q(m.symbols['phase_deadline']),'suspended_backgrounds':m.suspended,'wfi':m.wfi,'events':m.events,'terminal':sorted(m.terminal)})
    sources=[Path(__file__),ROOT/'checks/a733-smp-cached.py',ROOT/'checks/cpu-power-states.py',ROOT/'apps/cpu-runtime-qualification/idle.c']
    result={'elf_sha256':digest,'cases':cases,'sources':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},
        'scope':'Actual task creation, scheduler, workload, idle hook and wake paths from admitted EL1 fixtures. Startup/pre-scheduler/memory-table preparation reused as boundaries; peripherals/time modeled. Stops before terminal SVC; separate native cleanup checks cover retirement. No physical power or timing claim.'}
    (args.out/'result.json').write_text(json.dumps(result,indent=2)+'\n');print('PASS five actual scheduler idle/wake cases from one workload checkpoint')
if __name__=='__main__':main()
