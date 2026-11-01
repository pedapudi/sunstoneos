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
import sys

from harness import Image, LLD
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_SP, UC_ARM64_REG_LR, UC_ARM64_REG_PC

ROOT=Path(__file__).resolve().parents[1]
CHECK_INPUTS=['apps/os/thermal.c','apps/os/thermal-report.py','drivers/thermal/a733_thermal.c',
              'checks/os-thermal-cadence.c','checks/os-thermal-cadence.py']
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build',type=Path,required=True,help='configured OS build with compile_commands.json')
p.add_argument('--output',type=Path,required=True)
p.add_argument('--baseline',type=Path,help='retained thermal.c before the cadence correction')
p.add_argument('--startup-baseline',type=Path,help='retained thermal.c before startup snapshot filtering')
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
spec=importlib.util.spec_from_file_location('thermal_report',ROOT/'apps/os/thermal-report.py')
report=importlib.util.module_from_spec(spec);spec.loader.exec_module(report)
unit=next(u for u in json.loads((a.build/'compile_commands.json').read_text())
          if u['file'].endswith(('/apps/os/thermal.c','/os-image/return-thermal.c')))
argv=shlex.split(unit['command']);flags=argv[:argv.index('-o')]
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
    def state(self):return struct.unpack('<9Q',self.uc.mem_read(self.image.address('cadence_state'),72))
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

def replay(m):
    start=1000000;m.call(0,start)
    m.call(4,start,996);assert m.call(1,start)==1;m.call(2,start+1000)
    for i in range(1,1002):
        at=start+i*1000
        m.call(4,at,0 if i%100==0 else 999)
        assert m.call(1,at)==1
    m.call(3,start+1002000,1)
    return m.decode(),m.read_times()

m=build('fixed',ROOT/'apps/os/thermal.c');r,times=replay(m)
assert report.classify(r)=='complete'
gaps=[y['time_us']-x['time_us'] for x,y in zip(r['readings'],r['readings'][1:])]
assert max(gaps)<=101000 and len(r['readings'])==11
assert m.state()[1]==2100996,'period drifted from first actual snapshot'
cases=['actual-owner-nonread-branch','actual-policy-window-and-classifier','fixed-period-phase']
frozen=m.decode()
m.call(1,2200000);m.call(2,2200001);m.call(3,2200002,0)
assert m.decode()==frozen
cases.append('later-owner-steps-preserve-first-window')
if a.baseline:
    old=build('baseline',a.baseline);before,old_times=replay(old)
    assert report.classify(before)=='incomplete'
    assert max(y['time_us']-x['time_us'] for x,y in zip(before['readings'],before['readings'][1:]))>125000
    assert times==old_times,'snapshot correction added a sensor read'
    cases += ['old-source-reproduces-coverage-failure','unchanged-sensor-read-schedule']

def startup(m,filtered):
    start=1000000;m.call(0,start)
    for offset,mask in [(0,0),(25000,1),(50000,8),(75000,22)]:
        m.call(9,start+offset,mask);assert m.call(1,start+offset)==1
        if offset<75000:
            assert m.state()[8]==1,'startup load released before complete sensors'
            if filtered:assert m.state()[0]==0 and m.state()[1]==0
    assert m.state()[8]==0 and m.state()[0]==1
    m.call(2,start+75001);m.call(3,start+80000,1)
    return m.decode(),m.read_times()

first,first_times=startup(m,True)
assert report.classify(first)=='complete' and first['readings'][0]['seen']==31
assert first['readings'][0]['time_us']==1075000
cases.append('partial-startup-recorded-before-load-release-and-first-window')
if a.startup_baseline:
    old_start=build('startup-baseline',a.startup_baseline);bad,bad_times=startup(old_start,False)
    assert report.classify(bad)=='incomplete' and bad['readings'][0]['seen']==0
    assert first_times==bad_times
    cases.append('previous-startup-row-failure-with-identical-sensor-reads')
# Full coverage with an old channel stays throttled and must not become row 0.
m.call(0,1000000);m.call(9,1000000,1);m.call(1,1000000)
m.call(9,2100001,30);m.call(1,2100001)
assert m.state()[0]==0 and m.state()[1]==0 and m.state()[8]==1
m.call(9,2125001,1);m.call(1,2125001)
assert m.state()[0]==1 and m.state()[8]==0
m.call(2,2125002);m.call(3,2125003,1)
assert report.classify(m.decode())=='complete'
cases.append('stale-startup-snapshot-waits-for-fresh-full-coverage')
# A later stale record remains visible, preserving the failure evidence.
m.call(0,1000000);m.call(1,1000000);m.call(2,1000001)
m.call(9,2100001,0);m.call(1,2100001);m.call(3,2100002,1)
assert m.state()[0]==2 and m.decode()['readings'][-1]['stale']==31
assert report.classify(m.decode())=='incomplete'
cases.append('later-stale-snapshot-is-not-filtered')
# A critical startup failure with no valid channel must still publish row 0.
m.call(0,1000000);m.call(9,1000000,0);m.call(10,1000000,1);m.call(2,1000000)
assert m.call(1,1000000)==0xffffffff and m.state()[0]==1 and m.state()[3]==0
m.call(3,1000001,0)
assert m.decode()['readings'][0]['seen']==0 and m.decode()['readings'][0]['level']==2
cases.append('critical-startup-with-no-seen-channels-is-retained')
# Preserve a genuine long owner stall as incomplete, without invented rows.
m.call(0,1000000);m.call(1,1000000);m.call(2,1000001);m.call(1,1350000)
assert m.state()[0]==2 and m.state()[1]==1400000
m.call(3,1350001,1);assert report.classify(m.decode())=='incomplete'
cases.append('missed-periods-not-backfilled')
# Critical events bypass the periodic deadline and remain visible before fault.
m.call(0,1000000);m.call(1,1000000);m.call(2,1000001);m.call(5,1025000,110000)
assert m.call(1,1025000)==0xffffffff and m.state()[0]==2 and m.state()[4]>0
assert m.state()[1]==1100000 and m.state()[3]==1
m.call(3,1025001,0);assert m.decode()['readings'][-1]['level']==2
cases.append('immediate-critical-snapshot-preserves-phase-and-stops-clock-step')
for label,op in [('inactive',8),('retaining',6),('rearm-fault',7)]:
    m.call(0,1000000);m.call(1,1000000);before=m.state()[0]
    m.call(op,1100000,1)
    # Keep the next sensor read in the future, so this exercises cached branches.
    if label=='rearm-fault':
        # No sensor read is due at the next record deadline after a late poll.
        m.call(0,1000000);m.call(1,1000000);m.call(1,1099000);before=m.state()[0];m.call(7,1100000)
    m.call(1,1100000);assert m.state()[0]==before,label
    cases.append('no-cached-publication-'+label)
(a.output/'result.json').write_text(json.dumps(dict(passed=True,cases=cases,max_gap_us=max(gaps),
    sensor_read_times=times,report=r,
    source_sha256=hashlib.sha256((ROOT/'apps/os/thermal.c').read_bytes()).hexdigest(),
    baseline_sha256=hashlib.sha256(a.baseline.read_bytes()).hexdigest() if a.baseline else None,
    startup_baseline_sha256=hashlib.sha256(a.startup_baseline.read_bytes()).hexdigest() if a.startup_baseline else None,
    script_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
    limits='Modeled device/task boundaries; actual owner, policy and report code.'),indent=2)+'\n')
print('PASS thermal cadence:',len(cases),'cases, maximum gap',max(gaps),'us')
