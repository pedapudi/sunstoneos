#!/usr/bin/env python3
"""Execute the linked initial-state guard; device reads and THS readiness are modeled.

Actual report/bin/oscillator decode, all guard branches and policy freshness
checks execute. No physical authority or measured state is produced.
"""
import argparse
import importlib.util
from pathlib import Path
from unicorn.arm64_const import UC_ARM64_REG_PC, UC_ARM64_REG_LR
from harness import Image, save
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('entry_prior',ROOT/'checks/cpu-runtime-qualification.py')
prior=importlib.util.module_from_spec(spec);spec.loader.exec_module(prior)

class Model(prior.Model):
    def __init__(self,image,layout):
        super().__init__(image);self.layout=layout;self.thermal_ready=1
        for a,n in ((0x08871000,0x3000),(0x07090000,0x1000),(0x03006000,0x1000)):
            self.uc.mem_map(a,n)
        self.put32(0x07090160,0x10008002);self.put32(0x0300624c,0x400)
        for i,(pll,mux) in enumerate(zip((0xf8802600,0xf8802700,0xf8802600),(0x03010000,0x03000000,0x03010105))):
            self.put32(0x08871000+4096*i,pll);self.put32(0x0887101c+4096*i,mux)
        self.c=0x41ed0000;self.p=0x41ee0000
        c=layout.layouts['a733_cpu_clock'];p=layout.layouts['a733_thermal_policy'];g=layout.layouts['a733_cpu_clock_grant']
        config=self.c+c['config'];grant=config+layout.layouts['a733_cpu_clock_config']['grant']
        self.put32(grant+g['clusters'],3);self.put32(grant+g['max_mhz'],1794);self.put32(grant+g['max_mhz']+4,2002)
        self.put32(grant+g['changes'],4);self.put32(self.c+c['changes'],4);self.put32(self.c+c['inherited'],3)
        self.put32(self.c+c['rail_uv'],800000);self.put32(self.c+c['rail_uv']+4,800000)
        self.put32(self.c+c['report']+layout.layouts['a733_cpu_clock_report']['sid_word'],0x400)
        self.put32(self.p+p['initialized'],1);self.put32(self.p+p['cpu_critical_millicelsius'],70000)
        self.put32(self.p+p['published_seen'],31)
        for i in range(5):self.put64(self.p+p['seen_us']+i*8,1000000)
        self.read_callback=layout.address('read_word','return-cpu.c')
    def code(self,uc,a,size,unused):
        if a==self.layout.address('os_cpu_return_thermal_ready'):
            uc.reg_write(prior.fault.REGS[0],self.thermal_ready);uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_LR));return
        super().code(uc,a,size,unused)
    def guard(self):
        self.writes.clear()
        value=self.call(0,'os_cpu_return_entry_guard',(self.c,self.p,1000100,self.read_callback,0))
        assert not [w for w in self.writes if 0x08871000<=w[1]<0x08874000],self.writes
        value &= 0xffffffff
        return value if value<2**31 else value-2**32

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('elf',type=Path);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
    a.out.mkdir(parents=True,exist_ok=False)
    names={'a733_cpu_clock','a733_cpu_clock_grant','a733_cpu_clock_config','a733_cpu_clock_report','a733_thermal_policy'}
    layout=Image(a.elf,names);image=prior.fault.Image(a.elf);rows=[]
    c=layout.layouts['a733_cpu_clock'];policy=layout.layouts['a733_thermal_policy']
    cases=[('success',lambda m:None,1),('thermal-not-ready',lambda m:setattr(m,'thermal_ready',0),-1),
        ('missing-sensor',lambda m:m.put32(m.p+policy['published_seen'],15),0),
        ('stale-sensor',lambda m:m.put64(m.p+policy['seen_us']+32,99),-1),
        ('future-sensor',lambda m:m.put64(m.p+policy['seen_us'],1000200),-1),
        ('throttle',lambda m:m.put32(m.p+policy['level'],1),-1),
        ('critical',lambda m:m.uc.mem_write(m.p+policy['critical'],b'\x01'),-1),
        ('wrong-thermal-trip',lambda m:m.put32(m.p+policy['cpu_critical_millicelsius'],110000),-1),
        ('inspection-incomplete',lambda m:m.put32(m.c+c['inherited'],1),-1),
        ('changed-since-inspection',lambda m:m.put32(m.c+c['changes'],3),-1),
        ('wrong-bin',lambda m:m.put32(0x0300624c,0x300),-1),
        ('unstable-oscillator',lambda m:m.put32(0x07090160,0x8002),-1),
        ('wrong-reference',lambda m:m.put32(0x07090160,0x10000002),-1)]
    for i in range(2):cases.append((f'rail-{i}-not800',lambda m,i=i:m.put32(m.c+c['rail_uv']+4*i,1000000),-1))
    for i in range(3):
        for off in (0,0x1c):cases.append((f'domain-{i}-boot-{off:x}',lambda m,i=i,off=off:m.put32(0x08871000+4096*i+off,0),-1))
        for off,bit in ((4,29),(8,31),(0x18,31)):
            cases.append((f'domain-{i}-pattern-{off:x}',lambda m,i=i,off=off,bit=bit:m.put32(0x08871000+4096*i+off,1<<bit),-1))
    for name,change,expected in cases:
        model=Model(image,layout);change(model);actual=model.guard();assert actual==expected,(name,actual,expected)
        rows.append({'name':name,'passed':True,'result':actual})
    save(a.out/'results.json',{'scope':'actual linked guard with modeled registers/THS readiness; no hardware','cases':rows})
    print('PASS',len(rows),'linked initial-state guard cases')
if __name__=='__main__':main()
