#!/usr/bin/env python3
"""Actual ADC owner/core/native MMIO/USB instructions; analog/GIC/providers modeled."""
import argparse,hashlib,importlib.util,json,struct
from pathlib import Path
from unicorn import UC_HOOK_MEM_WRITE,UC_HOOK_MEM_READ
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
 spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
base=module('adc_compare_base',ROOT/'checks/adc-application-native.py')
class Machine(base.Machine):
 def __init__(self,image,session,variant,nominal):
  super().__init__(image,session,variant,nominal)
  for a in (0x02002000,0x03400000,0x07090000):self.cpu.mem_map(a,0x4000 if a==0x02002000 else 0x1000)
  self.w1c=[];self.io=[];self.stop_masks=[]
  for name in ('a733_adc_poll','a733_adc_read'):del self.hooks[image.address(name)]
  self.hooks[image.address('a733_clock_token_status')]='a733_clock_token_status'
  for a,v in ((0x07090160,0x10000002),(0x02002000,0xf8636300),(0x02002fc0,0x81000000),(0x02521004,0x00800000),(0x0252100c,0x1f00),(0x025210fc,1)):
   self.cpu.mem_write(a,struct.pack('<I',v))
  self.cpu.hook_add(UC_HOOK_MEM_WRITE,self.write,begin=0x02521000,end=0x02521fff)
  self.cpu.hook_add(UC_HOOK_MEM_READ,self.read,begin=0x02521000,end=0x02521fff)
 def read(self,cpu,access,a,size,value,unused):self.io.append(['read',a])
 def write(self,cpu,access,a,size,value,unused):
  assert size==4;self.io.append(['write',a,value])
  if a in (0x02521030,0x02521034,0x02521038):self.w1c.append((a,self.reg(a)&~value))
 def reg(self,a):return int.from_bytes(self.cpu.mem_read(a,4),'little')
 def code(self,cpu,a,size,unused):
  for address,value in self.w1c:self.cpu.mem_write(address,struct.pack('<I',value))
  self.w1c=[]
  if a==self.image.address('a733_clock_token_status'):self.returned(0);return
  if a==self.image.address('a733_adc_stop'):self.stop_masks.append(self.field('masked'))
  super().code(cpu,a,size,unused)
 def conversion(self,data,low,high):
  for a,v in ((0x02521038,data),(0x02521030,low),(0x02521034,high)):
   self.cpu.mem_write(a,struct.pack('<I',v))
  for i in range(4):self.cpu.mem_write(0x02521080+4*i,struct.pack('<I',1024*i))
 def step(self):self.time+=25000;return self.call('adc_step')
def run(image,session_path,nominal_path,out):
 out.mkdir(parents=True,exist_ok=False);elf=base.Image(image);session=session_path.read_bytes();nominal=json.loads(nominal_path.read_text());decode=module('adc_compare_decoder',ROOT/'apps/adc/observe.py').decode
 assert struct.unpack_from('<Q',session,600)[0]==3 and session[770]==5
 results={}
 for variant in ('success','no-events','changed-window','unselected-status','stop-failure','cancelled','deadline'):
  m=Machine(elf,session,variant,nominal);assert m.call('adc_construct')==1
  for i in range(15):
   if m.field('stage')==8:break
   assert m.step()==0
  else:raise AssertionError('startup unbounded')
  low,high=struct.unpack_from('<HH',session,772);window=high<<16|low
  assert m.reg(0x02521040)==window and m.reg(0x02521048)==window
  assert m.reg(0x02521044)==0 and m.reg(0x0252104c)==0
  assert m.reg(0x02521008)==0x5000f and m.reg(0x02521020)==m.reg(0x02521024)==5
  # Immutable native selection and selected register policy, using real wrappers.
  m.held=True;scratch=0x50002000
  assert m.call('a733_adc_native_thresholds',m.native,m.driver+elf.layouts['a733_adc']['thresholds'])==0
  for address,value in ((0x02521044,window),(0x02521040,window^1),(0x02521040,window|0x1000),(0x02521008,0x1000f),(0x02521020,1),(0x02521024,16),(0x02521030,2),(0x02521034,16),(0x025210fc,0)):
   before=len(m.io);assert m.call('write_register',m.base,address,value,unit='/apps/adc/native.c')==0
   assert not any(x[0]=='write' for x in m.io[before:])
  assert m.call('read_register',m.base,0x02521044,scratch,unit='/apps/adc/native.c')==0;m.held=False
  # Threshold-only service must consume and retain masks while no raw set completes.
  m.conversion(0,0 if variant=='no-events' else 1,0 if variant=='no-events' else 4);assert m.step()==0
  assert m.field('sets')==0 and m.field('low_seen')==(0 if variant=='no-events' else 1)
  assert m.reg(0x02521030)==m.reg(0x02521034)==0
  if variant=='changed-window':m.cpu.mem_write(0x02521040,struct.pack('<I',window^1))
  if variant=='unselected-status':m.conversion(15,2,0)
  else:m.conversion(15,0,0)
  if variant=='cancelled':m.live=False
  if variant=='deadline':m.time=60990000
  r=m.step()
  failed=variant in ('changed-window','unselected-status','cancelled','deadline')
  if not failed:
   assert r==0 and m.field('sets')==1;m.conversion(15,0,0 if variant=='no-events' else 1);assert m.step()==0 and m.field('sets')==2
   if variant=='stop-failure':m.cpu.mem_write(0x02002fc0,struct.pack('<I',0x82000000));failed=True
   for i in range(5):
    r=m.step()
    if r in (1,0xffffffff):break
   else:raise AssertionError('completion unbounded')
  if failed:
   assert r==0xffffffff and m.field('result')==2
   before=len(m.io);m.step();assert len(m.io)==before
   if variant=='unselected-status':assert m.reg(0x02521030)==2
  else:
   assert r==1 and m.stop_masks==[1]
   assert not any(m.reg(a) for a in (0x02521008,0x02521020,0x02521024,0x02521028)) and not m.reg(0x02521004)&0x10000
   m.call('board_usb0_task');frame=bytes(m.cpu.mem_read(elf.address('adc_response'),288));result=decode(frame,session)
   assert result['compare_window']['low_seen_mask']==(0 if variant=='no-events' else 1)
   assert result['compare_window']['high_seen_mask']==(0 if variant=='no-events' else 5)
   if variant=='success':(out/'native-frame.bin').write_bytes(frame)
  results[variant]=dict(passed=True,mmio_reads=sum(x[0]=='read' for x in m.io),mmio_writes=sum(x[0]=='write' for x in m.io),stop_masks=m.stop_masks)
 (out/'result.json').write_text(json.dumps(dict(status='pass',cases=results,physical_access=False,scope=__doc__),indent=2)+'\n')
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__)
 for name in ('image','session','nominal','output'):p.add_argument('--'+name,type=Path,required=True)
 a=p.parse_args();run(a.image,a.session,a.nominal,a.output)
