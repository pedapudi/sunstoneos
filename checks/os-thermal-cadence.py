#!/usr/bin/env python3
"""Execute OS thermal scheduling, sample publication and capture with modeled THS delays."""
import argparse
import importlib.util
import json
from pathlib import Path
import shlex
import struct
from harness import CLANG, LLD, Image, Steps, hook_addresses
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_SP, UC_ARM64_REG_LR, UC_ARM64_REG_PC, UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3
ROOT=Path(__file__).resolve().parents[1]
CHECK_INPUTS=['apps/os/thermal.c','apps/os/thermal-report.h','apps/os/thermal-report.py','checks/os-thermal-cadence.c','include/FreeRTOSConfig.h']
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build',required=True,type=Path)
p.add_argument('--output',required=True,type=Path)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
spec=importlib.util.spec_from_file_location('cadence_report',ROOT/'apps/os/thermal-report.py');report=importlib.util.module_from_spec(spec);spec.loader.exec_module(report)
values=dict(line.split(' = ',1) for line in (a.build/'CMakeFiles/sunstone-a733-os.dir/flags.make').read_text().splitlines() if ' = ' in line)
flags=[CLANG,'--target=aarch64-none-elf',*shlex.split(values['C_DEFINES']),*shlex.split(values['C_INCLUDES']),*shlex.split(values['C_FLAGS'])]
assert '-DSUNSTONE_A733_OS_CPU_CLOCK=1' in flags
steps=Steps(a.output,'commands.json');objects=[]
for name in ('checks/os-thermal-cadence.c','runtime/memory.c'):
 obj=a.output/(Path(name).stem+'.o');objects.append(obj)
 steps.command('compile-'+obj.stem,[*flags,'-c',ROOT/name,'-o',obj])
elf=a.output/'check.elf';steps.command('link',[LLD,'-Ttext=0x41000000','--entry=thermal_cadence_check','--gc-sections',*objects,'-o',elf])
image=Image(elf);m=Uc(UC_ARCH_ARM64,UC_MODE_ARM);m.mem_map(0x200000,0x10000);m.mem_map(0x41000000,0x400000);m.mem_map(0x42000000,0x10000)
for address,data in image.parts:m.mem_write(address,data)
stop=0x4200fff0
hook_addresses(m,[stop],lambda cpu,address,size,unused:cpu.emu_stop())
def call(mode,now=1000000,delay=0,irq=0):
 for reg,value in ((UC_ARM64_REG_SP,0x4200f000),(UC_ARM64_REG_LR,stop),(UC_ARM64_REG_X0,mode),(UC_ARM64_REG_X1,now),(UC_ARM64_REG_X2,delay),(UC_ARM64_REG_X3,irq)):m.reg_write(reg,value)
 m.emu_start(image.address('thermal_cadence_check'),stop,count=100000)
 assert m.reg_read(UC_ARM64_REG_PC)==stop
 return struct.unpack('<8Q',m.mem_read(image.address('thermal_cadence_result'),64))
def window():
 raw=bytes(m.mem_read(image.address('window'),report.HEADER_BYTES));h=report.header(raw,11,12)
 # This fixture starts the first window after the first actual sample.
 raw+=bytes(m.mem_read(image.address('readings'),h['rows']*report.ROW_BYTES))
 decoded=report.decode(raw,11,12);decoded['classification']=report.classify(decoded);return decoded
cases=[]
call(0);call(2,1000010);call(3,1000020);empty=window()
assert empty['classification']=='incomplete' and empty['scheduled_first_us']==0
cases.append('window-ending-before-first-sample')
call(0);call(1,1000000,2);call(2,1000010)
# One-ms polling and real read completion jitter: periods can span125002 then
#124998us, while every sample remains close to its fixed scheduled slot.
for tick in range(1,501):
 # Poll one microsecond before each25ms boundary, then at the boundary.
 wake=999 if tick%25==24 else 0
 call(1,1000000+tick*1000+wake,4 if tick%250==125 else 2)
call(3,1500010);r=window()
assert r['classification']=='complete' and r['rows']==5
assert [x['time_us'] for x in r['readings']][:3]==[1000002,1125004,1250002]
assert r['scheduled_first_us']==1000000
cases.append('actual-one-ms-polls-and-completion-jitter-without-drift')
for extra,expected in ((0,'complete'),(1,'incomplete')):
 call(0);call(1);call(2,1000010)
 # One wakeup tick plus one read/publication tick and counter quantization.
 r=call(1,1126000,1001+extra);call(3,r[5]+10)
 assert window()['classification']==expected
cases.append('actual-read-completion-at-and-beyond-lateness-budget')
call(0);call(1);before=call(1,1010000,100,1);assert before[3]==2 and before[0]==1025000
assert call(1,1024000)[3]==2
assert call(1,1025000)[3]==3
cases.append('early-irq-preserves-periodic-deadline')
call(0);call(1);r=call(1,1024000,2000,1);assert r[0]==1050000 and r[3]==2
assert call(1,1027000)[3]==2
cases.append('read-completion-skips-crossed-deadline')
call(0);call(1);call(2,1000010);r=call(1,1400000,3)
assert r[3]==2 and r[2]==2 and r[0]>r[5] and r[1]>r[5]
call(3,1400010);assert window()['classification']=='incomplete'
cases.append('stall-publishes-once-and-remains-incomplete')
call(0);call(1);call(2,1000010);r=call(4,1010000,3,1)
assert r[2]==2 and r[4]==1
call(3,1010010);assert window()['classification']=='thermal-stop'
cases.append('critical-records-before-deadline-and-terminates')
# A later recognition cannot rewrite the first published window.
call(0);call(1);call(2,1000010);call(1,1125000);call(3,1125010);first=window()
call(1,1250000);call(2,1250010);call(3,1250020);assert window()==first
cases.append('first-window-remains-immutable')
(a.output/'result.json').write_text(json.dumps(dict(status='PASS',cases=cases,report=first,limits='Actual native OS step/record/window; modeled privilege, driver completion and policy, no physical sensor qualification.'),indent=2)+'\n')
print('PASS OS thermal cadence:',len(cases),'native cases')
