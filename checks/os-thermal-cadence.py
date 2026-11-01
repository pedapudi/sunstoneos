#!/usr/bin/env python3
"""Run the actual thermal owner step with delayed reads and cached-policy snapshots.

Device reads, task identity, clock completion and GIC calls are modeled. The
owner branch, policy, record writer, window publication and host classification
are actual code. No device is opened and no host coverage limit is changed.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shlex
import struct
import subprocess

from harness import CLANG, Image, LLD
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_SP, UC_ARM64_REG_LR, UC_ARM64_REG_PC

ROOT=Path(__file__).resolve().parents[1]
CHECK_INPUTS=['apps/os/thermal.c','apps/os/thermal-report.py','drivers/thermal/a733_thermal.c',
              'checks/os-thermal-cadence.c','checks/os-thermal-cadence.py',
              'apps/os/thermal-report.h','include/FreeRTOSConfig.h']
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build',type=Path,required=True,help='configured OS build')
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
spec=importlib.util.spec_from_file_location('thermal_report',ROOT/'apps/os/thermal-report.py')
report=importlib.util.module_from_spec(spec);spec.loader.exec_module(report)
if (a.build/'compile_commands.json').exists():
    unit=next(u for u in json.loads((a.build/'compile_commands.json').read_text())
              if u['file'].endswith(('/apps/os/thermal.c','/os-image/return-thermal.c')))
    argv=shlex.split(unit['command']);flags=argv[:argv.index('-o')]
else:
    values=dict(line.split(' = ',1) for line in
                (a.build/'CMakeFiles/sunstone-a733-os.dir/flags.make').read_text().splitlines()
                if ' = ' in line)
    flags=[CLANG,'--target=aarch64-none-elf',*shlex.split(values['C_DEFINES']),
           *shlex.split(values['C_INCLUDES']),*shlex.split(values['C_FLAGS'])]
flags += ['-DSUNSTONE_A733_OS_CPU_CLOCK=1','-I'+str(ROOT/'apps/os')]
commands=[]
def run(cmd):
    cmd=list(map(str,cmd));commands.append(cmd)
    r=subprocess.run(cmd,capture_output=True,text=True)
    (a.output/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
    assert r.returncode==0,r.stdout+r.stderr

class Model:
    def __init__(self,path):
        self.image=Image(path,layout_names=set());self.uc=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
        self.uc.mem_map(0x200000,0x10000)
        self.uc.mem_map(0x41000000,0x1200000);self.uc.mem_map(0x60000000,0x10000)
        for address,data in self.image.parts:self.uc.mem_write(address,data)
    def call(self,op,at,value=0):
        for reg,v in ((UC_ARM64_REG_X0,op),(UC_ARM64_REG_X1,at),(UC_ARM64_REG_X2,value),
                      (UC_ARM64_REG_SP,0x6000f000),(UC_ARM64_REG_LR,0x6000fff0)):
            self.uc.reg_write(reg,v)
        self.uc.emu_start(self.image.address('cadence_control'),0x6000fff0,count=200000)
        assert self.uc.reg_read(UC_ARM64_REG_PC)==0x6000fff0
        return self.uc.reg_read(UC_ARM64_REG_X0)&0xffffffff
    def state(self):return struct.unpack('<12Q',self.uc.mem_read(self.image.address('cadence_state'),96))
    def decode(self):
        n=self.state()[6];return report.decode(bytes(self.uc.mem_read(self.image.address('cadence_report'),n)),11,12)
    def read_times(self):return list(struct.unpack('<'+'Q'*self.state()[2],self.uc.mem_read(self.image.address('cadence_read_times'),8*self.state()[2])))

def build(name,source):
    out=a.output/name;out.mkdir();objects=[]
    for i,path in enumerate((ROOT/'checks/os-thermal-cadence.c',ROOT/'drivers/thermal/a733_thermal.c',ROOT/'runtime/memory.c')):
        obj=out/f'{i}.o';objects.append(obj)
        extra=['-DTHERMAL_SOURCE="'+str(source.resolve())+'"'] if i==0 else []
        run([*flags,*extra,'-c',path,'-o',obj])
    elf=out/'cadence.elf'
    run([LLD,'-Ttext=0x41000000','-Tdata=0x41200000','--entry=cadence_control','--gc-sections',*objects,'-o',elf])
    return Model(elf)

def step(m,at,delay=0,irq=False):
    m.call(4,at,delay)
    if irq:m.call(11,at)
    return m.call(1,at)

def finish(m,at,success=1):
    m.call(3,at,success);return m.decode()

m=build('fixed',ROOT/'apps/os/thermal.c');cases=[]
m.call(0,1000000);m.call(2,1000010)
assert report.classify(finish(m,1000020))=='incomplete'
cases.append('window-ending-before-first-sample')
# Real one-ms owner passes, delayed reads and actual policy updates.
m.call(0,1000000);step(m,1000000,2);m.call(2,1000010)
for tick in range(1,501):
    wake=999 if tick%25==24 else 0
    step(m,1000000+tick*1000+wake,4 if tick%250==125 else 2)
r=finish(m,1500010)
assert report.classify(r)=='complete' and r['rows']==5
assert r['scheduled_first_us']==1000002
assert [x['time_us'] for x in r['readings']][:3]==[1000002,1125004,1250002]
assert m.state()[9]==1525000
cases.append('one-ms-polls-and-read-completion-jitter-without-phase-drift')
for extra,expected in ((0,'complete'),(1,'incomplete')):
    m.call(0,1000000);step(m,1000000);m.call(2,1000010)
    step(m,1126000,1001+extra)
    assert report.classify(finish(m,m.state()[11]+10))==expected
cases.append('read-completion-at-and-beyond-2001us-budget')
m.call(0,1000000);step(m,1000000);step(m,1010000,100,True)
assert m.state()[2]==2 and m.state()[9]==1025000
step(m,1024000);assert m.state()[2]==2
step(m,1025000);assert m.state()[2]==3
cases.append('early-irq-preserves-periodic-sensor-deadline')
m.call(0,1000000);step(m,1000000);step(m,1024000,2000,True)
assert m.state()[9]==1050000 and m.state()[2]==2
step(m,1027000);assert m.state()[2]==2
cases.append('read-completion-skips-crossed-sensor-slot')
# The first read's completion shifts record slots two us after sensor slots.
m.call(0,1000000);step(m,1000000,2);m.call(2,1000010)
step(m,1125000);before=m.state()[2]
step(m,1125002)
assert m.state()[2]==before and m.state()[0]==2
assert report.classify(finish(m,1125010))=='complete'
cases.append('actual-owner-nonread-branch-records-without-extra-ths-read')
first=m.decode();step(m,1375000);m.call(2,1375010);m.call(3,1375020,0)
assert m.decode()==first
cases.append('later-recognition-preserves-first-window')

def startup(m):
    m.call(0,1000000)
    for offset,mask in [(0,0),(25000,1),(50000,8),(75000,22)]:
        m.call(9,1000000+offset,mask);step(m,1000000+offset)
        if offset<75000:assert m.state()[8]==1 and m.state()[0]==0 and m.state()[1]==0
    assert m.state()[8]==0 and m.state()[0]==1
    m.call(2,1075001)
    return finish(m,1080000)
first=startup(m)
assert report.classify(first)=='complete' and first['readings'][0]['seen']==31
assert first['readings'][0]['time_us']==first['scheduled_first_us']==1075000
cases.append('partial-startup-recorded-before-load-release-and-first-window')
m.call(0,1000000);m.call(9,1000000,0);step(m,1000000);m.call(2,1000001)
m.call(9,1025000,31);step(m,1025000)
assert report.classify(finish(m,1025010))=='incomplete'
cases.append('begin-before-first-valid-row-retains-missing-start-coverage')
m.call(0,1000000);m.call(9,1000000,1);step(m,1000000)
m.call(9,2100001,30);step(m,2100001)
assert m.state()[0]==0 and m.state()[1]==0 and m.state()[8]==1
m.call(9,2125001,1);step(m,2125001)
assert m.state()[0]==1 and m.state()[8]==0
m.call(2,2125002);assert report.classify(finish(m,2125003))=='complete'
cases.append('stale-startup-waits-for-full-fresh-coverage')
m.call(0,1000000);step(m,1000000);m.call(2,1000001)
m.call(9,2100001,0);step(m,2100001)
r=finish(m,2100002)
assert r['readings'][-1]['stale']==31 and report.classify(r)=='incomplete'
cases.append('later-stale-snapshot-remains-visible')
m.call(0,1000000);m.call(9,1000000,0);m.call(10,1000000,1);m.call(2,1000000)
assert step(m,1000000)==0xffffffff and m.state()[0]==1 and m.state()[3]==0
r=finish(m,1000001,0)
assert r['readings'][0]['seen']==0 and r['readings'][0]['level']==2
cases.append('critical-startup-with-missing-channels-remains-visible')
m.call(0,1000000);step(m,1000000);m.call(2,1000001);step(m,1400000,3)
assert m.state()[0]==2 and m.state()[1]==1500000 and m.state()[9]==1425000
assert report.classify(finish(m,1400010))=='incomplete'
cases.append('missed-slots-not-backfilled')
m.call(0,1000000);step(m,1000000);m.call(2,1000001);m.call(5,1025000,110000)
assert step(m,1025000)==0xffffffff and m.state()[0]==2 and m.state()[4]>0
assert m.state()[1]==1125000 and m.state()[3]==1
r=finish(m,1025001)
assert r['readings'][-1]['level']==2 and report.classify(r)=='thermal-stop'
cases.append('immediate-critical-preserves-phase-and-stops-cpu-step')
for label,op in [('inactive',8),('retaining',6),('rearm-fault',7)]:
    m.call(0,1000000);step(m,1000000);step(m,1124000,2000);before=m.state()[0]
    m.call(op,1127000,1);step(m,1127000)
    assert m.state()[0]==before,label
    cases.append('no-cached-publication-'+label)
(a.output/'result.json').write_text(json.dumps(dict(passed=True,cases=cases,
    period_us=125000,lateness_us=2001,source_sha256=hashlib.sha256((ROOT/'apps/os/thermal.c').read_bytes()).hexdigest(),
    limits='Modeled device/task boundaries; actual owner, policy, writer and v2 classifier.'),indent=2)+'\n')
print('PASS thermal cadence:',len(cases),'cases')
