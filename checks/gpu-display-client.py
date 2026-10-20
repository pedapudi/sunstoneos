#!/usr/bin/env python3
"""Execute the linked two-input caller and actual copied-output/bank accessors.

The accepted constructor checkpoint is reused. GPU service queue completion and
pixel values are modeled; immutable input copying, request/release ordering,
output borrowing, RGB565 conversion and bank publication execute natively.
"""
import argparse,hashlib,importlib.util,json,struct
from pathlib import Path
from unicorn import UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC
ROOT=Path(__file__).resolve().parents[1]
s=importlib.util.spec_from_file_location('gpu_display_native',ROOT/'checks/gpu-display-native.py');n=importlib.util.module_from_spec(s);s.loader.exec_module(n)
class Machine(n.Machine):
 def __init__(self,image,path,folder):
  raw=(folder/'session-first.bin').read_bytes();contract=(folder/'contract-first.bin').read_bytes();super().__init__(image,path,raw,contract)
  state=json.loads((folder/'constructor.json').read_text());assert state['image_sha256']==hashlib.sha256(path.read_bytes()).hexdigest()
  assert state['session_sha256']==hashlib.sha256(raw).hexdigest() and state['contract_sha256']==hashlib.sha256(contract).hexdigest()
  for row in state['memory']:self.u.mem_write(row['address'],(folder/row['file']).read_bytes())
  self.tasks=state['tasks'];self.scheduler=2;self.task=self.get(self.app+self.af['handles']+16);self.stopped=False
  self.put(self.app+self.af['pmic_ready'],1,4)
  self.service=self.app+self.af['gpu_service'];self.sl=image.layouts['pvr_gpu_service'];self.rl=image.layouts['pvr_render_service'];self.gl=image.layouts['a733_gpu_graphics_memory'];self.graphics=self.app+self.af['gpu_graphics'];self.submission=self.graphics+self.gl['submission']
  self.put(self.service+self.sl['owner'],0,4);self.put(self.service+self.sl['phase'],3,4)
  self.layout=0x43e00000;assert self.call('pvr_render_texture_layout_profile',1280,720,2,self.layout)==1
  self.output=self.get(self.graphics+self.gl['payload_extent'])+self.get(self.graphics+self.gl['render']+image.layouts['pvr_render_requirements']['payload_bytes'],4)+self.get(self.layout+image.layouts['pvr_render_texture_layout']['download'],4)
  self.reply={};self.ticket=100;self.frame=-1;self.input=bytearray();self.borrow=None;self.events=[];self.requests=[];self.q=image.layouts['pvr_render_texture_request'];self.r=image.layouts['pvr_gpu_service_reply'];self.canonical=[];self.shapes=[]
  inputs=self.call('gpu_display_inputs_get');self.inputs=inputs
  for frame in range(2):
   self.shapes.append(bytes(self.u.mem_read(inputs+16+frame*64,20)))
   ofs,count=struct.unpack('<2I',self.u.mem_read(inputs+16+frame*64+20,8));self.canonical.append(bytes(self.u.mem_read(inputs+ofs,count)))
  for name in ['pvr_gpu_service_send','pvr_gpu_service_texture','pvr_gpu_service_collect','vTaskDelay','vTaskSuspend']:
   at=image.address(name);self.hooks[at]=name;self.u.hook_add(UC_HOOK_CODE,self.service_code,begin=at,end=at)
 def service_code(self,cpu,at,size,unused):
  name=self.hooks[at];a=[self.u.reg_read(UC_ARM64_REG_X0+i) for i in range(8)]
  # Remove the base callback's unsupported-boundary dispatch at these sites.
  if name=='vTaskSuspend':self.stopped=True;self.u.emu_stop();return
  if name=='vTaskDelay':self.ret();return
  if name=='pvr_gpu_service_collect':
   assert a[0]==self.service and a[1]==self.ticket
   self.u.mem_write(a[2],bytes(512))
   for key,value in self.reply.items():self.put(a[2]+self.r[key],value,8 if key in ('ticket','epoch','texture_generation') else 4)
   self.ret(1);return
  self.ticket+=1;self.reply=dict(ticket=self.ticket,result=0,epoch=1,phase=3,render_phase=9,render_sequence=self.frame+1)
  if name=='pvr_gpu_service_send':
   op=self.get(a[1],4);assert op in (1,2)
   self.events.append('open' if op==1 else 'close')
   if op==2:
    assert self.frame==1 and self.borrow is None;self.reply['phase']=5
   self.put(a[3],self.ticket);self.ret(1);return
  assert name=='pvr_gpu_service_texture' and a[0]==self.service and a[1]==1
  q=a[2];op=self.get(q+self.q['operation'],4);previous=self.get(q+self.q['previous'],4);offset=self.get(q+self.q['offset'],4);count=self.get(q+self.q['bytes'],4);generation=self.get(q+self.q['generation'])
  self.requests.append(dict(operation=op,generation=generation,previous=previous,offset=offset,bytes=count,input_shape=bytes(self.u.mem_read(q+self.q['input'],20)).hex()))
  if op==0:
   assert self.borrow is None and previous==self.frame+1;self.frame+=1;assert self.frame<2
   assert bytes(self.u.mem_read(q+self.q['input'],20))==self.shapes[self.frame]
   self.input=bytearray();self.reply['texture_generation']=self.frame+1
  elif op==1:
   assert generation==self.frame+1 and offset==len(self.input) and 0<count<=65536
   self.input.extend(bytes(self.u.mem_read(a[3],count)));assert self.canonical[self.frame][:len(self.input)]==self.input
  elif op==2:
   assert generation==self.frame+1 and previous==self.frame and bytes(self.input)==self.canonical[self.frame] and not offset and not count;self.reply['render_sequence']=self.frame+1
  elif op==3:
   assert self.borrow is None and previous==self.frame+1 and 0<count<=65536
   self.borrow=(self.ticket,self.frame+1,offset,count)
   self.u.mem_write(self.output,bytes((self.frame*99+offset//4+i*17)&255 for i in range(count)))
   for key,value,width in [('phase',9,4),('sequence',self.frame+1,4),('texture_output_state',1,4),('texture_output_frame',self.frame+1,4),('texture_output_offset',offset,4),('texture_output_bytes',count,4),('texture_output_client',0,4),('texture_output_ticket',self.ticket,8)]:self.put(self.submission+self.rl[key],value,width)
  elif op==4:
   assert self.borrow and (generation,previous,offset)==self.borrow[:3] and count==0
   assert self.get(self.app+self.af['copied']+4*self.frame,4)==self.borrow[2]+self.borrow[3]
   self.borrow=None;self.put(self.submission+self.rl['texture_output_state'],0,4)
  else:raise AssertionError(op)
  self.put(a[6],self.ticket);self.ret(1)
 def code(self,cpu,at,size,unused):
  if self.hooks.get(at) in ('pvr_gpu_service_send','pvr_gpu_service_texture','pvr_gpu_service_collect','vTaskDelay','vTaskSuspend'):return
  super().code(cpu,at,size,unused)
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--image',type=Path,required=True);p.add_argument('--checkpoint',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
 e=n.base.Image(a.image);m=Machine(e,a.image,a.checkpoint);m.call('gpu_display_render_task',0)
 assert m.stopped and not m.get(m.app+m.af['fatal'],4)
 assert m.get(m.app+m.af['frames_ready'],4)==m.get(m.app+m.af['gpu_closed'],4)==1 and m.borrow is None
 assert m.events==['open','close'] and m.frame==1
 fb=e.address('framebuffer');fl=e.layouts['hdmi_framebuffer'];bank=m.get(fb+fl['extent'])
 framehash=[hashlib.sha256(bytes(m.u.mem_read(bank+36864+i*1847296,1843200))).hexdigest() for i in range(2)]
 for file,address,count in [('main-after-client.bin',0x41000000,0x400000),('bank-after-client.bin',bank,4194304)]:
  (a.output/file).write_bytes(bytes(m.u.mem_read(address,count)))
 record=dict(passed=True,image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(),scope=__doc__,physical_access=False,requests=m.requests,events=m.events,frame_rgb565_sha256=framehash,bank_address=bank)
 (a.output/'client.json').write_text(json.dumps(record,indent=2)+'\n');print('PASS actual caller two distinct inputs, exact output release, GPU CLOSE, immutable display bank')
if __name__=='__main__':main()
