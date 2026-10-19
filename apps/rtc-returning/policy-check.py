#!/usr/bin/env python3
"""Focused linked driver/capability checks; no scheduler or physical access."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR,UC_ARM64_REG_PSTATE,UC_ARM64_REG_CPACR_EL1
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--evidence',type=Path,required=True);a=p.parse_args();e=a.evidence
spec=importlib.util.spec_from_file_location('policy_layout',e/'layout-helper.py');layout=importlib.util.module_from_spec(spec);spec.loader.exec_module(layout)
image=layout.Image(e/'build/policy.elf');rl=image.layouts['a733_rtc'];ol=image.layouts['a733_rtc_ops'];cl=image.layouts['a733_rtc_calendar_result']
assert image.layouts['a733_rtc_snapshot']['cancelled']==84
cases=[]
for variant in ('readonly-rc-poisoned-metadata','readonly-rc-null-input-write','readonly-external','strict-rc-refusal','strict-external','configuration-unavailable'):
 u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0x40000000,0x04000000)
 for address,data in image.parts:
  if data:u.mem_write(address,data)
 u.reg_write(UC_ARM64_REG_PSTATE,5);u.reg_write(UC_ARM64_REG_CPACR_EL1,3<<20)
 r=0x43000000;ops=0x43002000;setting=0x43003000;out=0x43004000;end=0x43fff000
 callbacks={'now':end+0x100,'read':end+0x110,'write':end+0x120,'pause':end+0x130,'cancel':end+0x140}
 state={'reads':[],'writes':[],'time':1000000}
 def put(address,value,size=8):u.mem_write(address,value.to_bytes(size,'little'))
 def get(address,size=8):return int.from_bytes(u.mem_read(address,size),'little')
 for name,target in (('now_us','now'),('read','read'),('write','write')):put(ops+ol[name],callbacks[target])
 if variant=='readonly-rc-null-input-write':put(ops+ol['write'],0)
 if variant=='readonly-rc-poisoned-metadata':
  for name in ('read_only','read_source','prescaler','calibration','oscillator'):put(r+rl[name],0xffffffff,4)
 external=variant in ('readonly-external','strict-external')
 values={0:0x11 if external else 0x401c,4:1 if external else 0,8:15,12:0x7a000000,0x160:0x10000003,
  0x10:20000,0x14:0x010203,0x20:0,0x24:0,0x28:0,0x2c:0,0x30:0,0x50:0}
 def ret(value):u.reg_write(UC_ARM64_REG_X0,value);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
 def code(cpu,address,size,cookie):
  args=[u.reg_read(UC_ARM64_REG_X0+i) for i in range(3)]
  if address==callbacks['now']:state['time']+=1;put(args[1],state['time']);ret(1)
  elif address==callbacks['read']:
   assert args[0]==r;state['reads'].append(args[1])
   if variant=='configuration-unavailable' and args[1]==12:ret(0)
   else:put(args[2],values[args[1]],4);ret(1)
  elif address in (callbacks['write'],callbacks['pause'],callbacks['cancel']):
   state['writes'].append(args);raise AssertionError('Mutation/wait callback must not execute')
 u.hook_add(UC_HOOK_CODE,code)
 def call(name,args):
  for i,v in enumerate(args):u.reg_write(UC_ARM64_REG_X0+i,v)
  u.reg_write(UC_ARM64_REG_SP,0x43ffe000);u.reg_write(UC_ARM64_REG_LR,end)
  u.emu_start(image.address(name),end,count=1000000)
  assert u.reg_read(UC_ARM64_REG_PC)==end,name
  return u.reg_read(UC_ARM64_REG_X0)
 readonly=not variant.startswith('strict')
 result=call('a733_rtc_open_readonly' if readonly else 'a733_rtc_open',[r,ops,r,2000000])
 if variant in ('strict-rc-refusal','configuration-unavailable'):
  assert result==0 and get(r+rl['state'],4)==5 and not state['writes']
  if variant=='configuration-unavailable':assert state['reads']==[0,4,12]
  cases.append({'name':variant,'passed':True,'reads':state['reads'],'writes':state['writes']});continue
 assert result==1 and get(r+rl['state'],4)==1
 assert get(r+rl['read_source'],4)==(1 if external else 2)
 if readonly:
  assert get(r+rl['ops']+ol['write'])==0 and get(r+rl['read_only'],4)==1
  before=list(state['reads']);before_state=get(r+rl['state'],4)
  for name,args in [('a733_rtc_arm',[r,2]),('a733_rtc_irq',[r]),('a733_rtc_finish',[r,1])]:
   assert call(name,args)==0 and state['reads']==before and not state['writes']
   assert get(r+rl['state'],4)==before_state
  # Exercise the real unchanged calendar/source APIs with the retained
  # read-only instance ops. Raw caller ops remain separate authority.
  for flags in (1,2):
   u.mem_write(out,bytes(128));u.mem_write(setting,struct.pack('<4I',flags,20000 if flags==2 else 0,0,0))
   assert call('a733_rtc_initialize',[setting,r+rl['ops'],r,2000000,out])==0
   assert bytes(u.mem_read(out,48))==bytes(48) and state['reads']==before and not state['writes']
  u.mem_write(out,bytes(128));u.mem_write(setting,struct.pack('<4I',4,20000,0,0))
  assert call('a733_rtc_set_calendar',[setting,r+rl['ops'],r,2000000,callbacks['pause'],callbacks['cancel'],out])==1
  assert get(out+cl['outcome'],4)==4 and bytes(u.mem_read(out,48))==bytes(48)
  assert state['reads']==before and not state['writes']
 else:assert get(r+rl['read_only'],4)==0 and get(r+rl['ops']+ol['write'])==callbacks['write']
 cases.append({'name':variant,'passed':True,'constructor_reads':state['reads'],'writes':state['writes'],'readonly_mutators_rejected_without_io':readonly,'snapshot_bytes':88})
result={'passed':True,'physical_access':False,'scope':'Actual linked production open/open_readonly, arm/irq/finish and unchanged initialize/set_calendar entry checks. Time/read callbacks modeled. Tests only changed source/capability boundaries; calendar arithmetic, alarm execution and unchanged monitor suites reused.','policy_elf_sha256':hashlib.sha256((e/'build/policy.elf').read_bytes()).hexdigest(),'checker_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'cases':cases}
(e/'policy-result.json').write_text(json.dumps(result,indent=2)+'\n');print(f'{len(cases)} focused source/capability cases passed')
