#!/usr/bin/env python3
"""Execute the linked finite thermal task with modeled scheduler and sensors."""
import argparse,hashlib,importlib.util,json,struct
from pathlib import Path
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR,UC_ARM64_REG_PSTATE,UC_ARM64_REG_CPACR_EL1
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--evidence',type=Path,required=True);a=p.parse_args();e=a.evidence
spec=importlib.util.spec_from_file_location('layout',e/'layout-helper.py');layout=importlib.util.module_from_spec(spec);spec.loader.exec_module(layout)
im=layout.Image(e/'build/probe.elf');cases=[]
for variant in ('normalized26-adopt','normalized26-cold','direct24-adopt','no-samples','invalid-calibration','invalid-ref','unsafe-direct26','irq-pending','irq-after-lock','inherited-enabled','second-set-partial','partial-sets','negative-temperature'):
 u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0x02000000,0x06000000);u.mem_map(0x40000000,0x04000000)
 for addr,data in im.parts:
  if data:u.mem_write(addr,data)
 u.reg_write(UC_ARM64_REG_PSTATE,5);u.reg_write(UC_ARM64_REG_CPACR_EL1,3<<20)
 for name in ('ullCriticalNesting','ullPortInterruptNesting'):u.mem_write(im.address(name),bytes(8))
 s={'time':1000000,'task':None,'scheduler':False,'exit':None,'held':set(),'created':0,'reads':[],'writes':[],'enabled_at':None,'rounds':0}
 regs={0x03400000:0x33,0x03400108:0,0x03400208:0x80 if variant=='irq-pending' else 0,0x03400308:0,
  0x07090160:0x10000002 if variant=='direct24-adopt' else 0x983fb0f7,
  0x02002000:0xf8636300 if variant=='direct24-adopt' else 0xf8675f00,
  0x020020a0:0xff126310,0x020020a8:0,0x020020ac:0,0x02003908:0x0a000a00,0x02003988:0x0a000000,
  0x02002518:0x03000005,0x02002fc0:0x40 if variant=='normalized26-cold' else 0x81000040 if variant in ('direct24-adopt','unsafe-direct26') else 0x80000040,
  0x02002fe4:0,0x03006244:0 if variant=='invalid-calibration' else 0x66961102,0x03006248:0x19689689,0x0300624c:0x496}
 if variant=='invalid-ref':regs[0x02002000]^=0x100
 for off in (0,4,8,0x10,0x14,0x18,0x20,0x24,0x28,0x2c,0x30,0xa0,0xa4,0xa8):regs[0x02522000+off]=0
 if variant=='inherited-enabled':regs[0x02522004]=1
 def ret(v=0):u.reg_write(UC_ARM64_REG_X0,v);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
 names=['platform_init','xTaskCreateStatic','vTaskStartScheduler','vTaskDelay','platform_exit','platform_panic','xTaskGetSchedulerState','xTaskGetCurrentTaskHandle','xQueueCreateMutexStatic','xQueueGetMutexHolder','xQueueSemaphoreTake','xQueueGenericSend']
 hooks={im.address(n):n for n in names}
 def code(cpu,address,size,cookie):
  word=int.from_bytes(u.mem_read(address,4),'little');kind=word&0xffffffe0;reg=word&31
  if kind in (0xd5384240,0xd53be000,0xd53be020):
   value=4 if kind==0xd5384240 else 24000000 if kind==0xd53be000 else s['time']*24
   if kind==0xd53be020:s['time']+=1
   u.reg_write(UC_ARM64_REG_X0+reg,value);u.reg_write(UC_ARM64_REG_PC,address+4);return
  name=hooks.get(address)
  if not name:return
  args=[u.reg_read(UC_ARM64_REG_X0+i) for i in range(8)]
  if name=='platform_init':ret()
  elif name=='xTaskCreateStatic':
   assert args[0]==im.address('probe',unit='/apps/thermal-returning/main.c') and args[2:5]==[4096,0,3]
   s['task']=args[0];ret(1)
  elif name=='vTaskStartScheduler':s['scheduler']=True;u.reg_write(UC_ARM64_REG_PC,s['task']);u.reg_write(UC_ARM64_REG_X0,0)
  elif name=='vTaskDelay':assert args[0]==1 and not s['held'];s['time']+=1000;ret()
  elif name=='xTaskGetSchedulerState':ret(2)
  elif name=='xTaskGetCurrentTaskHandle':ret(1)
  elif name=='xQueueCreateMutexStatic':s['created']+=1;ret(0x43000000+s['created']*0x100)
  elif name=='xQueueGetMutexHolder':ret(1 if args[0] in s['held'] else 0)
  elif name=='xQueueSemaphoreTake':
   assert not s['held'];s['held'].add(args[0])
   if variant=='irq-after-lock':regs[0x03400208]=0x80
   ret(1)
  elif name=='xQueueGenericSend':assert args[0] in s['held'];s['held'].remove(args[0]);ret(1)
  elif name=='platform_exit':s['exit']=args[0];u.emu_stop()
  else:raise AssertionError('unexpected platform panic')
 def read(cpu,access,address,size,value,cookie):
  assert size==4;s['reads'].append(address)
  if 0x02522000<=address<=0x025220d0:assert s['held'] and regs[0x02002fe4]&0x10001==0x10001
  if address==0x02522020 and regs[0x02522004]==31 and s['enabled_at'] is not None:
   elapsed=s['time']-s['enabled_at']
   if elapsed>10000 and variant!='no-samples':
    if variant=='partial-sets':regs[address]|=1<<(s['rounds']%5)
    elif s['rounds']==0:regs[address]|=31
    elif elapsed>20000:regs[address]|=15 if variant=='second-set-partial' else 31
  if 0x025220c0<=address<=0x025220d0:
   v=(3000 if variant=='negative-temperature' else 2400)+((address-0x025220c0)//4)*5+s['rounds']
  else:
   assert address in regs,hex(address);v=regs[address]
  u.mem_write(address,struct.pack('<I',v))
 def write(cpu,access,address,size,value,cookie):
  assert size==4 and s['held'];s['writes'].append([address,value]);assert address in regs,hex(address)
  if address in (0x02522020,0x02522024,0x02522028,0x0252202c):
   regs[address]&=~value
   if address==0x02522020 and regs[0x02522004]==31:s['rounds']+=1
  else:
   regs[address]=value
   if address==0x02522004 and value==31:s['enabled_at']=s['time']
 u.hook_add(UC_HOOK_CODE,code);u.hook_add(UC_HOOK_MEM_READ,read,begin=0x02000000,end=0x07ffffff);u.hook_add(UC_HOOK_MEM_WRITE,write,begin=0x02000000,end=0x07ffffff)
 u.reg_write(UC_ARM64_REG_SP,0x43ffe000);u.reg_write(UC_ARM64_REG_LR,0x43fff000)
 u.emu_start(im.address('kernel_main'),0,count=10000000)
 length=int.from_bytes(u.mem_read(0x42000018,8),'little');log=bytes(u.mem_read(0x42000100,length)).decode();assert length<16127
 ok=variant in ('normalized26-adopt','normalized26-cold','direct24-adopt','partial-sets','negative-temperature')
 disabled=ok or variant in ('no-samples','second-set-partial')
 assert s['exit']==(0 if disabled else 1),(variant,log)
 assert ('PASS native calibrated thermal observations' in log)==ok,(variant,log)
 assert ('COMPLETE native thermal configured disable' in log)==disabled,(variant,log)
 if disabled:
  assert not s['held'] and regs[0x02522004]==regs[0x02522014]==regs[0x02522018]==0
  assert f'THERMAL_OUTCOME=0x{0 if ok else 1:016x}' in log and 'THERMAL_CONFIGURED_DISABLED=0x0000000000000001' in log
  assert 'THERMAL_RESOURCES=RETAINED_CLOCKS_LEASES_CONFIGURATION' in log
  assert regs[0x02002fe4]&0x10001==0x10001 and regs[0x02002fc0]&0x80000000
 if ok:
  assert 'THERMAL_SETS=0x0000000000000002' in log
  assert 'THERMAL_FACTORY_TENTH_C=0x0000000000000102' in log
  assert log.count('THERMAL_CODE=')==10
  assert 'THERMAL_CORRECTION=0x00000000000007fc' in log
 if variant=='negative-temperature':assert 'THERMAL_MILLICELSIUS=0xffff' in log
 if variant=='irq-pending':assert not s['writes'] and not any(0x02522000<=x<=0x025220d0 for x in s['reads'])
 if variant in ('invalid-ref','unsafe-direct26','invalid-calibration'):assert not s['writes']
 parent_writes=[v for addr,v in s['writes'] if addr==0x02002fc0]
 assert bool(parent_writes)==(variant=='normalized26-cold')
 assert not any(addr in (0x02002000,0x020020a0,0x02002518) or 0x03006000<=addr<0x03007000 or 0x03400000<=addr<0x03500000 or 0x02521000<=addr<0x02522000 for addr,v in s['writes'])
 (e/(variant+'.log')).write_text(log)
 cases.append({'name':variant,'passed':True,'exit':s['exit'],'configured_disabled':disabled,'sensor_success':ok,'mmio_writes':s['writes'],'modeled_elapsed_us':s['time']-1000000})
result={'passed':True,'physical_access':False,'elf_sha256':hashlib.sha256((e/'build/probe.elf').read_bytes()).hexdigest(),'scope':'Actual linked task constructor/client, counter conversion, native owner checks/register filters, common clock registry and register mutex, calibration/startup/read/stop instructions. FreeRTOS scheduling and queue primitives, architectural observations, time and MMIO effects modeled. No physical acquisition, scheduling, analog drain, electrical safety or ROM return evidence.','cases':cases}
(e/'check-result.json').write_text(json.dumps(result,indent=2)+'\n');print(f'{len(cases)} focused linked thermal cases passed')
