#!/usr/bin/env python3
"""Execute the linked RTC task and driver with modeled scheduler/time/MMIO."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR,UC_ARM64_REG_PSTATE,UC_ARM64_REG_CPACR_EL1
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--evidence',type=Path,required=True);a=p.parse_args();e=a.evidence
helper=e/'layout-helper.py'
spec=importlib.util.spec_from_file_location('layout',helper);layout=importlib.util.module_from_spec(spec);spec.loader.exec_module(layout)
image=layout.Image(e/'build/probe.elf')
variants=['external-readonly','normal-rc-progress','normal-rc-day-zero','calibrated-rc-refusal','rc-disabled','rc-prescaler-changed','rc-source-changed','rc-calibration-selected','rc-gate-lost','rc-unused-factors-changed','rc-shared-dcxo-changed','rc-prescaler-reserved-changed','normal-rc-other-delta','gate-off']
cases=[]
for variant in variants:
 u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0x02000000,0x06000000);u.mem_map(0x40000000,0x04000000)
 for address,data in image.parts:
  if data:u.mem_write(address,data)
 u.reg_write(UC_ARM64_REG_PSTATE,5);u.reg_write(UC_ARM64_REG_CPACR_EL1,3<<20)
 state={'time':1000000,'exit':None,'reads':[],'writes':[],'delays':0,'task':None,'scheduler':False,'rtc_writes':0}
 rc=variant not in ('external-readonly','gate-off')
 initial=20000*86400
 phase=950000 if variant in ('advance-two','day-rollover') else 0
 def value(address):
  elapsed=0 if variant=='no-advance' else state['time']-1000000
  seconds=initial+(elapsed+phase)//1000000
  if variant=='normal-rc-other-delta':seconds=initial+(elapsed+phase)*16000000//(1000000*32*32768)
  values={0x0701020c:0 if variant=='gate-off' else 0x10001,0x03400000:0x33,
   0x0340011c:16 if variant=='irq-enabled' else 0,0x0340021c:16 if variant=='irq-pending' else 0,
   0x0340031c:16 if variant=='irq-active' else 0,0x07090000:0x11,0x07090004:1,
   0x07090010:seconds//86400,0x07090014:((seconds%86400//3600)<<16)|((seconds%3600//60)<<8)|(seconds%60),
   0x07090020:0x1234,0x07090024:0x00010203,0x07090028:0,0x0709002c:0,0x07090030:0,0x07090050:0}
  if rc:
   values.update({0x07090000:0x401c,0x07090004:0,0x07090008:15,0x0709000c:0x7a000000,0x07090160:0x10000003})
   if variant=='normal-rc-day-zero':values[0x07090010]=0
   if variant=='calibrated-rc-refusal':values[0x0709000c]|=1
   if variant=='rc-disabled':values[0x07090160]&=~1
   if variant=='normal-rc-other-delta':values[0x07090008]=0
   if state['delays']:
    if variant=='rc-prescaler-changed':values[0x07090008]=14
    if variant=='rc-source-changed':values[0x07090000]|=1;values[0x07090004]=1
    if variant=='rc-calibration-selected':values[0x0709000c]|=1
    if variant=='rc-gate-lost':values[0x07090160]&=~1
    if variant=='rc-unused-factors-changed':values[0x0709000c]=0x7b000006
    if variant=='rc-shared-dcxo-changed':values[0x07090160]=0x20ff0001
    if variant=='rc-prescaler-reserved-changed':values[0x07090008]=0xffff000f
  if variant=='invalid-source':values[0x07090004]=0
  if variant=='source-changed' and state['delays']:values[0x07090000]=0x15
  if variant=='invalid-calendar':values[0x07090010]=0
  if variant=='inherited-event':values[0x07090030]=1
  assert address in values,hex(address)
  return values[address]
 def ret(v=0):u.reg_write(UC_ARM64_REG_X0,v);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
 hooks={image.address(name):name for name in ['platform_init','xTaskCreateStatic','vTaskStartScheduler','vTaskDelay','platform_exit','platform_panic']}
 def code(cpu,address,size,cookie):
  word=int.from_bytes(u.mem_read(address,4),'little');kind=word&0xffffffe0;reg=word&31
  # Execute actual counter initialization/conversion/monotonic checks. Only
  # architectural register observations are supplied by this fixture.
  if kind in (0xd5384240,0xd53be000,0xd53be020):
   v=4 if kind==0xd5384240 else (25000000 if variant=='counter-frequency' else 24000000) if kind==0xd53be000 else state['time']*24
   if kind==0xd53be020:state['time']+=1
   u.reg_write(UC_ARM64_REG_X0+reg,v);u.reg_write(UC_ARM64_REG_PC,address+4);return
  name=hooks.get(address)
  if name is None:return
  args=[u.reg_read(UC_ARM64_REG_X0+i) for i in range(7)]
  if name=='platform_init':ret()
  elif name=='xTaskCreateStatic':
   assert state['task'] is None and args[0]==image.address('probe',unit='/apps/rtc-returning/main.c') and args[2]==2048 and args[3]==0 and args[4]==3
   state['task']=args[0];ret(0x43000000)
  elif name=='vTaskStartScheduler':
   assert state['task'];state['scheduler']=True;u.reg_write(UC_ARM64_REG_PC,state['task']);u.reg_write(UC_ARM64_REG_X0,0)
   if variant=='write-attempt':
    # Execute the real linked refusing callback before entering the client.
    # The persistent invariant counter must prevent normal-result publication.
    u.reg_write(UC_ARM64_REG_PC,image.address('write_reg',unit='/apps/rtc-returning/main.c'));u.reg_write(UC_ARM64_REG_LR,state['task'])
    u.reg_write(UC_ARM64_REG_X0+1,0x14);u.reg_write(UC_ARM64_REG_X0+2,1)
  elif name=='vTaskDelay':
   assert args[0]==1
   state['delays']+=1;state['time']+=1600000 if variant=='deadline' else 1000;ret()
  elif name=='platform_exit':state['exit']=args[0];u.emu_stop()
  else:raise AssertionError('Unexpected platform panic')
 def read(cpu,access,address,size,v,cookie):
  state['reads'].append(address);assert size==4
  u.mem_write(address,struct.pack('<I',value(address)))
 def write(cpu,access,address,size,v,cookie):
  state['writes'].append([address,v]);raise AssertionError(f'Unexpected MMIO write {address:x}')
 u.hook_add(UC_HOOK_CODE,code);u.hook_add(UC_HOOK_MEM_READ,read,begin=0x02000000,end=0x07ffffff);u.hook_add(UC_HOOK_MEM_WRITE,write,begin=0x02000000,end=0x07ffffff)
 u.reg_write(UC_ARM64_REG_SP,0x43ffe000);u.reg_write(UC_ARM64_REG_LR,0x43fff000)
 u.emu_start(image.address('kernel_main'),0,count=5000000)
 length=int.from_bytes(u.mem_read(0x42000018,8),'little');text=bytes(u.mem_read(0x42000100,length)).decode()
 assert state['task'] and state['scheduler'] and not state['writes'] and length<16127
 success=variant in ('external-readonly','normal-rc-progress','rc-unused-factors-changed','rc-shared-dcxo-changed','rc-prescaler-reserved-changed')
 assert state['exit']==(1 if variant=='write-attempt' else 0),(variant,text)
 if variant=='write-attempt':
  assert 'FAIL unexpected RTC write request' in text and 'COMPLETE native RTC read-only result' not in text and 'PASS native RTC' not in text,text
 else:
  assert 'COMPLETE native RTC read-only result' in text,text
  outcome=0 if success else 1
  assert f'RTC_READ_OUTCOME=0x{outcome:016x}' in text,text
  assert ('RTC_READ_LABEL=COHERENT_SUCCESS' if success else 'RTC_READ_LABEL=CONTROLLED_NEGATIVE') in text,text
  assert ('PASS native RTC coherent read progression' in text)==success,text
  assert 'RTC_READ_REASON=' in text,text
 if variant in ('gate-off','irq-enabled','irq-pending','irq-active'):
  assert 'RTC_READ_OBSERVED_MASK=0x0000000000000001' in text,text
 if variant=='invalid-source':assert 'RTC_READ_OBSERVED_MASK=0x0000000000000007' in text,text
 if variant=='invalid-calendar':assert 'RTC_READ_DAY=0x0000000000000000' in text,text
 if variant=='counter-frequency':
  assert not state['reads'] and 'RTC_READ_OBSERVED_MASK=0x0000000000000000' in text and 'RTC_READ_REASON=counter init' in text,text
 if success:
  assert 'PASS native RTC coherent read progression' in text and 1100000<=state['time']-1000000<1500000,(variant,text)
  expected=1
  assert f'calendar advance seconds=0x{expected:016x}' in text,text
  if variant=='day-rollover':assert 'day=0x0000000000004e21' in text,text
 if variant in ('gate-off','irq-enabled','irq-pending','irq-active'):
  assert not any(0x07090000<=x<=0x07090050 for x in state['reads'])
 if variant=='gate-off':assert state['reads']==[0x0701020c]
 if variant=='external-readonly':assert not any(x in state['reads'] for x in (0x07090008,0x0709000c,0x07090160))
 if rc and success:assert 'RTC_READ_OBSERVED_MASK=0x00000000000000ff' in text and 'RTC_READ_SOURCE_KIND=0x0000000000000002' in text,text
 if variant=='calibrated-rc-refusal':assert 0x07090010 not in state['reads'] and 0x07090160 not in state['reads']
 if variant=='rc-disabled':assert 0x07090010 not in state['reads']
 if variant=='normal-rc-other-delta':assert 'RTC_READ_REASON=calendar progression' in text and 'RTC_READ_SOURCE_KIND=0x0000000000000002' in text,text
 if variant=='deadline':assert state['delays']==1 and 'wait deadline' in text
 # The actual linked callback must reject every store without an MMIO write.
 for offset in (0,0x10,0x14,0x20,0x24,0x28,0x2c,0x30,0x50):
  u.reg_write(UC_ARM64_REG_X0,0);u.reg_write(UC_ARM64_REG_X0+1,offset);u.reg_write(UC_ARM64_REG_X0+2,1);u.reg_write(UC_ARM64_REG_LR,0x43fff000)
  u.emu_start(image.address('write_reg',unit='/apps/rtc-returning/main.c'),0x43fff000,count=100)
  assert u.reg_read(UC_ARM64_REG_X0)==0 and not state['writes']
 (e/(variant+'.log')).write_text(text)
 cases.append({'name':variant,'passed':True,'exit':state['exit'],'operation_outcome':None if variant=='write-attempt' else 0 if success else 1,'elapsed_modeled_us':state['time']-1000000,'delays':state['delays'],'mmio_reads':len(state['reads']),'mmio_writes':state['writes'],'log_bytes':length})
result={'passed':True,'physical_access':False,'scope':'Actual linked kernel_main task creation, finite RTC task, production counter conversion/checks and production RTC open/coherent-read instructions. Scheduler calls, architectural register observations and MMIO device values modeled; no physical timing, scheduling, oscillator accuracy, ROM return or full RTC service qualification. Unchanged monitor checks reused.','elf_sha256':hashlib.sha256((e/'build/probe.elf').read_bytes()).hexdigest(),'checker_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'layout_helper_sha256':hashlib.sha256(helper.read_bytes()).hexdigest(),'cases':cases}
(e/'check-result.json').write_text(json.dumps(result,indent=2)+'\n');print(f'{len(cases)} focused linked RTC cases passed')
