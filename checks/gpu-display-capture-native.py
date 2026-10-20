#!/usr/bin/env python3
"""Changed GPU/display USB composition in the actual linked image.

Root admission/publication and CPU/kernel scheduling boundaries are modeled;
root separately checks the exact fixture. Existing GPU service completions and
pixels, PMIC bus responses, SHA compression and TinyUSB transfer effects are
modeled. New provider binding, USB platform MMIO, copied-slot evidence, display
callbacks, mailbox identity and TX/ZLP ownership execute natively. No hardware.
"""
import argparse,hashlib,importlib.util,json,struct
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import UC_HOOK_CODE,UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR,UC_ARM64_REG_PSTATE
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
n=module('capture_native_base',ROOT/'checks/gpu-display-native.py')
c=module('capture_client_base',ROOT/'checks/gpu-display-client.py')
r=module('capture_replay_base',ROOT/'checks/gpu-display-replay.py')
host=module('capture_host',ROOT/'apps/gpu-display/capture.py')
def nested(path,typename,field):
 with path.open('rb') as f:
  for cu in ELFFile(f).get_dwarf_info().iter_CUs():
   for die in cu.iter_DIEs():
    if die.tag!='DW_TAG_structure_type' or die.attributes.get('DW_AT_name',None) is None or die.attributes['DW_AT_name'].value.decode()!=typename:continue
    for member in die.iter_children():
     if member.attributes.get('DW_AT_name',None) and member.attributes['DW_AT_name'].value.decode()==field:
      typ=member.get_DIE_from_attribute('DW_AT_type')
      return {x.attributes['DW_AT_name'].value.decode():int(x.attributes['DW_AT_data_member_location'].value) for x in typ.iter_children() if x.tag=='DW_TAG_member' and 'DW_AT_name' in x.attributes}
 raise AssertionError((typename,field))
class Machine(n.Machine):
 def __init__(self,image,path,raw,contract):
  super().__init__(image,path,raw,contract);self.path=path;self.raw=raw;self.contract=contract
  self.u.reg_write(UC_ARM64_REG_PSTATE,0x345)
  self.u.mem_write(self.sealed+self.sf['bytes'],raw);self.u.mem_write(self.sealed+self.sf['digest'],hashlib.sha256(raw).digest());self.put(self.sealed+self.sf['ready'],1,4)
  self.ul=nested(path,'gpu_display_application','usb');self.usb=self.app+self.af['usb']
  self.custom={};self.sha={};self.holders={};self.rx=bytearray();self.tx=bytearray();self.tx_limit=1024;self.tx_capacity=1024;self.zero_write=False;self.flushes=0;self.write_calls=[];self.reason='';self.phase='constructor';self.submits=[];self.reaped=[];self.events=[];self.critical=0;self.queue=0;self.status=0;self.pending_frame=0;self.pm_live=1
  for address,count in [(0x02002000,0x2000),(0x03900000,0x20000),(0x04100000,0x1000),(0x05000000,0x400000),(0x07010000,0x1000),(0x07061000,0x1000),(0x07090000,0x1000)]:self.u.mem_map(address,count)
  defaults=[(0x07090160,0x10000002),(0x070101ac,1),(0x02002000,0xf8000000|(99<<16)|(99<<8)),(0x020022e0,0xf4000000|(3<<16)|(99<<8)),(0x02003928,0x20002),(0x020039a8,0x20000),(0x020025a4,0x30001),(0x020025c0,(1<<5)|(1<<9)),(0x020025e0,(1<<11)|(1<<31)),(0x02002580,0x80000000),(0x02002588,0x80000000),(0x020025e4,0x40302),(0x0200258c,0x10007),(0x020025b4,0x10007),(0x02003300,0x40000000),(0x02003304,0x01000100),(0x02003340,0xa1000005),(0x04100040,0x8140)]
  for at,value in defaults:self.put(at,value,4)
  names=['storage_digest_init','storage_digest_add','storage_digest_end','storage_digest_bytes','a733_pmic_submit','a733_pmic_result','a733_pmic_reap','a733_pmic_live','xQueueGetMutexHolder','xQueueSemaphoreTake','xQueueGenericSend','vTaskDelay','vTaskSuspend','tusb_rhport_init','tud_task_ext','tud_mounted','tud_speed_get','tud_vendor_n_available','tud_vendor_n_read','tud_vendor_n_write_available','tud_vendor_n_write','tud_vendor_n_write_flush','pvr_gpu_service_send','pvr_gpu_service_texture','pvr_gpu_service_collect','a733_hdmi_service','a733_hdmi_stop','a733_hdmi_poll']
  for name in names:
   if not image.symbols.get(name):continue
   at=image.address(name);self.custom[at]=name;self.u.hook_add(UC_HOOK_CODE,self.boundary,begin=at,end=at)
  self.u.hook_add(UC_HOOK_MEM_READ,self.read,begin=0x02000000,end=0x08000000);self.u.hook_add(UC_HOOK_MEM_WRITE,self.write,begin=0x02000000,end=0x08000000)
 def code(self,cpu,at,size,unused):
  if at in self.custom:return
  name=self.hooks.get(at)
  if name=='vTaskEnterCritical':self.critical+=1
  elif name=='vTaskExitCritical':assert self.critical;self.critical-=1
  super().code(cpu,at,size,unused)
  if name in ('a733_smp_counter','a733_counter_clock_now'):self.ticks+=24
 def boundary(self,cpu,at,size,unused):
  name=self.custom[at];a=[self.u.reg_read(UC_ARM64_REG_X0+i) for i in range(8)]
  if name=='storage_digest_init':self.sha[a[0]]=hashlib.sha256();self.ret()
  elif name=='storage_digest_add':assert a[0] in self.sha;self.sha[a[0]].update(bytes(self.u.mem_read(a[1],a[2])));self.ret(1)
  elif name=='storage_digest_end':self.u.mem_write(a[1],self.sha[a[0]].digest());self.ret()
  elif name=='storage_digest_bytes':self.u.mem_write(a[2],hashlib.sha256(bytes(self.u.mem_read(a[0],a[1]))).digest());self.ret()
  elif name in ('xQueueGetMutexHolder','xQueueSemaphoreTake','xQueueGenericSend'):
   if name=='xQueueGetMutexHolder':self.ret(self.holders.get(a[0],0))
   elif name=='xQueueSemaphoreTake':assert not self.holders.get(a[0]);self.holders[a[0]]=self.task;self.ret(1)
   else:assert self.holders.get(a[0])==self.task;self.holders[a[0]]=0;self.ret(1)
  elif name=='vTaskSuspend':self.reason='suspend';self.stopped=True;self.u.emu_stop()
  elif name=='vTaskDelay':
   self.ticks+=24000;self.ret()
   if self.phase=='usb':self.reason='delay';self.stopped=True;self.u.emu_stop()
  elif name=='a733_pmic_live':assert a[0]==self.app+self.af['pmic'];self.ret(self.pm_live)
  elif name=='a733_pmic_submit':
   assert a[0]==self.app+self.af['pmic'] and a[1]==48 and a[2]==self.usb+self.ul['rail'] and a[4]==self.get(self.app+self.af['deadline'])
   assert a[3]==[0,38,8,36][len(self.submits)]
   self.submits.append(a[3]);self.pm_request=a[5];self.pm_id=len(self.submits)
   self.u.mem_write(a[5],struct.pack('<5Q',a[0],777,self.get(self.app+self.af['session']+32),self.pm_id,48));self.ret(0)
  elif name=='a733_pmic_result':
   assert a[0]==self.app+self.af['pmic'] and a[1]==self.pm_request
   sl=self.image.layouts['a733_pmic_snapshot'];self.u.mem_write(a[2],bytes(sl['programmed_uv']+4))
   for key,value,width in [('rail',self.submits[-1],4),('chip',3,1),('enabled',1,4),('decoded',1,4),('programmed_uv',[3300000,3300000,1200000,900000][self.pm_id-1],4)]:self.put(a[2]+sl[key],value,width)
   self.ret(2)
  elif name=='a733_pmic_reap':assert a[0]==self.app+self.af['pmic'] and a[1]==self.pm_request;self.reaped.append(self.pm_id);self.ret(2)
  elif name=='tusb_rhport_init':self.tusb_calls=getattr(self,'tusb_calls',0)+1;assert self.tusb_calls==1;self.ret(1)
  elif name=='tud_task_ext':self.ret()
  elif name=='tud_mounted':self.ret(1)
  elif name=='tud_speed_get':self.ret(2)
  elif name=='tud_vendor_n_available':self.ret(len(self.rx))
  elif name=='tud_vendor_n_read':
   count=min(a[2],len(self.rx),7);self.u.mem_write(a[1],bytes(self.rx[:count]));del self.rx[:count];self.ret(count)
  elif name=='tud_vendor_n_write_available':self.ret(self.tx_capacity)
  elif name=='tud_vendor_n_write':
   self.write_calls.append(a[2])
   count=0 if self.zero_write else min(a[2],self.tx_limit);self.tx.extend(bytes(self.u.mem_read(a[1],count)));self.ret(count)
  elif name=='tud_vendor_n_write_flush':self.flushes+=1;self.ret(0)
  elif name in ('pvr_gpu_service_send','pvr_gpu_service_texture','pvr_gpu_service_collect'):
   self.hooks[at]=name;c.Machine.service_code(self,cpu,at,size,unused)
  elif name=='a733_hdmi_service':assert a[0]==self.h and self.get(self.h+self.hcl['state'],4)==11;self.ret(1)
  elif name=='a733_hdmi_stop':assert a[0]==self.h;self.put(self.h+self.hcl['state'],12,4);self.ret(1)
  elif name=='a733_hdmi_poll':assert a[0]==self.h and self.get(self.h+self.hcl['state'],4) in (12,13);self.put(self.h+self.hcl['state'],13,4);self.ret(1)
  else:raise AssertionError(name)
 def read(self,*args):r.Machine.read(self,*args)
 def write(self,*args):r.Machine.write(self,*args)
 def mask(self,name):return r.Machine.mask(self,name)
 def acquire_clock(self,*args):return r.Machine.acquire_clock(self,*args)
 def invoke(self,name,*args,unit=None,count=100000000):
  self.callname=name;self.stopped=False;self.reason='';self.u.reg_write(UC_ARM64_REG_SP,0x43fd0000);self.u.reg_write(UC_ARM64_REG_LR,0x43fff000)
  for i,value in enumerate(args):self.u.reg_write(UC_ARM64_REG_X0+i,value)
  self.u.emu_start(self.image.address(name,unit=unit),0,count=count)
  assert self.stopped or self.u.reg_read(UC_ARM64_REG_PC)==0x43fff000,(name,hex(self.u.reg_read(UC_ARM64_REG_PC)),self.trace[-20:])
  return self.u.reg_read(UC_ARM64_REG_X0)
 def call(self,name,*args):return self.invoke(name,*args)
 def usb_batch(self,first=False):
  self.task=self.get(self.app+self.af['handles']+24);self.phase='usb';self.stopped=False;self.reason=''
  if first:
   self.u.reg_write(UC_ARM64_REG_SP,0x43fc0000);self.u.reg_write(UC_ARM64_REG_LR,0x43fff000);self.u.reg_write(UC_ARM64_REG_X0,0);at=self.image.address('gpu_display_usb_task')
  else:self.u.context_restore(self.usb_context);at=self.u.reg_read(UC_ARM64_REG_PC)
  self.u.emu_start(at,0,count=10000000)
  pl=self.image.layouts['a733_usb_platform'];failure=self.usb+self.ul['platform']+pl['failure'];fl=self.image.layouts['a733_usb_failure']
  assert self.stopped and self.reason=='delay',(self.reason,hex(self.u.reg_read(UC_ARM64_REG_PC)),{k:self.get(failure+fl[k],8 if k in ('address','last_us','sample_us','deadline_us') else 4) for k in fl},self.submits,self.reaped,self.trace[-10:])
  self.usb_context=self.u.context_save()
 def gpu_setup(self):
  image=self.image;self.service=self.app+self.af['gpu_service'];self.sl=image.layouts['pvr_gpu_service'];self.rl=image.layouts['pvr_render_service'];self.gl=image.layouts['a733_gpu_graphics_memory'];self.graphics=self.app+self.af['gpu_graphics'];self.submission=self.graphics+self.gl['submission'];self.layout=0x43e00000
  assert self.call('pvr_render_texture_layout_profile',1280,720,2,self.layout)==1
  self.output=self.get(self.graphics+self.gl['payload_extent'])+self.get(self.graphics+self.gl['render']+image.layouts['pvr_render_requirements']['payload_bytes'],4)+self.get(self.layout+image.layouts['pvr_render_texture_layout']['download'],4)
  self.put(self.service+self.sl['owner'],0,4);self.put(self.service+self.sl['phase'],3,4)
  self.reply={};self.ticket=100;self.frame=-1;self.input=bytearray();self.borrow=None;self.events=[];self.requests=[];self.q=image.layouts['pvr_render_texture_request'];self.r=image.layouts['pvr_gpu_service_reply'];self.canonical=[];self.shapes=[]
  inputs=self.call('gpu_display_inputs_get')
  for frame in range(2):
   self.shapes.append(bytes(self.u.mem_read(inputs+16+frame*64,20)));offset,count=struct.unpack('<2I',self.u.mem_read(inputs+16+frame*64+20,8));self.canonical.append(bytes(self.u.mem_read(inputs+offset,count)))
 def display_setup(self):
  e=self.image;self.fb=e.address('framebuffer');self.fl=e.layouts['hdmi_framebuffer'];self.hdmi=self.app+self.af['hdmi'];self.hl=e.layouts['a733_hdmi_native'];self.h=self.hdmi+self.hl['controller'];self.hcl=e.layouts['a733_hdmi'];self.de=self.fb+self.fl['de'];self.dl=e.layouts['a733_de'];self.bank=self.get(self.fb+self.fl['extent']);r.Machine.setup(self)
 def deliver(self,packet):
  self.rx.extend(packet);self.tx=bytearray()
  for i in range(20):
   self.usb_batch()
   if self.get(self.app+self.af['observation'],4)==1:break
  assert self.get(self.app+self.af['observation'],4)==1
  self.task=self.get(self.app+self.af['handles']+8);self.call('gpu_display_observation_poll')
  assert self.get(self.app+self.af['observation'],4)==2
  self.tx_capacity=0;self.usb_batch();assert not self.tx and self.get(self.app+self.af['observation'],4)==3
  self.tx_capacity=1024
  for i in range(30):
   self.usb_batch()
   if len(self.tx)==1024:break
  assert len(self.tx)==1024
  self.task=self.get(self.app+self.af['handles']+24)
  self.call('tud_vendor_tx_cb',0,512);self.call('tud_vendor_tx_cb',0,512)
  assert self.get(self.app+self.af['observation'],4)==3
  self.call('tud_vendor_tx_cb',0,0);assert self.get(self.app+self.af['observation'],4)==0
  return bytes(self.tx)
def preserve(m,folder,label):
 folder.mkdir(parents=True,exist_ok=True)
 pools=[];pl=m.image.layouts['device_memory_pool'];al=m.image.layouts['device_memory_allocation']
 for name,fn in [('wb','a733_memory_reservations_init'),('nc','a733_memory_uncached_init')]:
  pool=m.call(fn);end=0
  for i in range(32):
   slot=pool+pl['allocations']+i*32
   if m.get(slot+al['owner']):end=max(end,m.get(slot+al['first'],4)+m.get(slot+al['pages'],4))
  pools.append((m.get(pool+pl['backing']),end*4096))
 memory=[]
 spans=[(0x41000000,0x400000),(0x43f80000,0x80000),*pools,(0x02002000,0x2000),(0x03900000,0x20000),(0x04100000,0x1000),(0x05000000,0x400000),(0x07010000,0x1000),(0x07061000,0x1000),(0x07090000,0x1000)]
 for at,count in spans:
  file=label+'-'+hex(at)+'.bin';data=bytes(m.u.mem_read(at,count));(folder/file).write_bytes(data);memory.append(dict(address=at,file=file,sha256=hashlib.sha256(data).hexdigest()))
 attrs={k:getattr(m,k) for k in ['tasks','clocks','submits','reaped','events','queue','status','pending_frame','critical','pm_live','ticks','tusb_calls','pm_request','pm_id'] if hasattr(m,k)}
 attrs['holders']={str(k):v for k,v in m.holders.items()}
 if hasattr(m,'usb_context'):
  saved=m.u.context_save();m.u.context_restore(m.usb_context)
  attrs['usb_regs']={str(reg):m.u.reg_read(reg) for reg in [*[UC_ARM64_REG_X0+i for i in range(29)],UC_ARM64_REG_SP,UC_ARM64_REG_LR,UC_ARM64_REG_PC,UC_ARM64_REG_PSTATE]}
  m.u.context_restore(saved)
 record=dict(image_sha256=hashlib.sha256(m.path.read_bytes()).hexdigest(),session_sha256=hashlib.sha256(m.raw).hexdigest(),contract_sha256=hashlib.sha256(m.contract).hexdigest(),memory=memory,attributes=attrs)
 (folder/(label+'.json')).write_text(json.dumps(record,indent=2)+'\n')
def restore(m,folder,label):
 record=json.loads((folder/(label+'.json')).read_text())
 assert record['image_sha256']==hashlib.sha256(m.path.read_bytes()).hexdigest() and record['session_sha256']==hashlib.sha256(m.raw).hexdigest() and record['contract_sha256']==hashlib.sha256(m.contract).hexdigest()
 for row in record['memory']:
  data=(folder/row['file']).read_bytes();assert hashlib.sha256(data).hexdigest()==row['sha256'];m.u.mem_write(row['address'],data)
 attrs=record['attributes']
 for k,v in attrs.items():
  if k=='usb_regs':
   for reg,value in v.items():m.u.reg_write(int(reg),value)
   m.usb_context=m.u.context_save()
  else:setattr(m,k,{int(a):b for a,b in v.items()} if k=='holders' else v)
 m.scheduler=2;m.stopped=False

def main():
 p=argparse.ArgumentParser(description=__doc__)
 for key in ('image','session','contract','output'):p.add_argument('--'+key,type=Path,required=True)
 a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True);e=n.base.Image(a.image);raw=a.session.read_bytes();contract=a.contract.read_bytes();m=Machine(e,a.image,raw,contract)
 epoch,generation=struct.unpack_from('<2Q',raw,48);lease=struct.unpack_from('<Q',raw,64)[0];build=raw[72:104];digest=hashlib.sha256(raw).digest()
 identity=dict(epoch=epoch,generation=generation,lease=lease,build_id=build,session_sha256=digest)
 if (a.output/'caller-checkpoint.json').exists():restore(m,a.output,'caller-checkpoint')
 else:
  assert m.call('a733_memory_reservations_commit_verified')==m.call('a733_memory_uncached_commit_verified')==1
  m.call('a733_smp_diagnostic_start');assert len(m.tasks)==5 and len(m.clocks)==1
  m.scheduler=2;m.task=m.get(m.app+m.af['handles']+24);m.put(m.app+m.af['pmic_ready'],1,4)
  for i in range(500):
   m.usb_batch(first=i==0)
   if m.get(m.app+m.af['usb_ready'],4):break
  assert m.get(m.app+m.af['usb_ready'],4)==1 and m.reaped==[1,2,3,4]
  pending=m.deliver(host.request(1,epoch,generation));(a.output/'pending.bin').write_bytes(pending)
  host.decode(pending,request_id=1,**identity)
  m.gpu_setup();m.phase='gpu';m.task=m.get(m.app+m.af['handles']+16);m.call('gpu_display_render_task',0)
  assert m.reason=='suspend' and m.get(m.app+m.af['frames_ready'],4)==1 and not m.get(m.app+m.af['fatal'],4)
  preserve(m,a.output,'caller-checkpoint')
  print('PASS shared USB providers and native two-input evidence publication',flush=True)
 record=dict(passed=True,scope=__doc__,image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(),session_sha256=digest.hex(),contract_sha256=hashlib.sha256(contract).hexdigest(),tasks=len(m.tasks),counter_instances=len(m.clocks),rail_requests=m.submits,rail_reaped=m.reaped,physical_access=False)
 (a.output/'caller.json').write_text(json.dumps(record,indent=2)+'\n')
 if (a.output/'presented-checkpoint.json').exists():restore(m,a.output,'presented-checkpoint')
 else:
  m.task=m.get(m.app+m.af['handles']+8);m.phase='display';m.display_setup()
  for i in range(400):
   result=m.call('gpu_display_output_step');m.ticks+=2400
   assert result<0x80000000,(i,result,m.get(m.de+m.dl['error'],4),m.get(m.de+m.dl['step'],4))
   if m.get(m.app+m.af['presentations'],4)==3:break
  assert m.get(m.app+m.af['presentations'],4)==3
  preserve(m,a.output,'presented-checkpoint');print('PASS source-verified 0/1/0 records with post-completion timestamps',flush=True)
 # Recreate only the private address aliases after restoring preserved memory.
 m.fb=e.address('framebuffer');m.fl=e.layouts['hdmi_framebuffer'];m.hdmi=m.app+m.af['hdmi'];m.hl=e.layouts['a733_hdmi_native'];m.h=m.hdmi+m.hl['controller'];m.hcl=e.layouts['a733_hdmi'];m.de=m.fb+m.fl['de'];m.dl=e.layouts['a733_de'];m.deadline=m.get(m.app+m.af['deadline']);m.bank=m.get(m.fb+m.fl['extent'])
 if (a.output/'terminal-checkpoint.json').exists():restore(m,a.output,'terminal-checkpoint')
 else:
  m.task=m.get(m.app+m.af['handles']+8);m.ticks=m.get(m.app+m.af['stop_at'])*24
  for i in range(300):
   result=m.call('gpu_display_output_step');m.ticks+=2400;assert result<0x80000000
   if m.get(m.app+m.af['pmic_park_request'],4):break
  assert m.get(m.app+m.af['pmic_park_request'],4)==1 and m.get(m.app+m.af['state'],4)!=2
  m.phase='pmic';m.task=m.get(m.app+m.af['handles']);m.call('pmic_task',0)
  assert m.reason=='suspend' and m.get(m.app+m.af['pmic_parked'],4)==1
  m.task=m.get(m.app+m.af['handles']+8);assert m.call('gpu_display_output_step')==1
  assert m.get(m.app+m.af['state'],4)==2 and not m.get(m.app+m.af['fatal'],4)
  healthy=m.deliver(host.request(2,epoch,generation));(a.output/'healthy.bin').write_bytes(healthy)
  decoded=host.decode(healthy,request_id=2,**identity);assert decoded['state']==2
  preserve(m,a.output,'terminal-checkpoint');print('PASS terminal park acknowledgment and actual copied reply/TX/ZLP',flush=True)
 healthy=(a.output/'healthy.bin').read_bytes();decoded=host.decode(healthy,request_id=2,**identity)
 # Compare independent modeled completed-output bytes with the hashes/samples
 # emitted by the actual renderer/observation code.
 for frame,proof in enumerate(decoded['frames']):
  rgba=bytearray()
  for offset in range(0,host.PIXEL_BYTES,65536):
   count=min(65536,host.PIXEL_BYTES-offset);rgba.extend((frame*99+offset//4+i*17)&255 for i in range(count))
  rgb=b''.join(struct.pack('<H',((rgba[i]>>3)<<11)|((rgba[i+1]>>2)<<5)|(rgba[i+2]>>3)) for i in range(0,len(rgba),4))
  assert proof['rgba_sha256']==hashlib.sha256(rgba).hexdigest() and proof['rgb565_sha256']==hashlib.sha256(rgb).hexdigest()
  input_offset=840+16+64*frame;assert proof['input_sha256']==raw[input_offset+32:input_offset+64].hex()
  for sample in proof['samples']:
   index=sample['y']*1280+sample['x'];assert sample['rgba']==struct.unpack_from('<I',rgba,index*4)[0] and sample['rgb565']==struct.unpack_from('<H',rgb,index*2)[0]
 record.update(pending_sha256=hashlib.sha256((a.output/'pending.bin').read_bytes()).hexdigest(),healthy_sha256=hashlib.sha256(healthy).hexdigest(),terminal=decoded['terminal'],presentations=decoded['presentations'])
 (a.output/'result.json').write_text(json.dumps(record,indent=2)+'\n');print('PASS independent output hashes and fixed samples',flush=True)
if __name__=='__main__':main()
