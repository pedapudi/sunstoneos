#!/usr/bin/env python3
"""Native TV0 preparation/replay over the actual immutable copied frame bank.

Reuses the constructor/client checkpoint. HDMI PHY/controller preparation and
PMIC readiness, HDMI health service and HDMI stop are modeled component boundaries. Clock/rail/PCK
leases, display mappings, IOMMU, DE callbacks and MMIO access execute natively;
register effects, counter and mutex/critical scheduling are modeled.
"""
import argparse,hashlib,importlib.util,json,re,struct
from pathlib import Path
from unicorn import UC_HOOK_CODE,UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC,UC_ARM64_REG_PSTATE
ROOT=Path(__file__).resolve().parents[1]
s=importlib.util.spec_from_file_location('display_native',ROOT/'checks/gpu-display-native.py');n=importlib.util.module_from_spec(s);s.loader.exec_module(n)
class Machine(n.Machine):
 def __init__(self,image,path,checkpoint,client):
  super().__init__(image,path,(checkpoint/'session-first.bin').read_bytes(),(checkpoint/'contract-first.bin').read_bytes())
  state=json.loads((checkpoint/'constructor.json').read_text())
  assert state['session_sha256']==hashlib.sha256((checkpoint/'session-first.bin').read_bytes()).hexdigest()
  assert state['contract_sha256']==hashlib.sha256((checkpoint/'contract-first.bin').read_bytes()).hexdigest()
  self.binding={key:state[key] for key in ['image_sha256','session_sha256','contract_sha256']}
  self.binding.update(constructor_sha256=hashlib.sha256((checkpoint/'constructor.json').read_bytes()).hexdigest(),client_sha256=hashlib.sha256((client/'client.json').read_bytes()).hexdigest(),main_sha256=hashlib.sha256((client/'main-after-client.bin').read_bytes()).hexdigest(),bank_sha256=hashlib.sha256((client/'bank-after-client.bin').read_bytes()).hexdigest())
  self.clocks=state['clocks'];self.tasks=state['tasks']
  self.u.mem_write(0x41000000,(client/'main-after-client.bin').read_bytes());record=json.loads((client/'client.json').read_text());self.bank=record['bank_address'];self.u.mem_write(self.bank,(client/'bank-after-client.bin').read_bytes())
  assert record['image_sha256']==hashlib.sha256(path.read_bytes()).hexdigest()
  self.scheduler=2;self.task=self.get(self.app+self.af['handles']+8);self.u.reg_write(UC_ARM64_REG_PSTATE,0x345)
  self.stopped=False
  self.fb=image.address('framebuffer');self.fl=image.layouts['hdmi_framebuffer'];self.hdmi=self.app+self.af['hdmi'];self.hl=image.layouts['a733_hdmi_native'];self.h=self.hdmi+self.hl['controller'];self.hcl=image.layouts['a733_hdmi'];self.de=self.fb+self.fl['de'];self.dl=image.layouts['a733_de'];self.holders={};self.events=[];self.queue=0;self.status=0;self.status_reads=0;self.races=[0,0];self.fault=0;self.critical=0
  for base,count in [(0x02002000,0x2000),(0x03900000,0x20000),(0x05000000,0x400000),(0x07010000,0x1000),(0x07061000,0x1000),(0x07090000,0x1000)]:self.u.mem_map(base,count)
  for address,value in [(0x07090160,0x10000002),(0x070101ac,1),(0x02002000,0xf8000000|(99<<16)|(99<<8)),(0x020022e0,0xf4000000|(3<<16)|(99<<8)),(0x02003928,0x20002),(0x020039a8,0x20000),(0x020025c0,1<<5),(0x020025e0,1<<11),(0x02002580,0x80000000),(0x02002588,0x80000000),(0x020025e4,0x40302),(0x0200258c,0x10007),(0x020025b4,0x10007)]:self.put(address,value,4)
  for name in ['a733_pmic_live','xQueueGetMutexHolder','xQueueSemaphoreTake','xQueueGenericSend','a733_hdmi_service','a733_hdmi_stop','a733_hdmi_poll','vTaskSuspend']:
   at=image.address(name);self.hooks[at]=name;self.u.hook_add(UC_HOOK_CODE,self.boundary,begin=at,end=at)
  self.u.hook_add(UC_HOOK_MEM_READ,self.read,begin=0x02000000,end=0x08000000);self.u.hook_add(UC_HOOK_MEM_WRITE,self.write,begin=0x02000000,end=0x08000000)
 def code(self,cpu,at,size,unused):
  name=self.hooks.get(at)
  if name in ('a733_pmic_live','xQueueGetMutexHolder','xQueueSemaphoreTake','xQueueGenericSend','a733_hdmi_service','a733_hdmi_stop','a733_hdmi_poll','vTaskSuspend'):return
  if name=='vTaskEnterCritical':self.critical+=1
  elif name=='vTaskExitCritical':assert self.critical;self.critical-=1
  super().code(cpu,at,size,unused)
 def boundary(self,cpu,at,size,unused):
  name=self.hooks[at];a=[self.u.reg_read(UC_ARM64_REG_X0+i) for i in range(8)]
  if name=='vTaskSuspend':self.stopped=True;self.u.emu_stop()
  elif name=='a733_pmic_live':assert a[0]==self.app+self.af['pmic'];self.ret(1)
  elif name=='xQueueGetMutexHolder':self.ret(self.holders.get(a[0],0))
  elif name=='xQueueSemaphoreTake':assert not self.holders.get(a[0]);self.holders[a[0]]=self.task;self.ret(1)
  elif name=='xQueueGenericSend':assert self.holders.get(a[0])==self.task;self.holders[a[0]]=0;self.ret(1)
  elif name=='a733_hdmi_service':assert a[0]==self.h and self.get(self.h+self.hcl['state'],4)==11;self.ret(1)
  elif name=='a733_hdmi_stop':assert a[0]==self.h;self.put(self.h+self.hcl['state'],12,4);self.ret(1)
  elif name=='a733_hdmi_poll':assert a[0]==self.h and self.get(self.h+self.hcl['state'],4) in (12,13);self.put(self.h+self.hcl['state'],13,4);self.ret(1)
 def read(self,u,access,address,size,value,unused):
  if address==0x07061008:self.put(address,self.get(address-8,4)&15,4)
  elif address==0x05008150:self.put(address,0,4)
  elif address==0x05008144:
   # FRAME_END latches on a fixed pseudo-random half of replay status reads,
   # independent of the driver step and of its W1C clears. A fixed period can
   # stay in phase with the driver and never land on one of the steps.
   if self.get(self.de+self.dl['action'],4)==4:
    self.status_reads=(self.status_reads*1103515245+12345)&0xffffffff
    if (self.status_reads>>16)&1:self.status|=1
    step=self.get(self.de+self.dl['step'],4)
    if self.status&1 and step in (4,17):self.races[step==17]+=1
   self.put(address,self.status,4)
 def write(self,u,access,address,size,value,unused):
  self.events.append([address,value,self.ticks//24])
  if address==0x05008144:
   self.status&=~(value&5)
  elif address==0x05008150 and value&1:
   assert self.critical==1 and any(v==self.task for v in self.holders.values())
   self.queue+=1;self.status|=4
   header=self.get(0x05008154,4);offset=header-0x10000000+32768
   # Actual immutable RCQ descriptor's UCH3 address payload at word8.
   page=bytes(self.u.mem_read(self.bank+offset,64));words=struct.unpack('<16I',page)
   self.put(0x05221010,words[8],4)
  elif address==0x05008154:self.status|=16
 def mask(self,name):
  text=(ROOT/'include/a733_clock_leases.h').read_text();bit=re.search(r'#define '+name+r'\s+A733_CLOCK_BIT\((\d+)\)',text);assert bit,name;return 1<<int(bit[1])
 def acquire_clock(self,shared,exclusive,out):
  return self.call('a733_clock_acquire',self.app+self.af['clocks'],self.owner,shared&((1<<64)-1),shared>>64,exclusive&((1<<64)-1),exclusive>>64,self.deadline,out)
 def setup(self):
  self.deadline=self.get(self.app+self.af['deadline']);self.owner=self.get(self.app+self.af['session']+self.image.layouts['hdmi_session']['owner'])
  shared=self.mask('A733_CLOCK_OSC')|self.mask('A733_CLOCK_FABRIC');assert self.acquire_clock(shared,self.mask('A733_CLOCK_VO1'),self.hdmi+self.hl['clock'])==0
  rail=self.hdmi+self.hl['rail'];assert self.call('a733_rail_acquire',self.app+self.af['rails'],self.owner,(1<<1)|(1<<21),0,self.deadline,rail)==1
  for i in range(10):
   result=self.call('a733_rail_poll',self.app+self.af['rails'],rail)
   if result==0:break
   assert result==1
  assert result==0
  client=self.get(self.app+self.af['session']+self.image.layouts['hdmi_session']['pck_client'],4);scratch=0x43e10000;pck=self.app+self.af['pck']
  assert self.call('a733_gpu_resources_acquire',pck,client,9,self.deadline,scratch,scratch+64)==0
  ml=self.image.layouts['a733_gpu_resource_manager'];self.u.mem_write(self.hdmi+self.hl['pck_lease'],bytes(self.u.mem_read(pck+ml['leases']+client*16,16)))
  self.put(self.h+self.hcl['state'],9,4)
  for i in range(3000):
   result=self.call('hdmi_framebuffer_prepare');self.ticks+=2400
   if result!=0:break
  assert result==1,(result,i,self.get(self.fb+self.fl['stage'],4),self.trace[-20:])
  assert self.get(self.de+self.dl['state'],4)==2
  self.put(self.h+self.hcl['state'],11,4);self.put(self.app+self.af['stage'],3,4);self.put(self.app+self.af['state'],1,4)
def save(m,folder,label):
 folder.mkdir(parents=True,exist_ok=True);memory=[]
 for address,count in [(0x41000000,0x400000),(m.bank,4194304),(0x02002000,0x2000),(0x03900000,0x20000),(0x05000000,0x400000),(0x07010000,0x1000),(0x07061000,0x1000),(0x07090000,0x1000)]:
  file=label+'-'+hex(address)+'.bin';(folder/file).write_bytes(bytes(m.u.mem_read(address,count)));memory.append(dict(address=address,file=file))
 record={k:getattr(m,k) for k in ['ticks','queue','status','status_reads','races','holders','events','owner','deadline']};record['memory']=memory;record['binding']=m.binding;record['image_sha256']=hashlib.sha256(m.image_path.read_bytes()).hexdigest();(folder/(label+'.json')).write_text(json.dumps(record,indent=2)+'\n')
def restore(m,folder,label):
 record=json.loads((folder/(label+'.json')).read_text());assert record['image_sha256']==hashlib.sha256(m.image_path.read_bytes()).hexdigest()
 assert record.pop('binding')==m.binding
 for row in record.pop('memory'):m.u.mem_write(row['address'],(folder/row['file']).read_bytes())
 for k,v in record.items():
  if k!='image_sha256':setattr(m,k,{int(a):b for a,b in v.items()} if k=='holders' else v)
def main():
 p=argparse.ArgumentParser(description=__doc__)
 for name in ('image','checkpoint','client','output'):p.add_argument('--'+name,type=Path,required=True)
 a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True);e=n.base.Image(a.image);m=Machine(e,a.image,a.checkpoint,a.client);m.image_path=a.image
 if (a.output/'presented.json').exists():restore(m,a.output,'presented')
 else:
  if (a.output/'prepared.json').exists():restore(m,a.output,'prepared')
  else:m.setup();save(m,a.output,'prepared')
  for i in range(300):
   r=m.call('gpu_display_output_step');m.ticks+=2400
   assert r<0x80000000,(i,r,m.get(m.de+m.dl['error'],4),m.get(m.de+m.dl['step'],4))
   if m.get(m.app+m.af['presentations'],4)==3:break
  assert m.get(m.app+m.af['presentations'],4)==3
  assert struct.unpack('<3I',m.u.mem_read(m.app+m.af['presented'],12))==(1,2,1)
  assert all(m.races),m.races
  assert 0<m.get(m.de+m.dl['replay_rcq_done'])<m.get(m.de+m.dl['replay_frame_end'])
  save(m,a.output,'presented');print('PASS actual TV0 replay 0→1→0',flush=True)
 m.ticks=m.get(m.app+m.af['stop_at'])*24
 for i in range(300):
  r=m.call('gpu_display_output_step');m.ticks+=2400;assert r<0x80000000,(i,r,m.get(m.de+m.dl['state'],4),m.get(m.de+m.dl['error'],4),m.get(m.de+m.dl['step'],4),m.get(m.app+m.af['stage'],4),m.get(m.fb+m.fl['failed'],4),m.events[-6:])
  if m.get(m.app+m.af['pmic_park_request'],4):break
 assert m.get(m.app+m.af['pmic_park_request'],4)==1 and m.get(m.app+m.af['state'],4)!=2
 m.task=m.get(m.app+m.af['handles']);m.call('pmic_task',0);assert m.stopped and m.get(m.app+m.af['pmic_parked'],4)==1
 m.stopped=False;m.task=m.get(m.app+m.af['handles']+8);assert m.call('gpu_display_output_step')==1
 assert m.get(m.app+m.af['state'],4)==2 and not m.get(m.app+m.af['fatal'],4)
 # Stable completion stays observable beyond the original absolute lifetime.
 m.ticks=(m.deadline+1)*24;assert m.call('gpu_display_output_step')==1
 results=dict(passed=True,scope=__doc__,physical_access=False,image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(),queue_updates=m.queue,presented=[1,2,1],events=m.events,
  frame_end_races=dict(step4=m.races[0],step17=m.races[1]),
  cases=['native-tv0-setup-iommu','three-paired-rcq-and-frame-end-observations','free-running-frame-end-at-clear-readbacks','hdmi-stop-before-de-disable','actual-pmic-owner-park-before-success','stable-terminal-success-after-expiry'])
 (a.output/'replay.json').write_text(json.dumps(results,indent=2)+'\n');print('PASS healthy HDMI/DE stop and PMIC owner acknowledgment',flush=True)
 # A separate pending submission reaches its original stop deadline without
 # an acquired final frame-end. No drain/reset/recycle is inferred.
 f=Machine(e,a.image,a.checkpoint,a.client);f.image_path=a.image;restore(f,a.output,'prepared')
 for i in range(100):
  assert f.call('gpu_display_output_step')<0x80000000;f.ticks+=2400
  if f.queue:break
 assert f.queue==1 and f.get(f.de+f.dl['state'],4)==0
 f.ticks=f.get(f.app+f.af['stop_at'])*24
 for i in range(30):
  r=f.call('gpu_display_output_step');f.ticks+=2400
  if r>=0x80000000:break
 assert r>=0x80000000 and f.get(f.app+f.af['fatal'],4)==1 and f.get(f.app+f.af['state'],4)==3
 pool=f.get(f.fb+f.fl['pool']);slot=f.get(f.fb+f.fl['lease']+e.layouts['device_memory_lease']['slot'],4);allocation=pool+e.layouts['device_memory_pool']['allocations']+slot*32
 assert f.get(allocation+e.layouts['device_memory_allocation']['state'],4)==3
 before=len(f.events);assert f.call('gpu_display_output_step')>=0x80000000 and len(f.events)==before
 (a.output/'uncertain.json').write_text(json.dumps(dict(passed=True,physical_access=False,scope=__doc__,image_sha256=results['image_sha256'],case='pending-replay-expiry-retains-bank-and-refuses-later-mmio'),indent=2)+'\n');print('PASS pending replay expiry retains ownership')
if __name__=='__main__':main()
