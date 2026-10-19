#!/usr/bin/env python3
"""Only the native application's pre-PLL reference admission; no NPU work."""
import argparse,hashlib,importlib.util,json,struct
from pathlib import Path
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import *
p=argparse.ArgumentParser()
for n in ('image','session','output'):p.add_argument('--'+n,type=Path,required=True)
p.add_argument('--cases',nargs='+')
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False);root=Path(__file__).resolve().parents[1]
s=importlib.util.spec_from_file_location('npu_clock_image',root/'checks/storage-read-native.py');native=importlib.util.module_from_spec(s);s.loader.exec_module(native);e=native.Image(a.image)
rows=[]
for label,osc,ref,accepted in [('26MHz-normalized24',0x10008002,0xf8675f00,True),('24MHz-normalized24',0x10000002,0xf8636300,True),('26MHz-wrong-equal-factors',0x10008002,0xf8636300,False),('26MHz-unstable',0x00008002,0xf8675f00,False),('26MHz-reference-disabled',0x10008002,0x78675f00,False),('26MHz-PERI0-M1-refused',0x10008002,0xf8675f00,False)]:
 if a.cases and label not in a.cases:continue
 u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0x02000000,0x07000000);u.mem_map(0x40000000,0x04000000);u.reg_write(UC_ARM64_REG_PSTATE,5)
 for addr,data in e.parts:u.mem_write(addr,data)
 session=0x43000000;clock=0x43001000;registry=0x43003000;locks=0x43004000;counter=0x43006000;task=0x43007000;ret=0x43fff000
 raw=bytearray(a.session.read_bytes())
 boot=e.layouts['npu_session']['boot'];pll_offset=e.layouts['npu_session']['pll']
 config=boot+e.layouts['a733_npu_application_boot']['npu']
 fabric=(0x03000002,0xc3000000,0x81000000)
 for i,value in enumerate(fabric):
  struct.pack_into('<I',raw,pll_offset+e.layouts['a733_npu_pll_config']['consumers']+4*i,value)
  struct.pack_into('<I',raw,config+e.layouts['a733_npu_platform_config']['fabric']+4*i,value&(0x0300001f if i==0 else 0x8700001f))
 for i,value in enumerate((200000000,600000000,600000000)):
  struct.pack_into('<I',raw,config+e.layouts['a733_npu_platform_config']['rate_hz']+4*i,value)
 u.mem_write(session,bytes(raw));running=False;finished=[]
 def returned(value=1):u.reg_write(UC_ARM64_REG_X0,value);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
 hooks={e.address(n):n for n in ('a733_npu_application_health','xTaskGetSchedulerState','xTaskGetCurrentTaskHandle','a733_counter_clock_now','a733_platform_lock_enter','a733_platform_lock_leave','npu_session_get')}
 def hook(cpu,address,size,unused):
  if address==ret:finished.append(True);cpu.emu_stop();return
  n=hooks.get(address)
  if n is None:return
  if n=='xTaskGetSchedulerState':returned(2 if running else 1)
  elif n=='xTaskGetCurrentTaskHandle':returned(task)
  elif n=='a733_counter_clock_now':cpu.mem_write(cpu.reg_read(UC_ARM64_REG_X1),struct.pack('<Q',1000));returned()
  elif n=='npu_session_get':returned(session)
  else:returned()
 u.hook_add(UC_HOOK_CODE,hook)
 def call(name,*args):
  finished.clear();u.reg_write(UC_ARM64_REG_SP,0x43ffeff0);u.reg_write(UC_ARM64_REG_LR,ret)
  for i,v in enumerate(args):u.reg_write(UC_ARM64_REG_X0+i,v)
  u.emu_start(e.address(name),0,count=1000000);assert finished,name;return u.reg_read(UC_ARM64_REG_X0)
 assert call('a733_clock_leases_init',registry,e.address('a733_counter_clock_now'),counter)==1
 for field,value in [('initialized',1),('now',e.address('a733_counter_clock_now')),('context',counter)]:
  width=4 if field=='initialized' else 8
  u.mem_write(locks+e.layouts['a733_platform_locks'][field],value.to_bytes(width,'little'))
 assert call('npu_application_clock_init',clock,session,registry,locks,counter,task,1000000)==1
 # Recorded second-board PERI0 and200/600/600MHz fabric configuration.
 # These modeled values exercise the actual private parent callback, including
 # the observed PERI0 bit24 permitted by the canonical shared provider.
 for addr,value in [(0x07090160,osc),(0x02002000,ref),(0x020020a0,0xff126312 if label=='26MHz-PERI0-M1-refused' else 0xff126310),
                    (0x020020a8,0),(0x020020ac,0x30000000),
                    (0x02003908,0x8fff0fff),(0x02003988,0x0fff0000)]:
  u.mem_write(addr,struct.pack('<I',value))
 running=True
 result=call('a733_npu_pll_poll',clock)
 assert result==(2 if accepted else 3),(label,result)
 assert int.from_bytes(u.mem_read(clock+e.layouts['a733_npu_pll']['wrote'],4),'little')==0
 rows.append(dict(case=label,accepted=accepted,result=result))
(a.output/'result.json').write_text(json.dumps({'passed':True,'physical_access':False,'image_sha256':hashlib.sha256(a.image.read_bytes()).hexdigest(),'cases':rows,'scope':'Actual application acquisition/reference checks and real PLL/clock metadata. Scheduler/health/current task, counter, mutex and MMIO values modeled. No PLL write, startup replay, inference or physical frequency claim.'},indent=2)+'\n')
print('PASS application normalized reference admission',len(rows))
