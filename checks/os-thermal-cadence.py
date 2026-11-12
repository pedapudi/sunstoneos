#!/usr/bin/env python3
"""Run the actual thermal owner step with delayed reads and cached-policy snapshots.

The owner runs as a task of the real FreeRTOS kernel under the host shim, with the
shim's A733 counter and the kernel's critical sections. Device reads, clock
completion and GIC calls are modeled. The owner branch, policy, record writer,
window publication, quiesce registry and host classification are actual code.
No return is requested, no device is opened and no host coverage limit is changed.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

import host_rtos
from harness import environment

ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build',type=Path,help='accepted for compatibility; the host build needs no OS build')
p.add_argument('--output',type=Path,required=True)
p.add_argument('--thermal-source',type=Path,default=ROOT/'apps/os/thermal.c',help='thermal owner source under test')
p.add_argument('--no-control',action='store_true')
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
spec=importlib.util.spec_from_file_location('thermal_report',ROOT/'apps/os/thermal-report.py')
report=importlib.util.module_from_spec(spec);spec.loader.exec_module(report)
NETWORK=Path.home()/'.cache/sunstoneos/sources/radio/stacks/FreeRTOS-Plus-TCP-b86ad086e9870099bb0ac18a75dcb7f6dd508cc7/source'
DEFINES=['SUNSTONE_A733_FAULT_PAGE=1','SUNSTONE_A733_OS=1','SUNSTONE_BOARD_APPLICATION=1',
         'SUNSTONE_BOARD_SUPERVISED=1','SUNSTONE_BOARD_USB2_DUAL=1']
INCLUDES=[ROOT/'apps/os',ROOT/'apps/board',ROOT/'platform/a733-smp-cached',ROOT/'drivers/usb',ROOT/'network/include',
          NETWORK/'include',NETWORK/'portable/Compiler/GCC']

class Model:
    """The fixture process, driven one command per line."""
    def __init__(self,path):
        self.proc=subprocess.Popen([str(path)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True,
                                   env=environment())
        self.last=[0]*12
    def ask(self,line):
        self.proc.stdin.write(line+'\n');self.proc.stdin.flush()
        reply=self.proc.stdout.readline();assert reply,'fixture exited'
        return reply.split()
    def call(self,op,at,value=0):
        fields=self.ask(f'call {op} {at} {value}')
        self.last=[int(v) for v in fields[1:]]
        return int(fields[0])&0xffffffff
    def state(self):return tuple(self.last)
    def decode(self):
        n=self.last[6];data=bytes.fromhex(self.ask('report')[0]) if n else b''
        return report.decode(data,11,12)
    def read_times(self):return [int(v) for v in self.ask('times')]
    def close(self):
        self.proc.stdin.close();assert self.proc.wait()==0

def build(name,source):
    out=a.output/name
    exe=host_rtos.build(out,['checks/os-thermal-cadence.c','drivers/thermal/a733_thermal.c','apps/os/quiesce.c'],'cadence',
        defines=DEFINES+['THERMAL_SOURCE="'+str(Path(source).resolve())+'"'],includes=INCLUDES,
        flags=['-Wno-error','-w','-ffunction-sections','-fdata-sections'],link=['-Wl,--gc-sections'])
    return Model(exe)

def step(m,at,delay=0,irq=False):
    m.call(4,at,delay)
    if irq:m.call(11,at)
    return m.call(1,at)

def finish(m,at,success=1):
    m.call(3,at,success);return m.decode()

m=build('fixed',a.thermal_source);cases=[]
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
# The owner sleeps between readings (at most 20 ms, to the next reading, the interrupt wakes it) and steps on the next tick
# only while it has work that cannot wait: starting, retaining, a masked interrupt awaiting its rearm, or the CPU clock
# driver working.
m.call(0,1000000);step(m,1000000)
assert m.call(12,1000000)==20 and m.call(12,1015000)==10 and m.call(12,1024500)==1 and m.call(12,1030000)==1
for label,change in [('inactive',(8,1)),('retaining',(6,1)),('rearm-pending',(7,1)),('clock-busy',(13,1))]:
    m.call(0,1000000);step(m,1000000);assert m.call(12,1000000)==20,label
    m.call(*change[:1],1000000,change[1]);assert m.call(12,1000000)==1,label
    cases.append('wakes-next-tick-'+label)
m.call(0,1000000);step(m,1000000);m.call(13,1000000,1);m.call(13,1000000,0);assert m.call(12,1000000)==20
cases.append('idle-sleep-to-next-reading')
# The owner steps only when idle_ticks wakes it. The first usable row comes from an interrupt-notified read, off the 25 ms
# read grid, and starts the 125 ms record phase there: each later record slot must wake the owner (board run
# 1793934571637827194-6be652e2 recorded every slot after the first 19.9 ms late, on the next periodic read).
m.call(0,1000000);m.call(9,1000000,0);step(m,1000000)
m.call(9,1010000,31);step(m,1010000,0,True)
assert m.state()[0]==1 and m.state()[10]==1010000
m.call(2,1010001)
t=1010000
while t<1600000:
    t+=1000*m.call(12,t);step(m,t)
r=finish(m,t+10)
assert r['rows']>=5 and report.classify(r)=='complete',[x['time_us'] for x in r['readings']]
cases.append('idle-sleep-wakes-for-each-record-slot')
controls={}
if not a.no_control:
    # Negative control: a read that restarts the 25 ms sensor period from its own completion
    # drifts the phase; the same cases must fail against that owner.
    mutant=host_rtos.mutated(a.output,'apps/os/thermal.c','thermal.next_read+=(1+(t-thermal.next_read)/25000)*25000;',
                             'thermal.next_read=t+25000;','thermal-phase-drift.c')
    done=subprocess.run([sys.executable,'-B',__file__,'--output',str(a.output/'control'),'--thermal-source',str(mutant),
                         '--no-control'],capture_output=True,text=True)
    assert done.returncode!=0,'control passed: sensor phase drift'
    controls['sensor-phase-drift']='fails'
(a.output/'result.json').write_text(json.dumps(dict(passed=True,cases=cases,negative_controls=controls,
    period_us=125000,lateness_us=2001,source_sha256=hashlib.sha256((ROOT/'apps/os/thermal.c').read_bytes()).hexdigest(),
    quiesce_sha256=hashlib.sha256((ROOT/'apps/os/quiesce.c').read_bytes()).hexdigest(),
    limits='Normal operation without a return claim; modeled device/task boundaries; actual owner, policy, registry, writer and v2 classifier.'),indent=2)+'\n')
print('PASS thermal cadence:',len(cases),'cases')
