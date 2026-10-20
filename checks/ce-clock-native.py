#!/usr/bin/env python3
"""Execute fixed CE provisioning and one existing finite SHA operation.

The linked registry, mutex adapter, sealed-session consumer, allocator, clock
provider, direct native MMIO/cache adapter and CE clients execute. Board readiness,
RTOS APIs, translations/cache visibility, inherited clocks and device completion
are modeled. The SHA device result is injected, not computed by native hardware.
No CPU startup, PMIC, USB controller, physical permission, timing or drain proof.
"""
import argparse,hashlib,importlib.util,json,pickle,struct
from unittest.mock import patch
from pathlib import Path
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR,UC_ARM64_REG_PSTATE,UC_ARM64_REG_FP,UC_ARM64_REG_XZR
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def save(p,j):p.write_text(json.dumps(j,indent=2)+'\n')
image_module=module('ce_image',ROOT/'checks/storage-read-native.py')
prep=module('ce_prepare',ROOT/'apps/crypto/prepare.py')
providers=module('ce_provider_inputs',ROOT/'checks/a733-provider-fixture.py')
observe=module('ce_observe',ROOT/'apps/crypto/observe.py')
CCU=0x02002000;NS=0x04603000;SEC=0x04603800
class Machine:
 def __init__(self,image,raw,request,mode='nominal'):
  self.image=image;self.raw=raw;self.request=request;self.mode=mode;self.u=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
  self.u.mem_map(0x41000000,0x03000000);self.u.mem_map(0x02000000,0x06000000)
  for at,data in image.parts:self.u.mem_write(at,data)
  self.u.reg_write(UC_ARM64_REG_PSTATE,5)
  self.app=image.address('crypto');self.af=image.layouts['crypto_application'];self.board=image.address('board');self.bf=image.layouts['board_application']
  self.shared=image.address('providers')+image.layouts['board_provider']['shared'];self.sf=image.layouts['a733_native_usb']
  self.clock=self.app+self.af['clock'];self.cf=image.layouts['a733_ce_clock'];self.engine=self.app+self.af['engine'];self.ef=image.layouts['a733_ce_session']
  self.time=1000000;self.task=1;self.holders={};self.trace=[];self.mmio=[];self.cache=[];self.clock_writes=0;self.starts=0;self.due=None
  self.incoming=bytearray();self.tx=bytearray();self.done=False;self.translations=[];self.register_mutex=0;self.contexts={};self.case="setup";self.phase="boundary";self.callname="";self.call_args=();self.mmio_in_progress=False
  self.regs={0x07090160:0x10000002,CCU:0xf8000000|(99<<8)|(99<<16),CCU+0xa0:0xfe000000|(99<<8)|(1<<20),
   CCU+0x1908:0x08060806,CCU+0x1988:0x08060000,CCU+0x580:0x80000000,CCU+0x588:0x80000000,
   CCU+0x5e4:0x40303,CCU+0xac0:0x00800000,CCU+0xac4:0xa5000000}
  self.u.mem_write(0x41f00000,raw);self.u.mem_write(self.board,raw[:592]);self.u.mem_write(self.board+self.bf['admitted_session'],raw[:592]);self.put(self.board+160,61000000,8)
  contract=bytearray(224);struct.pack_into('<Q',contract,0,prep.CONTRACT_MAGIC);struct.pack_into('<Q',contract,88,25000000)
  struct.pack_into('<2Q',contract,176,0x41f00000,824);contract[192:224]=hashlib.sha256(raw).digest();self.u.mem_write(0x43e00000,bytes(contract))
  if image.symbols.get('ullCriticalNesting'):
   self.put(image.address('ullCriticalNesting'),0,8);self.put(image.address('ullPortInterruptNesting'),0,8)
  names=['board_live','a733_smp_counter','a733_counter_clock_now','a733_cpu_coherence_publish','aarch64_device_translate',
   'xTaskGetSchedulerState','xTaskGetCurrentTaskHandle','xQueueGenericCreateStatic','xQueueCreateMutexStatic','xTaskCreateStatic',
   'xQueueGetMutexHolder','xQueueSemaphoreTake','xQueueGenericSend','vTaskDelay','platform_panic',
   'a733_native_usb_step','a733_native_usb_health','tusb_rhport_init','tud_task_ext','tud_vendor_n_available','tud_vendor_n_read','tud_vendor_n_write','tud_vendor_n_write_flush']
  self.hooks={image.address(n):n for n in names if image.symbols.get(n)};self.instructions={}
  for at,data in image.parts:
   for off in range(0,len(data)-3,4):
    word=int.from_bytes(data[off:off+4],'little');masked=word&0xffffffe0
    if masked in (0xd5384240,0xd53b0020,0xd5381000,0xd53be000,0xd53be020,0xd50b7a20,0xd50b7e20,0xd5087620):self.instructions[at+off]=word
  for at in set(self.hooks)|set(self.instructions)|{0x43fff000}:self.u.hook_add(UC_HOOK_CODE,self.code,begin=at,end=at)
  self.u.hook_add(UC_HOOK_MEM_READ,self.read_mmio,begin=0x02000000,end=0x07ffffff)
  self.u.hook_add(UC_HOOK_MEM_WRITE,self.write_mmio,begin=0x02000000,end=0x07ffffff)
 def put(self,a,v,n=4):self.u.mem_write(a,int(v).to_bytes(n,'little'))
 def get(self,a,n=4):return int.from_bytes(self.u.mem_read(a,n),'little')
 def ret(self,v=0):self.u.reg_write(UC_ARM64_REG_X0,v&((1<<64)-1));self.u.reg_write(UC_ARM64_REG_PC,self.u.reg_read(UC_ARM64_REG_LR))
 def code(self,u,at,size,unused):
  if at==0x43fff000:self.done=True;u.emu_stop();return
  if at in self.instructions and at not in self.hooks:
   word=self.instructions[at];kind=word&0xffffffe0;index=word&31;reg=UC_ARM64_REG_FP if index==29 else UC_ARM64_REG_LR if index==30 else UC_ARM64_REG_XZR if index==31 else UC_ARM64_REG_X0+index
   if kind in (0xd50b7a20,0xd50b7e20,0xd5087620):self.cache.append([kind,u.reg_read(reg)])
   else:u.reg_write(reg,{0xd5384240:4,0xd53b0020:0x04040004,0xd5381000:5,0xd53be000:24000000,0xd53be020:self.time*24}[kind])
   u.reg_write(UC_ARM64_REG_PC,at+4);return
  name=self.hooks[at];a=[u.reg_read(UC_ARM64_REG_X0+i) for i in range(8)];self.trace.append(name)
  if name=='board_live':self.ret(not(self.mode=='authority-loss' and self.clock_writes>=2))
  elif name=='a733_smp_counter':self.ret(self.time*24)
  elif name=='a733_counter_clock_now':self.put(a[1],self.time,8);self.ret(1)
  elif name=='a733_cpu_coherence_publish':self.ret(1)
  elif name=='aarch64_device_translate':
   attr=0x44 if 0x41c00000<=a[0]<0x41e00000 else 0xff if a[0]>=0x41000000 else 0
   self.u.mem_write(a[2],struct.pack('<QII',a[0],attr,3 if attr else 2));self.translations.append(a[0]);self.ret(1)
  elif name=='xTaskGetSchedulerState':self.ret(2)
  elif name=='xTaskGetCurrentTaskHandle':self.ret(self.task)
  elif name in ('xQueueGenericCreateStatic','xQueueCreateMutexStatic'):self.ret(a[3] if name=='xQueueGenericCreateStatic' else a[1])
  elif name=='xTaskCreateStatic':self.ret(a[6])
  elif name=='xQueueGetMutexHolder':self.ret(self.holders.get(a[0],0))
  elif name=='xQueueSemaphoreTake':
   assert not self.holders.get(a[0],0);self.holders[a[0]]=self.task;self.ret(1)
  elif name=='xQueueGenericSend':assert self.holders[a[0]]==self.task;self.holders[a[0]]=0;self.ret(1)
  elif name=='vTaskDelay':self.time+=a[0]*1000;self.ret();self.done=True;u.emu_stop()
  elif name=='platform_panic':raise AssertionError(('native panic',a,self.trace[-20:]))
  elif name in ('a733_native_usb_step','a733_native_usb_health','tusb_rhport_init','tud_task_ext'):self.ret(1)
  elif name=='tud_vendor_n_available':self.ret(len(self.incoming))
  elif name=='tud_vendor_n_read':
   count=min(a[2],len(self.incoming));u.mem_write(a[1],bytes(self.incoming[:count]));del self.incoming[:count];self.ret(count)
  elif name=='tud_vendor_n_write':self.tx.extend(u.mem_read(a[1],a[2]));self.ret(a[2])
  elif name=='tud_vendor_n_write_flush':self.ret(0)
  else:raise AssertionError(name)
 def read_mmio(self,u,access,at,size,value,unused):
  self.mmio_in_progress=True
  assert size==4
  if at!=0x03400118:assert self.holders.get(self.register_mutex)==self.task,('unlocked MMIO',hex(at))
  if at==NS+12 and self.due is not None and self.time>=self.due:self.regs[at]=16
  self.mmio.append(['r',at,self.regs.get(at,0)]);self.put(at,self.regs.get(at,0));self.mmio_in_progress=False
 def write_mmio(self,u,access,at,size,value,unused):
  self.mmio_in_progress=True
  assert size==4 and self.holders.get(self.register_mutex)==self.task
  self.mmio.append(['w',at,value]);old=self.regs.get(at,0)
  if at in (CCU+0xac0,CCU+0xac4,CCU+0x5e4):
   self.clock_writes+=1
   if self.mode=='readback-fault' and self.clock_writes==2:self.mmio_in_progress=False;return
   if self.mode=='expiry' and self.clock_writes==2:self.time=61000000
  self.regs[at]=value
  if at==NS+12:self.regs[at]=old&~value;self.due=None
  if at==NS+0x10:
   assert value==0x400 and self.clock_writes==6 and not self.starts
   assert (self.regs[CCU+0xac0]&0x8700001f)==0x81000000 and (self.regs[CCU+0xac4]&0x30003)==0x30003
   pointer=self.regs[NS]|((self.regs.get(NS+4,0)&255)<<32);d=bytes(u.mem_read(pointer,256));le=lambda o,n:int.from_bytes(d[o:o+n],'little')
   assert le(0,4)==0x10002 and le(4,4)==3 and le(29,5)==pointer+256 and le(40,4)==32
   payload=self.request[48:];padded=payload+b'\x80'+bytes((-(len(payload)+9))%64)+(len(payload)*8).to_bytes(8,'big')
   assert le(24,5)==pointer+384 and le(36,4)==len(padded)
   assert bytes(u.mem_read(pointer+384,len(padded)))==padded
   assert all([0xd50b7a20,pointer+off] in self.cache for off in range(0,256,64))
   assert all([0xd50b7a20,pointer+off] in self.cache for off in range(320,2432,64))
   assert [0xd50b7e20,pointer+256] in self.cache
   u.mem_write(pointer+256,hashlib.sha256(payload).digest());self.starts+=1;self.due=self.time+1500;self.regs[at]=0
  self.mmio_in_progress=False
 def call(self,name,*args,budget=20000000,unit=None):
  prior_task=self.task
  if name=='board_usb0_task':self.task=2 # Modeled separate USB task identity.
  self.callname=name;self.call_args=args;self.phase='running';self.done=False
  if name in self.contexts:
   self.u.context_restore(self.contexts[name]);entry=self.u.reg_read(UC_ARM64_REG_PC)
  else:
   stack=self.app+self.af['stack']+16384 if name=='crypto_task' else 0x43fc0000 if name=='board_usb0_task' else 0x43fd0000
   self.u.reg_write(UC_ARM64_REG_SP,stack&~15);self.u.reg_write(UC_ARM64_REG_LR,0x43fff000)
   for i,v in enumerate(args):self.u.reg_write(UC_ARM64_REG_X0+i,v)
   entry=self.image.address(name,unit)
  self.u.emu_start(entry,0,count=budget)
  assert self.done,('instruction bound',name,hex(self.u.reg_read(UC_ARM64_REG_PC)),self.trace[-15:])
  if name in ('crypto_task','board_usb0_task'):self.contexts[name]=self.u.context_save()
  self.phase='boundary';self.task=prior_task
  return self.u.reg_read(UC_ARM64_REG_X0)&0xffffffff
 def snapshot(self):
  names=('time','holders','regs','task','due','starts','clock_writes','incoming','tx','mode','case','phase','callname','call_args','mmio_in_progress','mmio','cache','trace','contexts','done','register_mutex','translations')
  return dict(ram=bytes(self.u.mem_read(0x41000000,0x03000000)),**pickle.loads(pickle.dumps({k:getattr(self,k) for k in names})))
 def restore(self,s):
  self.u.mem_write(0x41000000,s['ram'])
  for key,value in pickle.loads(pickle.dumps(s)).items():
   if key!='ram':setattr(self,key,value)
 def checkpoint(self,out,label):
  out.mkdir(exist_ok=True,parents=True);state=self.snapshot();(out/(label+'-ram.bin')).write_bytes(state.pop('ram'));state['cpu']=self.u.context_save()
  state['resumable']=not self.mmio_in_progress;state['image_sha256']=sha(self.image.path)
  (out/(label+'.pickle')).write_bytes(pickle.dumps(state))


def typed_model_facts(output,image,raw):
 old=Path.home()/'.cache/sunstoneos/evidence/crypto-single-core-20261018/host/MODEL-NOT-DEPLOYABLE-bundle'
 prior=json.loads((old/'prepared.json').read_text());record=json.loads((old/'admission.json').read_text())
 facts=output/'MODEL-NOT-PHYSICAL';facts.mkdir();warning=facts/'MODEL.json'
 save(warning,dict(physical_access=False,warning='Synthetic CE clock software fixture. Never physical authority.'))
 ref=dict(path=str(warning.resolve()),sha256=sha(warning));identity='CE-CLOCK-SOFTWARE-MODEL';port='NO-PHYSICAL-PORT'
 def bind(value):
  if isinstance(value,dict):
   if set(value)=={'path','sha256'}:return ref.copy()
   return {k:(sha(image) if k=='image_sha256' else identity if k=='identity' else port if k=='port' else bind(v)) for k,v in value.items()}
  if isinstance(value,list):return [bind(v) for v in value]
  return value
 records={}
 for original,row in prior['entry_evidence'].items():
  name=Path(original).name
  if name=='MODEL.json':continue
  fact=bind(json.loads((old/row['name']).read_text()))
  if name=='clocks.json':
   fact.pop('ce_local_gates_resets_ready',None);fact.pop('observed_ce400_peri0',None)
   fact.update(ce_local_reset_held_gated=True,ce_mbus_gate_off_critical_neighbors_on=True,
    ce_local_clock_reset_write_authorized=True,mbus_ce_gate_write_authorized=True)
  path=facts/name;save(path,fact);records[name]=dict(path=str(path.resolve()),sha256=sha(path))
 record=bind(record)
 for field in ('ddr','platform','initial','memory','clocks','supervision'):record[field]=records[field+'.json']
 for offset,name in ((80,'platform.json'),(216,'initial.json'),(248,'ddr.json'),(280,'platform.json'),(632,'initial.json'),(664,'memory.json'),(696,'clocks.json'),(728,'supervision.json')):
  raw[offset:offset+32]=bytes.fromhex(records[name]['sha256'])
 record['session_template_sha256']=hashlib.sha256(raw).hexdigest();record['schema']='a733-crypto-supervised-entry-v1'
 save(output/'admission.json',record)
 prep.validate_admission(record,sha(image),hashlib.sha256(raw).hexdigest(),bytes(raw),execution_profile='crypto-single-core-supervised-v1')
 return record

def main():
 p=argparse.ArgumentParser(description=__doc__)
 for name in ('image','output'):p.add_argument('--'+name,type=Path,required=True)
 p.add_argument('--authenticated-checkpoint',type=Path)
 a=p.parse_args();assert not a.output.exists();a.output.mkdir(parents=True)
 names=('crypto_application','board_application','board_provider','a733_native_usb','a733_platform_locks','a733_ce_clock','a733_ce_session','a733_ce_config','a733_ce','a733_clock_token','crypto_run')
 image=image_module.Image(a.image,layout_names=names);image.path=a.image
 source=Path.home()/'.cache/sunstoneos/evidence/crypto-single-core-20261018/session-fixture.bin'
 if a.authenticated_checkpoint:
  previous=a.authenticated_checkpoint.resolve().parent
  assert a.authenticated_checkpoint.resolve()==previous/'authenticated-before-clock.pickle'
  manifest=json.loads((previous/'manifest.json').read_text())
  assert manifest['image_sha256']==sha(a.image)
  assert {'authenticated-before-clock.pickle','authenticated-before-clock-ram.bin','session.bin','operation.bin'}<=set(manifest['files'])
  for name,row in manifest['files'].items():
   file=(previous/name).resolve();assert previous in file.parents
   assert file.stat().st_size==row['bytes'] and sha(file)==row['sha256']
  raw=(previous/'session.bin').read_bytes();request=(previous/'operation.bin').read_bytes()
  admission_record=json.loads((previous/'admission.json').read_text())
 else:
  raw=bytearray(providers.nominal_provider_session(source.read_bytes(),usb_reference_mode=2))
  _,_,build=prep.base.image_parts(a.image);raw[48:80]=build
  # Same actual producers and fixed provider fields; all authority is MODEL-only.
  for offset in (24,32,40):struct.pack_into('<Q',raw,offset,0x4345000000000000+offset)
  struct.pack_into('<Q',raw,600,3);struct.pack_into('<2Q',raw,616,12,13)
  request=prep.operation.encode(1,0xce00000000000001,b'cold clock request through the existing SHA client')
  raw[760:792]=hashlib.sha256(request).digest();raw[792:824]=bytes(32)
  admission_record=typed_model_facts(a.output,a.image,raw)
 raw=bytes(raw);providers.validate_nominal_providers(raw,2);prep.session_check(raw);prep.operation.decode(request,raw)
 (a.output/'session.bin').write_bytes(raw);(a.output/'operation.bin').write_bytes(request);save(a.output/'admission.json',admission_record)
 inputs={str(f.resolve()):sha(f) for f in (a.image,Path(__file__),ROOT/'checks/storage-read-native.py',ROOT/'checks/a733-provider-fixture.py',ROOT/'apps/crypto/prepare.py',ROOT/'apps/crypto/request.py',ROOT/'apps/crypto/observe.py',ROOT/'tools/check-a733-crypto-supervised.py',source,providers.SOURCE)}
 for f in (Path.home()/'.cache/sunstoneos/evidence/crypto-single-core-20261018/host/MODEL-NOT-DEPLOYABLE-bundle').glob('*.json'):inputs[str(f.resolve())]=sha(f)
 if a.authenticated_checkpoint:
  for f in (a.authenticated_checkpoint,previous/'authenticated-before-clock-ram.bin',previous/'manifest.json',previous/'inputs.json'):inputs[str(f.resolve())]=sha(f)
 save(a.output/'inputs.json',inputs);m=Machine(image,raw,request);results={}
 try:
  if a.authenticated_checkpoint:
   state=pickle.loads(a.authenticated_checkpoint.read_bytes());assert state['resumable'] and state['image_sha256']==sha(a.image)
   state['ram']=(previous/'authenticated-before-clock-ram.bin').read_bytes()
   for name in ('cpu','resumable','image_sha256'):state.pop(name)
   m.restore(state)
   assert m.phase=='boundary' and m.callname=='board_usb0_task' and not any(m.holders.values()) and not m.mmio_in_progress
   assert m.task==m.get(m.app+m.af['handle'],8) and m.task
   assert m.starts==0 and m.due is None and m.clock_writes==0 and m.mode=='nominal'
   assert m.get(m.clock+m.cf['state'])==0
   request_address=m.app+m.af['run']+image.layouts['crypto_run']['request']
   assert bytes(m.u.mem_read(request_address,len(request)))==request
   # The saved pre-input iteration only waited. Re-enter this task body's clean
   # loop entry on its aligned model stack; preserve all admitted/provider and
   # authenticated request bytes. No native task context constructor is modeled.
   m.contexts.pop('crypto_task',None)
   assert m.af['handle']-m.af['stack']==16384
   ll=image.layouts['a733_platform_locks'];m.register_mutex=m.get(m.shared+m.sf['locks']+ll['mutex'],8)
   assert m.get(m.app+m.af['input_ready'])==1 and not m.get(m.app+m.af['token'],8)
   assert not m.get(m.engine+m.ef['initialized']) and not m.clock_writes
   assert bytes(m.u.mem_read(0x41f00000,824))==raw
   from elftools.elf.elffile import ELFFile
   with a.image.open('rb') as stream:
    for segment in ELFFile(stream).iter_segments():
     if segment['p_type']=='PT_LOAD' and not(int(segment['p_flags'])&2):
      assert bytes(m.u.mem_read(int(segment['p_vaddr']),len(segment.data())))==segment.data()
  else:
   assert m.call('crypto_session_prepare',0x43e00000)==1
   assert m.call('a733_clock_leases_init',m.shared+m.sf['clocks'],image.address('a733_counter_clock_now'),m.shared+m.sf['counter'])==1
   assert m.call('a733_platform_locks_init',m.shared+m.sf['locks'],image.address('a733_counter_clock_now'),m.shared+m.sf['counter'])==1
   ll=image.layouts['a733_platform_locks'];m.register_mutex=m.get(m.shared+m.sf['locks']+ll['mutex'],8)
   assert m.call('board_application_start')==1;m.task=m.get(m.app+m.af['handle'],8)
   assert m.task and m.call('crypto_task')>=0 and not m.mmio and not m.get(m.app+m.af['token'],8)
   m.incoming.extend(b'\1'+struct.pack('<I',len(request))+request)
   m.call('board_usb0_task')
   assert not m.incoming and m.get(m.app+m.af['input_ready'])==1 and not m.mmio
  ready=m.snapshot();m.checkpoint(a.output,'authenticated-before-clock');results['deferred-until-authenticated-request']=True
  m.case='nominal-operation'
  for _ in range(40):
   m.call('crypto_task')
   if m.get(m.app+m.af['result']):break
  assert m.get(m.app+m.af['result'])==1 and m.get(m.clock+m.cf['state'])==2
  assert m.starts==1 and m.clock_writes==6
  assert [row[1] for row in m.mmio if row[0]=='w' and CCU<=row[1]<CCU+0x2000]==[CCU+0xac0,CCU+0xac4,CCU+0xac4,CCU+0xac0,CCU+0x5e4,CCU+0xac4]
  assert m.regs[CCU+0x5e4]==0x40307 and m.regs[CCU+0xac4]==0xa5030003 and m.regs[CCU+0xac0]==0x81800000
  assert m.get(m.engine+m.ef['initialized'])==1 and not m.get(m.engine+m.ef['fault'])
  assert bytes(m.u.mem_read(m.app+m.af['payload'],32))==hashlib.sha256(request[48:]).digest()
  pointer=m.get(m.engine+m.ef['controller']+image.layouts['a733_ce']['memory'],8)
  assert [0xd5087620,pointer+256] in m.cache
  terminal=m.snapshot();m.checkpoint(a.output,'terminal');save(a.output/'nominal-mmio.json',m.mmio)
  m.incoming.extend(b'\0');m.call('board_usb0_task')
  response=image.address('crypto_response');count=image.address('crypto_response_bytes')
  # The accepted transport callback contract owns completion accounting; only
  # the changed v3 response serialization/capture is checked here.
  if m.incoming:
   m.call('tud_vendor_tx_cb',0,144);m.call('board_usb0_task')
  frame=bytes(m.u.mem_read(response,176));decoded=observe.decode(frame,raw,request=request)
  assert decoded['status']==1 and decoded['request_id']==0xce00000000000001
  (a.output/'reply.bin').write_bytes(frame);results['actual-cold-provider-existing-SHA-copied-output']=True
  for mode in ('ambiguous-local','missing-neighbor','invalid-parent','readback-fault','authority-loss','expiry'):
   m.restore(ready);m.mode=mode;m.case=mode
   if mode=='ambiguous-local':m.regs[CCU+0xac0]|=0x80000000
   if mode=='missing-neighbor':m.regs[CCU+0x5e4]&=~2
   if mode=='invalid-parent':m.regs[CCU]&=~0x08000000
   m.call('crypto_task');assert m.get(m.app+m.af['result'])==2 and m.get(m.clock+m.cf['state'])==3
   assert not m.get(m.engine+m.ef['initialized']) and not m.starts
   assert m.clock_writes==(0 if mode in ('ambiguous-local','missing-neighbor','invalid-parent') else 2)
   assert m.get(m.shared+m.sf['locks']+ll['retained'])&1
   before=len(m.mmio)
   assert m.call('a733_ce_clock_provision',m.clock,m.app+m.af['config'],m.clock+m.cf['ops'],m.app,m.shared+m.sf['clocks'],m.app+m.af['token'])==0
   assert len(m.mmio)==before
   results[mode+'-retained-no-retry']=True
  for label,address,value in [('wrong-address',CCU+0x140,0),('reset-assert',CCU+0xac4,0xa5000000),('neighbor-change',CCU+0x5e4,0x40305),('wrong-source',CCU+0xac0,0x82800000)]:
   m.restore(terminal);m.mode='nominal';m.case=label
   assert m.call('a733_platform_lock_enter',m.shared+m.sf['locks'],0,60000000)==1
   before=len([r for r in m.mmio if r[0]=='w'])
   assert m.call('a733_ce_clock_native_write',m.app,address,value)==0
   assert len([r for r in m.mmio if r[0]=='w'])==before
   m.call('a733_platform_lock_leave',m.shared+m.sf['locks'],0);results[label+'-native-write-refused']=True
  # Exercise the actual new host branch using the actual serialized result.
  capture=module('ce_capture',ROOT/'tools/check-a733-crypto-supervised.py');capture_record={};packets=[]
  def exchange(packet,timeout):packets.append(packet);return frame
  result=capture.capture(exchange,raw,a.output,capture_record,lambda:None,10,now=lambda:0,pause=lambda _:None,request=request)
  assert result['status']==1 and packets==[b'\1'+struct.pack('<I',len(request))+request]
  assert (a.output/'output.bin').read_bytes()==hashlib.sha256(request[48:]).digest();results['v3-host-upload-and-copied-offset']=True
  # New cold/write facts are mandatory even with all inherited facts present.
  cache=prep.validate_admission(admission_record,sha(a.image),hashlib.sha256(raw).hexdigest(),raw,execution_profile='crypto-single-core-supervised-v1')
  clock_path=admission_record['clocks']['path']
  for field in ('ce_local_reset_held_gated','ce_mbus_gate_off_critical_neighbors_on','ce_local_clock_reset_write_authorized','mbus_ce_gate_write_authorized'):
   fact=json.loads(cache[clock_path]);fact[field]=False;data=(json.dumps(fact,indent=2)+'\n').encode()
   altered=bytearray(raw);altered[696:728]=hashlib.sha256(data).digest();changed=json.loads(json.dumps(admission_record))
   changed['clocks']['sha256']=hashlib.sha256(data).hexdigest();changed['session_template_sha256']=hashlib.sha256(altered).hexdigest()
   try:prep.validate_admission(changed,sha(a.image),hashlib.sha256(altered).hexdigest(),bytes(altered),{**cache,clock_path:data},True,'crypto-single-core-supervised-v1')
   except ValueError as e:assert 'Required CRYPTO physical fact absent' in str(e)
   else:raise AssertionError('missing cold fact accepted: '+field)
   results[field+'-host-refused']=True
  # Deterministic MODEL nonces preserve the exact natively executed session;
  # production continues to use fresh secrets. No launcher instructions execute.
  identifiers=struct.unpack_from('<3Q',raw,24)
  with patch.object(prep.secrets,'randbits',side_effect=identifiers):
   prep.prepare(a.image,a.image.parent/'crypto-single-core/manifest.json',a.output/'admission.json',a.output/'session.bin',
    a.output/'MODEL-NOT-DEPLOYABLE-bundle','/usr/lib/llvm-21/bin/clang','/home/sunil/.cache/sunstoneos/host/usr/bin/ld.lld-21',request=a.output/'operation.bin')
  assert (a.output/'MODEL-NOT-DEPLOYABLE-bundle/session.bin').read_bytes()==raw
  results['typed-v3-bundle-same-native-session']=True
  save(a.output/'result.json',dict(passed=True,physical=False,image_sha256=sha(a.image),session_sha256=hashlib.sha256(raw).hexdigest(),cases=results,scope=__doc__))
  print('PASS fixed CE cold clock provisioning, one existing SHA operation and focused refusals')
 except BaseException as error:
  m.checkpoint(a.output,'stopped');save(a.output/'failure.json',dict(error=repr(error),trace=m.trace[-40:],physical=False,case=m.case,phase=m.phase,resumable=not m.mmio_in_progress));raise
 finally:
  for path,digest in inputs.items():assert sha(path)==digest
  files={str(f.relative_to(a.output)):dict(bytes=f.stat().st_size,sha256=sha(f)) for f in a.output.rglob('*') if f.is_file()}
  save(a.output/'manifest.json',dict(image_sha256=sha(a.image),files=files,inputs=inputs,physical=False))
if __name__=='__main__':main()
