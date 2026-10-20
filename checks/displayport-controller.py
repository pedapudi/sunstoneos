#!/usr/bin/env python3
"""Execute actual linked DP controller/PHY code with modeled MMIO and sink.

Native resource admission, sink replies, PHY readiness and elapsed time are
explicit models. No electrical link, display pixels or DMA drain is qualified.
"""
import argparse,copy,hashlib,importlib.util,json,pickle,struct
from pathlib import Path
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR
ROOT=Path(__file__).resolve().parents[1]
def load(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
base=load('dp_controller_image',ROOT/'checks/storage-read-native.py')
class Model:
 def __init__(self,image):
  self.image=image;self.u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);self.u.mem_map(0x41000000,0x03000000)
  for a,b in image.parts:
   if b:self.u.mem_write(a,b)
  self.time=100;self.registers={};self.events=[];self.transactions=[];self.mode='normal';self.pending=None;self.counter=0;self.fifo=[];self.code_reply=0
  self.pattern=0;self.cr=0;self.eq=0;self.edid_offset=0;self.callback_base=0x43f00000;self.obj=0x43e00000;self.cfg=0x43e01000
  self.dpcd=bytearray(16);self.dpcd[0]=0x14;self.dpcd[1]=0x14;self.dpcd[2]=0x84
  self.edid=bytearray(128);self.edid[:8]=bytes.fromhex('00ffffffffffff00');self.edid[18:21]=bytes([1,4,0x80]);p=bytearray(18)
  struct.pack_into('<H',p,0,7425);p[2:12]=bytes([0,114,0x51,208,30,0x20,110,40,0x55,0]);p[17]=0x1e;self.edid[54:72]=p;self.edid[127]=(-sum(self.edid))&255
  for i in range(7):self.u.hook_add(UC_HOOK_CODE,self.callback,begin=self.callback_base+i*16,end=self.callback_base+i*16)
  self.u.hook_add(UC_HOOK_CODE,lambda *unused:self.u.emu_stop(),begin=0x43fff000,end=0x43fff000)
  ol=image.layouts['a733_dp_ops'];cl=image.layouts['a733_dp_config'];cfg=bytearray(88)
  for i,name in enumerate(('admit','read','write','barrier','now','wait')):struct.pack_into('<Q',cfg,cl['ops']+ol[name],self.callback_base+i*16)
  struct.pack_into('<Q',cfg,cl['context'],1);struct.pack_into('<QQII',cfg,cl['owner'],9,10000000,1,200000000);self.u.mem_write(self.cfg,bytes(cfg))
 def put(self,a,v,n=8):self.u.mem_write(a,int(v).to_bytes(n,'little'))
 def get(self,a,n=8):return int.from_bytes(self.u.mem_read(a,n),'little')
 def ret(self,v):self.u.reg_write(UC_ARM64_REG_X0,v&((1<<64)-1));self.u.reg_write(UC_ARM64_REG_PC,self.u.reg_read(UC_ARM64_REG_LR))
 def complete(self):
  if self.pending is None or self.time<self.pending[0] or self.mode=='timeout':return
  _,cmd,address,data=self.pending;self.pending=None;self.counter=(self.counter+1)&255;self.code_reply=0;self.fifo=[]
  count=(cmd&15)+1;kind=(cmd>>8)&15
  if self.mode=='defer':self.code_reply=2;return
  if kind==9:
   if address==0:self.fifo=list(self.dpcd[:count])
   elif address==0x202:
    if self.pattern==1:self.cr+=1;status=[0,0,0,0,0x11,0x11] if self.cr==1 else [0x11,0x11,0,0,0x11,0x11]
    elif self.pattern==2:self.eq+=1;status=[0x11,0x11,0,0,0x55,0x55] if self.eq==1 else [0x77,0x77,1,0,0x55,0x55]
    else:status=[0x77,0x77,1,0,0,0]
    if self.mode=='lost-cr':status=[0,0,0,0,0,0]
    self.fifo=status[:count]
   else:self.fifo=[0]*count
   if self.mode=='short':self.fifo=self.fifo[:-1]
  elif kind==8:
   if address==0x102:self.pattern=data[0]&7
  elif kind==0:self.edid_offset=data[0]
  elif kind==1:self.fifo=list(self.edid[self.edid_offset:self.edid_offset+count]);self.edid_offset+=count
  else:raise AssertionError(('Unsupported model AUX',hex(cmd),hex(address)))
 def read(self,a,w):
  self.time+=1;self.complete();self.events.append(['read',a,w])
  if a==0x06c01900:return 0x111
  off=a-0x05740000
  if off==0x14c:return 4 if self.pending else 1 if self.counter else 0
  if off==0x130:return 0
  if off==0x13c:return self.counter
  if off==0x138:return self.code_reply
  if off==0x148:return len(self.fifo)
  if off==0x134:return self.fifo.pop(0)
  return self.registers.get(a,0)
 def write(self,a,w,v):
  self.time+=1;self.events.append(['write',a,w,v]);self.registers[a]=v
  off=a-0x05740000
  if off==0x104:self.transactions.append(v&255)
  elif off==0x100:
   assert self.pending is None
   address=self.registers[0x05740108];kind=(v>>8)&15;count=(v&15)+1
   data=self.transactions[-count:] if not(kind&1) else []
   self.pending=(self.time+100,v,address,data);self.transactions=[]
  return 1
 def callback(self,u,at,size,unused):
  a=[u.reg_read(UC_ARM64_REG_X0+i) for i in range(5)];which=(at-self.callback_base)//16
  if which==0:self.ret(1)
  elif which==1:self.put(a[3],self.read(a[1],a[2]),4);self.ret(1)
  elif which==2:self.ret(self.write(a[1],a[2],a[3]))
  elif which==3:self.ret(1)
  elif which==4:self.time+=1;self.put(a[1],self.time);self.ret(1)
  elif which==5:self.time+=a[1];self.ret(int(self.time<a[2]))
 def call(self,name,*args):
  self.u.reg_write(UC_ARM64_REG_SP,0x43fd0000);self.u.reg_write(UC_ARM64_REG_LR,0x43fff000)
  for i,v in enumerate(args):self.u.reg_write(UC_ARM64_REG_X0+i,v)
  self.u.emu_start(self.image.address(name),0,count=4000000)
  assert self.u.reg_read(UC_ARM64_REG_PC)==0x43fff000,(name,hex(self.u.reg_read(UC_ARM64_REG_PC)))
  return self.u.reg_read(UC_ARM64_REG_X0)
 def checkpoint(self):
  state={k:copy.deepcopy(getattr(self,k)) for k in ('time','registers','events','transactions','pending','counter','fifo','code_reply','pattern','cr','eq','edid_offset')}
  return bytes(self.u.mem_read(self.obj,0x2000)),state
 def restore(self,point):
  memory,state=point;self.u.mem_write(self.obj,memory)
  for k,v in state.items():setattr(self,k,copy.deepcopy(v))
 def result(self):
  l=self.image.layouts['a733_dp'];at=self.obj+l['result'];return bytes(self.u.mem_read(at,352))
def main():
 p=argparse.ArgumentParser(description=__doc__)
 for k in ('image','output'):p.add_argument('--'+k,type=Path,required=True)
 a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False);image=base.Image(a.image,layout_names=('a733_dp','a733_dp_ops','a733_dp_config','a733_dp_result'))
 m=Model(image);assert m.call('a733_dp_init',m.obj,m.cfg)==1;new=m.checkpoint()
 assert m.call('a733_dp_prepare',m.obj)==1;ready=m.checkpoint();(a.output/'ready.pickle').write_bytes(pickle.dumps(ready));assert m.cr==2 and m.eq==2
 assert m.call('a733_dp_start',m.obj)==1;active=m.checkpoint();(a.output/'active.pickle').write_bytes(pickle.dumps(active))
 assert m.call('a733_dp_service',m.obj)==1;assert m.call('a733_dp_stop',m.obj)==1
 core=m.result();assert struct.unpack_from('<4I',core)==(4,0,6,1);(a.output/'core.bin').write_bytes(core)
 results=['complete-discovery-training-active-service-retained-stop']
 for mode in ('timeout','defer','short'):
  m.restore(new);m.mode=mode;assert m.call('a733_dp_prepare',m.obj)==0
  result=m.result();assert struct.unpack_from('<I',result)[0]==5;results.append(mode+'-refused-retained')
 m.restore(active);m.mode='lost-cr';assert m.call('a733_dp_service',m.obj)==0
 assert struct.unpack_from('<I',m.result())[0]==5;assert m.registers[0x05740800]==1;results.append('lost-link-retains-enabled-output')
 m.restore(ready);m.mode='normal';m.put(m.obj+image.layouts['a733_dp']['config']+image.layouts['a733_dp_config']['owner'],99)
 assert m.call('a733_dp_start',m.obj)==0;results.append('configuration-mutation-refused')
 (a.output/'trace.json').write_text(json.dumps(ready[1]['events'])+'\n')
 (a.output/'summary.json').write_text(json.dumps(dict(passed=True,physical=False,scope=__doc__,image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(),outcomes=results,core_sha256=hashlib.sha256(core).hexdigest()),indent=2)+'\n')
 print('PASS actual controller/PHY nominal and five focused retained faults')
if __name__=='__main__':main()
