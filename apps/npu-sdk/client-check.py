#!/usr/bin/env python3
"""Run the linked SDK application through its actual public mailbox API; model backend jobs and provider parking."""
import argparse,hashlib,json,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'checks'))
from harness import Image
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--image',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args();a.out.mkdir(parents=True,exist_ok=False)
im=Image(a.image,layout_names=('npu_runtime','npu_runtime_slot','npu_runtime_counters','npu_service','a733_npu_controller','a733_observe_identity','npu_finite_network','ss_npu_backend'))
L=im.layouts;enums={}
with a.image.open('rb') as f:
 for cu in ELFFile(f).get_dwarf_info().iter_CUs():
  for die in cu.iter_DIEs():
   if die.tag=='DW_TAG_enumerator':enums[die.attributes['DW_AT_name'].value.decode()]=int(die.attributes['DW_AT_const_value'].value)
rows=[]
for variant in ('matched','last-output-mismatch','last-IRQ-missing','last-job-failed','close-failed'):
 u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0x40000000,0x04000000)
 for at,data in im.parts:
  if data:u.mem_write(at,data)
 def put(at,v,n=8):u.mem_write(at,int(v).to_bytes(n,'little'))
 def get(at,n=8):return int.from_bytes(u.mem_read(at,n),'little')
 def ret(v=0):u.reg_write(UC_ARM64_REG_X0,v);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
 profile=im.address('npu_startup_return_profile');jobs=get(profile+16);rt=0x43000000
 service=rt+L['npu_runtime']['service'];slot=rt+L['npu_runtime']['slot'];controller=service+L['npu_service']['controller']
 put(im.address('instance'),service);put(service+L['npu_service']['irq_bound'],1,4);put(service+L['npu_service']['irq_completions'],1,4)
 put(im.address('a733_observe_identity')+L['a733_observe_identity']['deadline'],480000000)
 state={'pump':None,'ticks':24000000,'models':[],'exit':None,'retained':False,'closed':False,'log':''}
 names=('a733_npu_application_ready','a733_npu_application_runtime','npu_job_run','npu_job_close','a733_npu_application_stopped','a733_npu_application_quiesce','a733_npu_application_quiescent','a733_npu_application_fail','vTaskDelay','vTaskSuspend','vPortEnterCritical','platform_exit','platform_puts','platform_hex')
 hooks={im.address(n):n for n in names}
 def code(cpu,at,size,cookie):
  word=get(at,4)
  if word&0xffe0001f==0xd4000001:
   assert (word>>5)&0xffff in (0x5c0d,0x5c0e);u.reg_write(UC_ARM64_REG_PC,at+4);return
  if word&0xffffffe0==0xd53be020:
   u.reg_write(UC_ARM64_REG_X0+(word&31),state['ticks']);state['ticks']+=24;u.reg_write(UC_ARM64_REG_PC,at+4);return
  if at==0x43fff100:
   saved,continuation=state['pump'];state['pump']=None
   u.context_restore(saved);u.reg_write(UC_ARM64_REG_PC,continuation);return
  name=hooks.get(at)
  if name is None:return
  args=[u.reg_read(UC_ARM64_REG_X0+i) for i in range(6)]
  if name=='vTaskDelay':
   assert state['pump'] is None
   state['pump']=(u.context_save(),u.reg_read(UC_ARM64_REG_LR))
   u.reg_write(UC_ARM64_REG_SP,0x43ffd000);u.reg_write(UC_ARM64_REG_LR,0x43fff100)
   u.reg_write(UC_ARM64_REG_PC,im.address('ss_npu_service_step'));return
  if name=='a733_npu_application_ready':ret(1)
  elif name=='a733_npu_application_runtime':ret(rt)
  elif name=='npu_job_run':
   n=args[0];assert n==len(state['models']) and n<jobs
   network=im.address('npu_finite_networks')+n*48;fields=L['npu_finite_network']
   assert args[1:3]==[get(network+fields['input']),get(network+fields['input_bytes'])]
   assert args[4]==get(network+fields['reference_bytes']) and args[5]==1000000
   state['models'].append(n)
   if variant=='last-job-failed' and n==jobs-1:ret(-1 & ((1<<64)-1));return
   output=bytes(u.mem_read(get(network+fields['reference']),args[4]))
   if variant=='last-output-mismatch' and n==jobs-1:output=bytes([output[0]^1])+output[1:]
   u.mem_write(args[3],output)
   put(rt+L['npu_runtime']['counters']+L['npu_runtime_counters']['jobs'],n+1)
   put(service+L['npu_service']['irq_completions'],1+n+int(not(variant=='last-IRQ-missing' and n==jobs-1)),4)
   ret(1)
  elif name=='npu_job_close':
   if variant=='close-failed':ret(0);return
   state['closed']=True
   for field,value in [('opened',1),('closed',1),('state',enums['NPU_RUNTIME_CLOSED'])]:put(slot+L['npu_runtime_slot'][field],value,4)
   put(controller+L['a733_npu_controller']['state'],enums['A733_NPU_CLOSED'],4)
   put(service+L['npu_service']['irq_stopped'],1,4);ret(1)
  elif name in ('a733_npu_application_stopped','a733_npu_application_quiesce','a733_npu_application_quiescent'):ret(int(state['closed']))
  elif name=='platform_puts':
   text=bytearray();ptr=args[0]
   while (v:=bytes(u.mem_read(ptr,1)))!=b'\0':text.extend(v);ptr+=1
   state['log']+=text.decode();ret()
  elif name=='platform_hex':state['log']+=f'0x{args[0]:016x}';ret()
  elif name=='platform_exit':state['exit']=args[0];u.emu_stop()
  elif name=='vTaskSuspend':state['retained']=True;u.emu_stop()
  else:ret()
 u.hook_add(UC_HOOK_CODE,code)
 # Bind the actual mailbox to linked helper/close callbacks before starting
 # the application. Initialization of the service-ready publication is modeled.
 backend=0x43ffc000;fields=L['ss_npu_backend']
 put(backend+fields['run'],im.address('npu_job_run'));put(backend+fields['close'],im.address('sdk_close'))
 if 'models' in fields:
  put(backend+fields['models'],im.address('npu_finite_models'));put(backend+fields['count'],jobs,4)
 stop_hook=u.hook_add(UC_HOOK_CODE,lambda cpu,at,size,cookie:cpu.emu_stop() if at==0x43fff200 else None)
 u.reg_write(UC_ARM64_REG_X0,backend);u.reg_write(UC_ARM64_REG_SP,0x43ffb000);u.reg_write(UC_ARM64_REG_LR,0x43fff200)
 u.emu_start(im.address('ss_npu_service_bind'),0,count=10000)
 assert u.reg_read(UC_ARM64_REG_X0)==1
 u.hook_del(stop_hook)
 put(im.address('runtime'),rt);put(im.address('sdk_ready'),1,4)
 u.reg_write(UC_ARM64_REG_SP,0x43ffe000);u.reg_write(UC_ARM64_REG_LR,0x43fff000)
 u.emu_start(im.address('npu_runtime_client'),0,count=3000000)
 assert state['models']==list(range(jobs)),state
 if variant in ('matched','last-output-mismatch'):
  assert len(state['log'].encode())<=16127,'returned report would truncate'
  status=int(variant!='matched');assert state['exit']==status and not state['retained'],state
  assert f'NPU_INFERENCE={"MATCH" if not status else "MISMATCH"}; inference_jobs={jobs}\n' in state['log']
 else:assert state['retained'] and state['exit'] is None,state
 state.pop('pump',None)
 (a.out/(variant+'.log')).write_text(state['log']);rows.append({'case':variant,'passed':True,**state})
(a.out/'result.json').write_text(json.dumps({'passed':True,'image_sha256':hashlib.sha256(a.image.read_bytes()).hexdigest(),'cases':rows,'physical_access':False,'scope':__doc__},indent=2)+'\n');print('PASS linked SDK client: actual mailbox API, all jobs, final output mismatch, missing IRQ, job and close failures')
