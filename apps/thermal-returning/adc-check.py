"""Focused linked client/provider execution; counter, task/queue and MMIO effects modeled."""
import argparse
from pathlib import Path
import hashlib,importlib.util,json,struct
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR,UC_ARM64_REG_PSTATE,UC_ARM64_REG_CPACR_EL1,UC_ARM64_REG_CP_REG
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--evidence',type=Path,required=True);a=p.parse_args();E=a.evidence
helper=E/'layout-helper.py'
spec=importlib.util.spec_from_file_location('layout',helper);layout=importlib.util.module_from_spec(spec);spec.loader.exec_module(layout)
image=layout.Image(E/'build/probe.elf')
required=['a733_pmic_native_init','a733_pmic_init','a733_pmic_poll','a733_pmic_result','a733_pmic_reap','a733_platform_lock_enter','a733_pins_acquire_banked','xQueueGetMutexHolder','xTaskGetSchedulerState']
for name in required:image.address(name)
cases=[]
for variant in ['normalized26-adopt','normalized26-cold','unsafe-direct26','wrong-cldo3','partial-sets','no-samples','inherited-enabled','baseline-refusal','address-nack-retains']:
 u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0x02000000,0x06000000);u.mem_map(0x40000000,0x04000000)
 for a,data in image.parts:
  if data:u.mem_write(a,data)
 u.reg_write(UC_ARM64_REG_PSTATE,5);u.reg_write(UC_ARM64_REG_CPACR_EL1,3<<20);u.reg_write(UC_ARM64_REG_CP_REG,(14,0,3,3,0,24000000))
 for name in ['ullCriticalNesting','ullPortInterruptNesting']:u.mem_write(image.address(name),bytes(8))
 state={'time':1000000,'exit':None,'phase':'idle','reg':None,'data':None,'next_mutex':0x43010000,'holders':{},'transactions':[],'writes':[],'adc_rounds':0,'adc_enabled':None,'adc_regs':{},'task':None}
 regs={0x07090160:0x983fb0f7,0x02002000:0xf8675f00,0x0701000c:0,0x07010010:0x04000000,0x0701019c:0x10001,0x07025000:0x22,0x07025014:0x11,0x07025024:0x5,0x0708300c:0x40,0x07083010:0xf8,0x07083014:0x28,0x07083020:0x30,0x07083200:0x00f81000,0x07083210:0x80c0,0x07083218:0x100010}
 regs.update({0x03400000:0x33,0x03400108:0,0x03400208:0,0x03400308:0,
 0x02002518:0x03000005,0x020020a0:0xff126310,0x020020a8:0,0x020020ac:0,
 0x02003908:0x0a000a00,0x02003988:0x0a000000,0x02002fc4:0,
 0x02002fc0:0x40 if variant=='normalized26-cold' else 0x81000040 if variant=='unsafe-direct26' else 0x80000040})
 state['adc_regs']={0x02521000+off:0 for off in (0,4,8,0xc,0x10,0x20,0x24,0x28,0x30,0x34,0x38,0x80,0x84,0x88,0x8c,0xd0,0xdc,0xfc)}
 state['adc_regs'].update({0x02521004:0x00810000 if variant=='inherited-enabled' else 0x00800000,0x0252100c:0x1f00,0x025210fc:1})
 regs.update(state['adc_regs'])
 if variant=='baseline-refusal':regs[0x07083210]=0
 for a,v in regs.items():u.mem_write(a,struct.pack('<I',v))
 names=['platform_init','xTaskCreateStatic','vTaskStartScheduler','platform_panic','xTaskGetCurrentTaskHandle','xTaskGetSchedulerState','xQueueCreateMutexStatic','xQueueGetMutexHolder','xQueueSemaphoreTake','xQueueGenericSend','vTaskDelay','platform_exit']
 hooks={image.address(n):n for n in names}
 def read(a,n=8):return int.from_bytes(u.mem_read(a,n),'little')
 def write(a,v,n=8):u.mem_write(a,int(v).to_bytes(n,'little'))
 def ret(v=0):u.reg_write(UC_ARM64_REG_X0,v);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
 def code(cpu,a,size,cookie):
  word=int.from_bytes(u.mem_read(a,4),'little');kind=word&0xffffffe0;reg=word&31
  if kind in (0xd5384240,0xd53be000,0xd53be020):
   v=4 if kind==0xd5384240 else 24000000 if kind==0xd53be000 else state['time']*24
   if kind==0xd53be020:state['time']+=1
   u.reg_write(UC_ARM64_REG_X0+reg,v);u.reg_write(UC_ARM64_REG_PC,a+4);return
  name=hooks.get(a)
  if name is None:return
  args=[u.reg_read(UC_ARM64_REG_X0+i) for i in range(8)]
  if name=='platform_init':ret()
  elif name=='xTaskCreateStatic':
   assert args[0]==image.address('probe',unit='/apps/thermal-returning/adc.c') and args[2:5]==[4096,0,3]
   state['task']=args[0];ret(0x43000000)
  elif name=='vTaskStartScheduler':u.reg_write(UC_ARM64_REG_PC,state['task']);u.reg_write(UC_ARM64_REG_X0,0)
  elif name=='platform_panic':raise AssertionError('unexpected panic')
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
  if a in (0x02002fc0,0x02002fc4) or 0x02521000<=a<0x02522000:
   assert any(state['holders'].values()) and size==4
   if a==0x02521038:state['adc_regs'][a]&=~value;state['adc_rounds']+=1
   elif a in state['adc_regs']:
    state['adc_regs'][a]=value
    if a==0x02521004 and value&0x10000:state['adc_enabled']=state['time']
   return
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
    status=0x58;state['phase']='received';values={0xff:0,0x0e:3,0x0f:0,0x21:0x20,0x31:12 if variant=='wrong-cldo3' else 13};write(0x07083008,values[state['reg']],4)
   else:raise AssertionError(phase)
  else:raise AssertionError(hex(value))
  # Store pending controller effects in the read callback: Unicorn completes
  # the intercepted CPU write after this callback returns.
  state['control']=0x40 if status==0xf8 else 0x48
  write(0x07083010,status,4)
 def mmio_read(cpu,access,a,size,value,cookie):
  if a==0x03400208:
   write(a,0x40 if state.get('adc_irq_pending') else 0,4)
  if a in state['adc_regs']:
   assert any(state['holders'].values()) and read(0x02002fc4,4)&0x10001==0x10001
   if a==0x02521038 and state['adc_regs'][0x02521004]&0x10000 and state['adc_enabled'] is not None:
    if state['time']-state['adc_enabled']>1000*(state['adc_rounds']+1) and variant!='no-samples':
     state['adc_regs'][a]|=(1<<(state['adc_rounds']%4)) if variant=='partial-sets' else 15
     if state['adc_regs'][0x02521028]&state['adc_regs'][a]:state['adc_irq_pending']=True
   if 0x02521080<=a<=0x0252108c:
    v=0 if a==0x02521080 else 1024+(a-0x02521080)*101+state['adc_rounds'];write(a,v,4)
   else:write(a,state['adc_regs'][a],4)
  if a==0x0708300c and 'control' in state:write(a,state['control'],4)
 u.hook_add(UC_HOOK_CODE,code);u.hook_add(UC_HOOK_MEM_WRITE,mmio_write,begin=0x02000000,end=0x07ffffff);u.hook_add(UC_HOOK_MEM_READ,mmio_read,begin=0x02000000,end=0x07ffffff)
 u.reg_write(UC_ARM64_REG_SP,0x43ffe000);u.reg_write(UC_ARM64_REG_LR,0x43fff000)
 u.emu_start(image.address('kernel_main'),0,count=12000000)
 text=bytes(u.mem_read(0x42000100,read(0x42000018)) ).decode();(E/(variant+'.log')).write_text(text)
 pl=image.layouts['a733_pmic'];p=image.address('provider',unit=None)
 retained=read(p+pl['retained'],4);held=read(p+pl['held'],4)
 assert len(text)<16127
 success=variant in ('normalized26-adopt','normalized26-cold','partial-sets')
 disabled=success or variant=='no-samples'
 assert state['exit']==(0 if disabled else 1),(variant,text)
 assert ('PASS native GPADC raw observations' in text)==success,(variant,text)
 assert ('COMPLETE native ADC configured disable' in text)==disabled,(variant,text)
 if variant not in ('baseline-refusal','address-nack-retains'):
  assert state['transactions']==[0xff,0x0e,0x0f,0x21,0x31,0xff],state['transactions']
  assert not retained and not held
 if disabled:
  assert not any(state['holders'].values())
  assert not state['adc_regs'][0x02521004]&0x10000 and state['adc_regs'][0x02521008]==state['adc_regs'][0x02521028]==0
  assert f'ADC_OUTCOME=0x{0 if success else 1:016x}' in text
  assert 'ADC_RESOURCES=RETAINED_CLOCKS_CLDO3_PMIC_PINS_CONFIGURATION' in text
 if success:assert state.get('adc_irq_pending') and 'ADC_RETAINED_GIC_PENDING=0x0000000000000040' in text and 'ADC_SETS=0x0000000000000002' in text and text.count('ADC_RAW=')==8 and 'ADC_RAW=0x0000000000000000' in text
 if variant in ('wrong-cldo3','baseline-refusal','address-nack-retains'):
  assert not any(0x02521000<=a<0x02522000 or a in (0x02002fc0,0x02002fc4) for a,v in state['writes'])
 assert bool([a for a,v in state['writes'] if a==0x02002fc0])==(variant=='normalized26-cold')
 cases.append({'name':variant,'passed':True,'exit':state['exit'],'transactions':state['transactions'],'mmio_writes':state['writes'],'provider_retained':retained,'provider_held':held,'log_bytes':len(text),'elapsed_modeled_us':state['time']-1000000})
result={'passed':True,'scope':'Actual linked finite client, PMIC/native adapter, platform-lock context checks, clock/rail/pin managers and I2C state machine. Executes counter conversion and ADC native/core start/read/stop; models architectural observations, FreeRTOS task/queue calls and device effects; does not establish physical pin behavior, scheduling, return or accelerator operation. No provider or ownership callback is replaced.','elf_sha256':hashlib.sha256((E/'build/probe.elf').read_bytes()).hexdigest(),'checker_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'layout_helper_sha256':hashlib.sha256(helper.read_bytes()).hexdigest(),'cases':cases}
(E/'check-result.json').write_text(json.dumps(result,indent=2)+'\n');print(f'{len(cases)} focused linked ADC/provider cases passed')
