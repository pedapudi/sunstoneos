#!/usr/bin/env python3
"""Execute linked finite watchdog MMIO and breadcrumb vectors with modeled registers."""
import argparse,importlib.util,json,struct,io
from pathlib import Path
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--native',type=Path,required=True);p.add_argument('--evidence',type=Path,required=True);a=p.parse_args();a.evidence.mkdir(parents=True,exist_ok=True)
r=Path(__file__).resolve().parents[2]
s=importlib.util.spec_from_file_location('npu_watchdog_harness',r/'checks/harness.py');h=importlib.util.module_from_spec(s);s.loader.exec_module(h);im=h.Image(a.native/'image.elf',layout_names=('a733_observe_identity',))
s=importlib.util.spec_from_file_location('npu_watchdog_host',r/'apps/npu-finite/host.py');host=importlib.util.module_from_spec(s);s.loader.exec_module(host)
data=(a.native/'image.elf').read_bytes()
assert host.watchdog_marker(data)=={'mode':0xb1,'session_seconds':20}
from elftools.elf.elffile import ELFFile
elf=ELFFile(io.BytesIO(data));marker=next(x for x in elf.get_section_by_name('.symtab').iter_symbols() if x.name=='os_cpu_return_watchdog');section=elf.get_section(marker['st_shndx']);offset=section['sh_offset']+marker['st_value']-section['sh_addr']
for index in range(5):
 corrupted=bytearray(data);struct.pack_into('<Q',corrupted,offset+8*index,0)
 try:host.watchdog_marker(bytes(corrupted))
 except ValueError:pass
 else:raise AssertionError(('marker-field-admitted',index))
def machine():
 u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0x02000000,0x07000000);u.mem_map(0x40000000,0x04000000)
 for at,data in im.parts:
  if data:u.mem_write(at,data)
 u.reg_write(UC_ARM64_REG_SP,0x43ffe000);u.reg_write(UC_ARM64_REG_LR,0x43fff000)
 return u
def put(u,at,v,n=4):u.mem_write(at,int(v).to_bytes(n,'little'))
def get(u,at,n=4):return int.from_bytes(u.mem_read(at,n),'little')
results=[]
for case in ('admit','wrong-mode','wrong-config','wrong-output','refresh','failed','deadline','disarm','stop-armed','stop-disabled'):
 u=machine();writes=[]
 for at,v in ((0x02050014,0xb1),(0x02050010,1),(0x02050018,2)):put(u,at,v)
 for label,at in (('wrong-mode',0x02050014),('wrong-config',0x02050010),('wrong-output',0x02050018),('stop-disabled',0x02050014)):
  if case==label:put(u,at,0)
 def write(cpu,access,at,size,value,cookie):writes.append((at,value))
 u.hook_add(UC_HOOK_MEM_WRITE,write)
 if case in ('refresh','failed','deadline'):
  name='npu_finite_watchdog_refresh'
  for i,v in enumerate((100 if case=='deadline' else 1,100,int(case=='failed'))):u.reg_write(UC_ARM64_REG_X0+i,v)
 elif case=='disarm':name='npu_finite_watchdog_disarm'
 elif case.startswith('stop-'):name='npu_finite_watchdog_stop'
 else:name='npu_finite_watchdog_admitted'
 u.emu_start(im.address(name),0x43fff000,count=1000)
 if case in ('admit','wrong-mode','wrong-config','wrong-output'):assert u.reg_read(UC_ARM64_REG_X0)==int(case=='admit')
 if case=='refresh':assert writes==[(0x0205000c,0x14af)]
 if case in ('failed','deadline','stop-disabled'):assert writes==[]
 if case=='disarm':assert writes==[(0x02050014,0x16aa0000)] and u.reg_read(UC_ARM64_REG_X0)==0
 if case=='stop-armed':assert writes==[(0x02050014,0x16aa0001),(0x0205000c,0x14af)]
 results.append(case)
for case in ('stage','first-failure','failure-preserved','other-SVC','EL1-fault','EL1-fault-preserved')+tuple(f'EL1-slot-{n}' for n in (2,3,6,7,8,9,10,11,12,13,14,15)):
 u=machine();state={'stop':None};svc=0x1234 if case=='other-SVC' else 0x5c0d if case=='stage' else 0x5c0e
 before=[0x11110000+i for i in range(31)];before[0]=0x0601 if case=='stage' else 0xabcdef01;before[1]=0x12345678;before[2]=0x41424344
 for i,v in enumerate(before):u.reg_write(UC_ARM64_REG_X0+i,v)
 if case in ('failure-preserved','EL1-fault-preserved'):put(u,0x07090110,0x12345678)
 def code(cpu,at,size,cookie):
  word=get(u,at);kind=word&0xffffffe0;reg=word&31
  values={0xd5385200:0x15<<26|svc,0xd53800a0:0,0xd53be020:0x100000,0xd5384020:0x41234567,0xd5386000:0x416789ab,0xd5384000:0}
  if kind in values:u.reg_write(UC_ARM64_REG_X0+reg,values[kind]);u.reg_write(UC_ARM64_REG_PC,at+4);return
  if word==0xd69f03e0:state['stop']='eret';u.emu_stop()
  if at==im.address('FreeRTOS_SWI_Handler'):state['stop']='freertos';u.emu_stop()
  if at==im.address('platform_exception'):state['stop']='fault';u.emu_stop()
 u.hook_add(UC_HOOK_CODE,code)
 entry=im.address('_freertos_vector_table')+128*int(case.rsplit('-',1)[1]) if case.startswith('EL1-slot-') else im.address('npu_finite_el1_fault' if case.startswith('EL1-') else 'npu_finite_svc')
 u.emu_start(entry,0,count=1000)
 assert [u.reg_read(UC_ARM64_REG_X0+i) for i in range(31)]==before,case
 assert get(u,0x07090108)==0
 if case=='stage':assert get(u,0x07090104)==0x06015678 and state['stop']=='eret'
 elif case=='first-failure':assert get(u,0x07090110)==0xfa0b0000 and get(u,0x07090114)==before[0] and get(u,0x07090118)==before[2] and get(u,0x0709011c)==before[1]
 elif case in ('failure-preserved','EL1-fault-preserved'):assert get(u,0x07090110)==0x12345678
 elif case=='other-SVC':assert state['stop']=='freertos'
 elif case.startswith('EL1-slot-'):assert get(u,0x07090110)==(0xfa010001|int(case.rsplit('-',1)[1])<<8)
 elif case=='EL1-fault':assert get(u,0x07090110)==0xfa010001 and get(u,0x07090118)==0x41234567
 results.append(case)
# Enter real EL3 vector slots and preserve the first full fault record.
for slot in range(16):
 u=machine();state={'stopped':False}
 u.reg_write(UC_ARM64_REG_X0,0xdead)
 def el3_code(cpu,at,size,cookie):
  word=get(u,at);kind=word&0xffffffe0;reg=word&31
  if at==im.address('a733_smp_stop'):state['stopped']=True;u.emu_stop();return
  values={0xd53e5200:0x96000000,0xd53e4020:0x41012345,0xd53e6000:0x412abcde,0xd53800a0:0,0xd5381000:0,0xd53be020:1}
  if kind in values:u.reg_write(UC_ARM64_REG_X0+reg,values[kind]);u.reg_write(UC_ARM64_REG_PC,at+4);return
  if word&0xfff00000==0xd5100000 or word&0xfffff000==0xd5033000:u.reg_write(UC_ARM64_REG_PC,at+4)
 u.hook_add(UC_HOOK_CODE,el3_code)
 u.emu_start(im.address('sd_single_core_vectors')+128*slot,0,count=2000)
 assert state['stopped'] and get(u,0x07090110)==(0xfa0300ad|slot<<8),slot
 assert get(u,0x07090114)==0x96000000 and get(u,0x07090118)==0x41012345 and get(u,0x0709011c)==0x412abcde
 assert get(u,0x07090108)==0
 original=bytes(u.mem_read(0x07090110,16))
 u.reg_write(UC_ARM64_REG_SP,0x43ffe000);u.reg_write(UC_ARM64_REG_X0,0xbeef)
 u.emu_start(im.address('sd_single_core_vectors')+128*slot,0,count=2000)
 assert bytes(u.mem_read(0x07090110,16))==original
 results.append('EL3-slot-'+str(slot))
# Execute the actual supervisor until its first failure/deadline suspension.
for case in ('supervisor-failure','supervisor-deadline'):
 u=machine();state={'delays':0,'restart':0,'stopped':False,'time':1}
 for address,value in ((0x02050014,0xb1),(0x02050010,1),(0x02050018,2)):
  put(u,address,value)
 put(u,im.address('a733_observe_identity')+im.layouts['a733_observe_identity']['deadline'],100,8)
 def return_value(value=0):
  u.reg_write(UC_ARM64_REG_X0,value);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
 def supervisor_code(cpu,at,size,cookie):
  if at==im.address('xTaskGetTickCount'):return_value(123);return
  if at==im.address('a733_smp_counter'):return_value(state['time']);return
  if at==im.address('xTaskDelayUntil'):
   assert u.reg_read(UC_ARM64_REG_X0+1)==20
   assert get(u,u.reg_read(UC_ARM64_REG_X0))==123
   state['delays']+=1
   if case=='supervisor-failure':put(u,im.address('a733_npu_application_failed'),1)
   else:state['time']=100
   return_value();return
  if at==im.address('vTaskSuspend'):
   assert u.reg_read(UC_ARM64_REG_X0)==0
   state['stopped']=True;u.emu_stop()
 def supervisor_write(cpu,access,at,size,value,cookie):
  if at==0x0205000c:assert value==0x14af;state['restart']+=1
 u.hook_add(UC_HOOK_CODE,supervisor_code);u.hook_add(UC_HOOK_MEM_WRITE,supervisor_write)
 u.emu_start(im.address('supervisor',unit='apps/npu-finite/watchdog.c'),0,count=5000)
 assert state['stopped'] and state['delays']==state['restart']==1,state
 results.append(case)
(a.evidence/'results.json').write_text(json.dumps({'passed':True,'cases':results},indent=2)+'\n');print('PASS linked finite watchdog and vectors:',len(results),'cases')
