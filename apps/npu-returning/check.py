"""Focused linked client/provider execution; counter, task/queue and MMIO effects modeled."""
from pathlib import Path
import hashlib,importlib.util,json,struct
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR,UC_ARM64_REG_PSTATE,UC_ARM64_REG_CPACR_EL1,UC_ARM64_REG_CP_REG
import argparse
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--evidence',type=Path,required=True);a=p.parse_args();E=a.evidence
helper=E/'layout-helper.py'
spec=importlib.util.spec_from_file_location('layout',helper);layout=importlib.util.module_from_spec(spec);spec.loader.exec_module(layout)
image=layout.Image(E/'build/probe.elf')
required=['a733_pmic_native_init','a733_pmic_init','a733_pmic_poll','a733_pmic_result','a733_pmic_reap','a733_platform_lock_enter','a733_pins_acquire_banked','xQueueGetMutexHolder','xTaskGetSchedulerState']
for name in required:image.address(name)
cases=[]
for variant in ['complete-snapshot','wrong-voltage','changed-voltage','disabled-supply','baseline-refusal','address-nack-retains']:
 u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0x02000000,0x06000000);u.mem_map(0x40000000,0x04000000)
 for a,data in image.parts:
  if data:u.mem_write(a,data)
 u.reg_write(UC_ARM64_REG_PSTATE,5);u.reg_write(UC_ARM64_REG_CPACR_EL1,3<<20);u.reg_write(UC_ARM64_REG_CP_REG,(14,0,3,3,0,24000000))
 for name in ['ullCriticalNesting','ullPortInterruptNesting']:u.mem_write(image.address(name),bytes(8))
 state={'time':1000000,'exit':None,'phase':'idle','reg':None,'data':None,'next_mutex':0x43010000,'holders':{},'transactions':[],'writes':[]}
 regs={0x07090160:0x10000002,0x02002000:0xf8636300,0x0701000c:0,0x07010010:0x04000000,0x0701019c:0x10001,0x07025000:0x22,0x07025014:0x11,0x07025024:0x5,0x0708300c:0x40,0x07083010:0xf8,0x07083014:0x28,0x07083020:0x30,0x07083200:0x00f81000,0x07083210:0x80c0,0x07083218:0x100010}
 if variant=='baseline-refusal':regs[0x07083210]=0
 for a,v in regs.items():u.mem_write(a,struct.pack('<I',v))
 names=['a733_counter_clock_init','a733_counter_clock_now','xTaskGetCurrentTaskHandle','xTaskGetSchedulerState','xQueueCreateMutexStatic','xQueueGetMutexHolder','xQueueSemaphoreTake','xQueueGenericSend','vTaskDelay','platform_exit']
 hooks={image.address(n):n for n in names}
 def read(a,n=8):return int.from_bytes(u.mem_read(a,n),'little')
 def write(a,v,n=8):u.mem_write(a,int(v).to_bytes(n,'little'))
 def ret(v=0):u.reg_write(UC_ARM64_REG_X0,v);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
 def code(cpu,a,size,cookie):
  name=hooks.get(a)
  if name is None:return
  args=[u.reg_read(UC_ARM64_REG_X0+i) for i in range(4)]
  if name=='a733_counter_clock_init':ret(1)
  elif name=='a733_counter_clock_now':write(args[1],state['time']);ret(1)
  elif name=='xTaskGetCurrentTaskHandle':ret(0x43000000)
  elif name=='xTaskGetSchedulerState':ret(2)
  elif name=='xQueueCreateMutexStatic':
   q=state['next_mutex'];state['next_mutex']+=0x100;state['holders'][q]=0;ret(q)
  elif name=='xQueueGetMutexHolder':ret(state['holders'][args[0]])
  elif name=='xQueueSemaphoreTake':
   assert state['holders'][args[0]]==0;state['holders'][args[0]]=0x43000000;ret(1)
  elif name=='xQueueGenericSend':
   assert state['holders'][args[0]]==0x43000000;state['holders'][args[0]]=0;ret(1)
  elif name=='vTaskDelay':state['time']+=1000*args[0];ret()
  elif name=='platform_exit':state['exit']=args[0];u.emu_stop()
 def mmio_write(cpu,access,a,size,value,cookie):
  state['writes'].append([a,value])
  assert a in [0x07025000,0x07083008,0x0708300c],hex(a)
  if a==0x07083008:state['data']=value
  if a!=0x0708300c:return
  phase=state['phase']
  if value==0x68:
   status=8 if phase=='idle' else 0x10;state['phase']='start' if phase=='idle' else 'restart'
  elif value==0x58:status=0xf8;state['phase']='idle'
  elif value==0x48:
   if phase=='start':
    assert state['data']==0x6c;status=0x20 if variant=='address-nack-retains' else 0x18;state['phase']='address'
   elif phase=='address':state['reg']=state['data'];state['transactions'].append(state['reg']);status=0x28;state['phase']='register'
   elif phase=='restart':assert state['data']==0x6d;status=0x40;state['phase']='read-address'
   elif phase=='read-address':
    status=0x58;state['phase']='received';values={0xff:0,0x0e:3,0x0f:0,0x10:(0xfd if variant=='disabled-supply' else 0xff),0x13:(31 if variant=='wrong-voltage' or (variant=='changed-voltage' and len(state['transactions'])>6) else 30)};write(0x07083008,values[state['reg']],4)
   else:raise AssertionError(phase)
  else:raise AssertionError(hex(value))
  # Store pending controller effects in the read callback: Unicorn completes
  # the intercepted CPU write after this callback returns.
  state['control']=0x40 if status==0xf8 else 0x48
  write(0x07083010,status,4)
 def mmio_read(cpu,access,a,size,value,cookie):
  if a==0x0708300c and 'control' in state:write(a,state['control'],4)
 u.hook_add(UC_HOOK_CODE,code);u.hook_add(UC_HOOK_MEM_WRITE,mmio_write,begin=0x02000000,end=0x07ffffff);u.hook_add(UC_HOOK_MEM_READ,mmio_read,begin=0x02000000,end=0x07ffffff)
 u.reg_write(UC_ARM64_REG_SP,0x43ffe000);u.reg_write(UC_ARM64_REG_LR,0x43fff000)
 u.emu_start(image.address('probe',unit='/apps/npu-returning/main.c'),0,count=2000000)
 text=bytes(u.mem_read(0x42000100,read(0x42000018)) ).decode();(E/(variant+'.log')).write_text(text)
 pl=image.layouts['a733_pmic'];p=image.address('provider',unit=None)
 retained=read(p+pl['retained'],4);held=read(p+pl['held'],4)
 assert len(text)<16127
 if variant in ('complete-snapshot','wrong-voltage','changed-voltage','disabled-supply'):
  assert state['exit']==0,text
  assert state['transactions']==[0xff,0x0e,0x0f,0x10,0x13,0xff]*2
  assert not retained and not held and not any(state['holders'].values())
  assert [v for a,v in state['writes'] if a==0x07025000]==[0x2f,0xff,0xf2,0x22]
  assert 'COMPLETE native NPU supply observation' in text
  assert ('PASS native NPU supply configured profile and request cleanup' in text)==(variant=='complete-snapshot')
  assert ('NPU_SUPPLY_OUTCOME=0x0000000000000000' in text)==(variant=='complete-snapshot')
  assert state['time']-1000000>=5160
  assert 'NPU_SUPPLY_SAMPLE_COUNT=0x0000000000000002' in text
 else:
  assert state['exit']==1,text
  assert retained and held and any(state['holders'].values())
  if variant=='baseline-refusal':assert not state['writes'] and not state['transactions']
  else:assert not state['transactions'] and state['writes'][-1]==[0x0708300c,0x48]
 cases.append({'name':variant,'passed':True,'exit':state['exit'],'transactions':state['transactions'],'mmio_writes':state['writes'],'provider_retained':retained,'provider_held':held,'log_bytes':len(text),'elapsed_modeled_us':state['time']-1000000})
result={'passed':True,'scope':'Actual linked finite client, PMIC/native adapter, platform-lock context checks, clock/rail/pin managers and I2C state machine. Models counter sampling, FreeRTOS task/queue calls and device effects; does not establish physical pin behavior, scheduling, return or accelerator operation. No provider or ownership callback is replaced. No NPU aperture access, DMA, PLL or power-domain operation is admitted.','elf_sha256':hashlib.sha256((E/'build/probe.elf').read_bytes()).hexdigest(),'checker_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'layout_helper_sha256':hashlib.sha256(helper.read_bytes()).hexdigest(),'cases':cases}
(E/'check-result.json').write_text(json.dumps(result,indent=2)+'\n');print('Six focused NPU supply cases passed')
