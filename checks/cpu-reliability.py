#!/usr/bin/env python3
"""Run the CPU payload protocol with actual FreeRTOS scheduling and FP assembly.

OS admission, clock/thermal reports and architectural I/O are fixtures. Eight
rounds exercise the same loop as the 4096-round board application. This checks
scheduler integration, protocol races and refusal; no physical qualification.
"""
import argparse
import hashlib
import importlib.util
import json
import struct
from pathlib import Path
from unicorn.arm64_const import UC_ARM64_REG_PC, UC_ARM64_REG_SP
from harness import refused

ROOT=Path(__file__).resolve().parents[1]
def load(name,path):
    spec=importlib.util.spec_from_file_location(name,ROOT/path)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module
cached=load('cached','checks/a733-smp-cached.py')
decoder=load('decoder','apps/cpu-runtime-qualification/reliability-host.py')

class Model(cached.Machine):
    def __init__(self,image,case):
        super().__init__(image)
        self.case=case;self.outcome=None;self.failure=None;self.payload=None
        self.injected=False;self.hold=0
        self.slot=self.symbols['slots'];self.done=self.symbols['done']
        self.finish_count=0
        for c,u in enumerate(self.cpus):
            r=self.regs[c];r.update(currentel=4,spsel=1,daif=0x3c0,
                sctlr_el1=0x30d51805,scr_el3=0x531,tpidr_el1=c,
                vbar_el1=self.symbols['a733_cached_el1_vectors'],icc_pmr_el1=0xf0,
                icc_igrpen1_el1=1,icc_ctlr_el1=0,
                sp_el1=self.symbols['a733_cpu_el1_stacks']+(c+1)*32768)
            u.reg_write(UC_ARM64_REG_SP,r['sp_el1'])
            u.reg_write(UC_ARM64_REG_PC,self.symbols['a733_cached_el1'])
            for i,v in enumerate((c,7,24000000,480000000,self.rdist[c])):u.reg_write(cached.GPR[i],v)
            self.active[c]=True
            self.mmio[self.rdist[c]+0x10100]=1
            self.mmio[self.rdist[c]+0x10200]=0
    def instruction(self,u,a,size,c):
        if a==self.symbols['a733_smp_pre_scheduler']:
            self.put(c,'x0',1);u.reg_write(UC_ARM64_REG_PC,self.reg(c,'x30'));return
        if a==self.symbols['ss_finish']:
            self.finish_count+=1
            self.payload=self.read(self.reg(c,'x0'),self.reg(c,'x1'))
            self.outcome='finished';u.emu_stop();return
        if a in (self.symbols['ss_fail'],self.symbols['platform_panic'],self.symbols['a733_cached_smc']):
            self.failure=(c,hex(a),self.reg(c,'x0'),self.reg(c,'x1'))
            self.outcome='failed';u.emu_stop();return
        pause=(self.case=='suspend-race' and a==self.symbols['vTaskSuspend'] or
               self.case=='done-before-notify' and a==self.symbols['xTaskGenericNotify'])
        if pause and c==3 and not self.injected and int.from_bytes(self.read(self.done+3*4,4),'little')==1:
            self.injected=True;self.hold=10000;u.emu_stop();return
        if self.case=='wrong-clock' and a==self.symbols['ss_clock_info']:
            u.mem_write(self.reg(c,'x0'),b'CLOCK cpu_l_mhz=1014 cpu_b_mhz=1014 cpu_l_mv=800 cpu_b_mv=800 '+bytes(1))
            self.put(c,'x0',70);self.injected=True
            u.reg_write(UC_ARM64_REG_PC,self.reg(c,'x30'));return
        if self.case=='thermal-throttle' and a==self.symbols['ss_throttled']:
            self.put(c,'x0',1);self.injected=True
            u.reg_write(UC_ARM64_REG_PC,self.reg(c,'x30'));return
        super().instruction(u,a,size,c)
    def memory(self,u,access,a,size,value,c):
        # Match the one field whose release publishes CPU3's first payload.
        from unicorn import UC_MEM_WRITE
        if self.case=='payload-corrupt' and access==UC_MEM_WRITE and a==self.slot+3*640 and value==1 and not self.injected:
            self.injected=True
            u.mem_write(self.slot+3*640+128,bytes(8))
        super().memory(u,access,a,size,value,c)
    def run_case(self):
        for quantum in range(25000):
            if self.case=='missing-producer' and self.injected and quantum%100==0:
                self.cpus[0].mem_write(self.symbols['xTickCount'],struct.pack('<I',12000))
            for index in range(8):
                c=(index+quantum)%8;u=self.cpus[c];r=self.regs[c]
                if self.case=='missing-producer' and c==3 and int.from_bytes(self.read(self.symbols['started'],4),'little')==1:
                    self.injected=True;continue
                if c==3 and self.hold:
                    self.hold-=1
                    continue
                if not c:self.timer_level(c)
                if r['currentel']<=4 and not r['daif']&128 and r['icc_igrpen1_el1']&1 and r['icc_pmr_el1']>0xe0 and self.pending[c] and self.irq_active[c] is None:
                    self.exception(c,1,u.reg_read(UC_ARM64_REG_PC),irq=True)
                u.emu_start(u.reg_read(UC_ARM64_REG_PC),0x42200000,count=(211,347,509,673,887)[(quantum+c)%5])
                if self.outcome:return self.outcome
            self.clock+=1000
        raise AssertionError(('model budget',self.clock,self.steps,[hex(u.reg_read(UC_ARM64_REG_PC)) for u in self.cpus]))

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('elf',type=Path);ap.add_argument('--out',type=Path,required=True)
    ap.add_argument('--case',choices=('normal','suspend-race','done-before-notify','payload-corrupt','missing-producer','wrong-clock','thermal-throttle'),action='append')
    args=ap.parse_args();args.out.mkdir(parents=True,exist_ok=True)
    for case in args.case or ('normal','suspend-race','done-before-notify','payload-corrupt','missing-producer','wrong-clock','thermal-throttle'):
        model=Model(args.elf,case);outcome=model.run_case()
        assert outcome==('finished' if case in ('normal','suspend-race','done-before-notify') else 'failed'),(case,outcome,model.failure)
        if case!='normal':assert model.injected,case
        if outcome=='finished':
            result=decoder._decode(model.payload,8)
            assert result=={'rounds':8,'core_mask':255,'digests':decoder._digests(8)}
            if case in ('suspend-race','done-before-notify'):assert model.hold>0
        elif case=='payload-corrupt':assert model.failure[2]==0x404,model.failure
        elif case=='missing-producer':assert model.failure[2]>>8==1,model.failure
        elif case=='wrong-clock':assert model.failure[2]==0x706,model.failure
        elif case=='thermal-throttle':assert model.failure[2]>>8==9,model.failure
        record={'case':case,'outcome':outcome,'failure':model.failure,'steps':model.steps,
            'clock':model.clock,'remaining_hold':model.hold,
            'model_sources':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in
                (Path(__file__),ROOT/'checks/a733-smp-cached.py',ROOT/'checks/cpu-reliability-model/application.c',
                 ROOT/'checks/cpu-reliability-model/CMakeLists.txt',ROOT/'apps/cpu-runtime-qualification/reliability.c',
                 ROOT/'apps/cpu-runtime-qualification/reliability.h',ROOT/'apps/cpu-runtime-qualification/reliability-host.py')},
            'elf_sha256':hashlib.sha256(args.elf.read_bytes()).hexdigest(),
            'scope':'Real FreeRTOS scheduler and FP assembly; modeled architecture, OS admission, clocks and thermal state; eight protocol rounds; no physical evidence.'}
        (args.out/(case+'.json')).write_text(json.dumps(record,indent=2)+'\n')
        print('PASS',case,outcome,model.steps,flush=True)
    # Decoder rejection cases share one complete fixture.
    payload=struct.pack('<4Q',decoder.MAGIC,8,255,8)+b''.join(struct.pack('<3Q',8,d,0) for d in decoder._digests(8))
    for offset,value,reason in ((0,0,'header'),(24,7,'few ticks'),(32,7,'incomplete rounds'),(40,0,'digest')):
        bad=bytearray(payload);struct.pack_into('<Q',bad,offset,value)
        refused(lambda:decoder._decode(bad,8),reason)
    print('PASS decoder rejects malformed header, duration, rounds and digest')
if __name__=='__main__':main()
