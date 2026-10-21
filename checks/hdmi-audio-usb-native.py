#!/usr/bin/env python3
"""New copied HDMI-audio observation only; prior finite disposition is a model seed.

Actual leaf construction, USB shared providers, mailbox and TinyUSB execute.
Architectural services/RTOS/PMIC bus/DCD effects are modeled; no PCM DMA job,
I2S/sampler interval, CPU startup, hardware, queue or physical visibility test.
"""
import argparse,copy,gzip,hashlib,importlib.util,json,pickle,struct
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0
ROOT=Path(__file__).resolve().parents[1]
def module(name,p):
 s=importlib.util.spec_from_file_location(name,p);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
n=module('audio_observation_native',ROOT/'checks/hdmi-audio-native.py')
t=module('audio_observation_usb',ROOT/'checks/deinterlace-usb.py')
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def local(image,unit,name):
 found=set()
 with image.open('rb') as f:
  for cu in ELFFile(f).get_dwarf_info().iter_CUs():
   if not cu.get_top_DIE().attributes['DW_AT_name'].value.decode().endswith(unit):continue
   for d in cu.iter_DIEs():
    if d.tag!='DW_TAG_subprogram' or 'DW_AT_low_pc' not in d.attributes:continue
    q=d
    if 'DW_AT_name' not in q.attributes and 'DW_AT_abstract_origin' in q.attributes:q=q.get_DIE_from_attribute('DW_AT_abstract_origin')
    if q.attributes.get('DW_AT_name') and q.attributes['DW_AT_name'].value.decode()==name:found.add(int(d.attributes['DW_AT_low_pc'].value))
 assert len(found)==1,(unit,name,found);return found.pop()
class Native(n.Machine):
 def __init__(self,*a):
  super().__init__(*a);self.u.mem_map(0x04100000,0x10000);self.regions.append((0x04100000,0x10000))
  # New USB provider readbacks absent from the older finite-audio fixture.
  for address,value in {0x070101ac:self.registers.get(0x070101ac,0)|1,0x020025a4:0x30001,0x020025c0:1<<9,0x020025e0:1<<31}.items():
   self.registers[address]=value;self.put(address,value,4)
  self.power.pmic.update({0x11:self.power.pmic[0x11]|1,0x1a:70,0x23:8,0x3f:16})
  self.usb_init=self.i.address('tusb_rhport_init');self.u.hook_add(UC_HOOK_CODE,self.code,begin=self.usb_init,end=self.usb_init)
 def code(self,u,at,size,unused):
  if at==getattr(self,'usb_init',None):self.stop('usb-prepared');return
  return super().code(u,at,size,unused)
def construct(image,session,contract,pcm,edid,prior,out):
 i=n.Image(image);m=Native(i,session,contract,pcm,edid);cl=i.layouts['a733_smp_contract'];m.time=struct.unpack_from('<Q',contract,cl['armed_at'])[0]//24+100
 assert m.call('a733_smp_contract_valid',0x42004000,m.time*24)==1
 assert m.call('hdmi_session_prepare',0x42004000)==1
 m.call('a733_smp_diagnostic_start',budget=100000000);assert m.boundary==('scheduler',0) and len(m.tasks)==4
 app=i.address('app');af=i.layouts['hdmi_application'];nf=i.layouts['hdmi_audio_native'];audio=i.address('audio');uf=i.layouts['hdmi_audio_usb'];usb=i.address('usb')
 handles=[m.get(app+af['handles']+j*8) for j in range(3)]
 assert m.call('a733_storage_native_task_privileged',i.address('hdmi_audio_usb_task'),0)==1
 assert m.call('a733_storage_native_task_privileged',i.address('hdmi_audio_usb_task'),1)==0
 m.scheduler=2
 for _ in range(2000):
  m.task=handles[0];m.call('pmic_task')
  m.task=handles[2];m.call('hdmi_audio_usb_task')
  if m.boundary==('usb-prepared',0):break
  assert not m.get(app+af['fatal'],4),m.trace[-15:]
  m.time+=1000
 else:raise AssertionError('USB provider bound')
 assert m.get(usb+uf['rail_index'],4)==4 and not m.dma_started
 assert not any(e[2] in (0x0253501c,0x04601100) for e in m.events),'PCM/DMA replay prohibited'
 result=i.address('hdmi_audio_result');initial=bytes(m.u.mem_read(result,160));assert initial[128:]==hashlib.sha256(session).digest()
 prepared=bytes(m.u.mem_read(0x41000000,0x3000000));(out/'prepared-ram.gz').write_bytes(gzip.compress(prepared,mtime=0))
 old=prior.read_bytes();assert len(old)==160 and old[96:128]==initial[96:128]
 assert struct.unpack_from('<2Q',old,32)==struct.unpack_from('<2Q',initial,32)
 # Only accepted disposition fields are restored. New constructor bound all
 # identities and memory; the actual unchanged publisher seals this model seed.
 m.u.mem_write(result+48,old[48:72]);m.task=handles[1]
 i.symbols['audio_result_publish']={local(image,'/apps/hdmi-audio/native.c','publish')}
 assert m.call('audio_result_publish',2)==1 and m.get(audio+nf['published'],4)==1
 m.put(app+af['state'],2,4);terminal=bytes(m.u.mem_read(0x41000000,0x3000000));(out/'terminal-ram.gz').write_bytes(gzip.compress(terminal,mtime=0))
 raw=bytes(m.u.mem_read(result,160));(out/'result.bin').write_bytes(raw)
 assert raw[128:]==hashlib.sha256(session).digest() and raw[96:128]==hashlib.sha256(pcm).digest()
 value=dict(image=sha(image),session=hashlib.sha256(session).hexdigest(),time=m.time,prepared_sha256=sha(out/'prepared-ram.gz'),terminal_sha256=sha(out/'terminal-ram.gz'),provider_writes=len(m.events),dma_submissions=0,prior_result=sha(prior),handles=handles,scope=__doc__)
 (out/'producer.json').write_text(json.dumps(value,indent=2)+'\n');return value
class USB(t.Machine):
 def __init__(self,image,types,sizes,enums,speed,raw,time,handles,terminal=False):
  image.symbols['board_usb0_task']={image.address('hdmi_audio_usb_task')}
  super().__init__(image,types,sizes,enums,speed);self.cpu.mem_write(0x41000000,raw);self.now=time;self.current=handles[2];self.handles=handles
  self.custom={image.address('xTaskGetCurrentTaskHandle'):'task',image.address('a733_counter_clock_now'):'clock',image.address('vTaskSuspend'):'park'}
  self.custom[local(image.path,'/apps/hdmi-audio/usb.c','providers')]='healthy-provider'
  self.custom[image.address('a733_usb_platform_poll')]='prepared-platform'
  for at in self.custom:self.cpu.hook_add(UC_HOOK_CODE,self.hook,begin=at,end=at)
 def hook(self,cpu,at,size,unused):
  name=getattr(self,'custom',{}).get(at)
  if name:
   a=[cpu.reg_read(UC_ARM64_REG_X0+j) for j in range(3)]
   if name=='task':self.ret(self.current)
   elif name=='clock':self.now+=1;self.write(a[1],self.now,8);self.ret(1)
   elif name=='park':self.stop='park';cpu.emu_stop()
   elif name=='prepared-platform':self.ret(2)
   else:self.ret(1)
   return
  return super().hook(cpu,at,size,unused)
 def poll(self):
  self.current=self.handles[1];self.call('hdmi_audio_observation_poll');self.current=self.handles[2]
 def transfer(self,request):
  first=len(self.transfers);self.request(request)
  for _ in range(10):
   self.tick();self.poll()
   if 0x81 in self.pending:break
  assert 0x81 in self.pending
  self.complete(0x81);self.tick();assert 0x81 in self.pending and self.pending[0x81]['count']==0
  assert self.word(self.image.address('observation'))==3
  self.complete(0x81);self.tick();assert self.word(self.image.address('observation'))==0
  data=b''.join(self.transfers[first:]);assert len(data)==512;return data
 def tick(self):return super().tick()
def main():
 p=argparse.ArgumentParser(description=__doc__)
 for name in ('image','session','contract','pcm','edid','prior-result','output'):p.add_argument('--'+name,type=Path,required=True)
 p.add_argument('--resume',action='store_true');a=p.parse_args();a.output.mkdir(parents=True,exist_ok=a.resume)
 raw=a.session.read_bytes();contract=a.contract.read_bytes();pcm=a.pcm.read_bytes();assert len(raw)==816 and len(contract)==224
 identity=dict(image=sha(a.image),session=sha(a.session),contract=sha(a.contract),pcm=sha(a.pcm));path=a.output/'result.json'
 record=json.loads(path.read_text()) if path.exists() else dict(identity=identity,cases={},physical_access=False,scope=__doc__)
 assert record['identity']==identity,'Refuse stale image/session evidence'
 def passed(name,**detail):record['cases'][name]=dict(passed=True,check_source_sha256=sha(__file__),**detail);path.write_text(json.dumps(record,indent=2)+'\n');print('PASS',name,flush=True)
 producer=a.output/'producer.json'
 if not producer.exists():v=construct(a.image,raw,contract,pcm,a.edid.read_bytes(),a.prior_result,a.output);passed('constructor-usb-provider-and-publication',dma_submissions=0)
 else:
  v=json.loads(producer.read_text());assert v['image']==identity['image'] and v['session']==identity['session'];assert sha(a.output/'prepared-ram.gz')==v['prepared_sha256'] and sha(a.output/'terminal-ram.gz')==v['terminal_sha256']
 image=t.base.Image(a.image,layout_names=());image.path=a.image;types,sizes,enums=t.metadata(a.image)
 epochs=struct.unpack_from('<2Q',raw,24);request=struct.pack('<4I2Q',0x4f414748,1,1,32,*epochs)
 for speed,label,terminal in [(1,'pending-full',False),(1,'terminal-full',True),(2,'terminal-high',True)]:
  if label in record['cases']:continue
  state=gzip.decompress((a.output/('terminal-ram.gz' if terminal else 'prepared-ram.gz')).read_bytes());m=USB(image,types,sizes,enums,speed,state,v['time'],v['handles']);m.enumerate();wire=m.transfer(request)
  assert struct.unpack_from('<4I',wire)==(0x52414748,1,512,1) and wire[64:96]==raw[48:80] and wire[96:128]==hashlib.sha256(raw).digest()
  assert struct.unpack_from('<I',wire,40)[0]==int(terminal)
  if terminal:assert wire[128:288]==(a.output/'result.bin').read_bytes()
  else:assert not any(wire[128:288])
  (a.output/(label+'.bin')).write_bytes(wire);passed(label,actual_tinyusb=True,terminal_zlp=True)
 # New ownership/parser boundaries use saved constructor state; no device job.
 layouts=n.Image(a.image).layouts
 app=image.address('app');af=layouts['hdmi_application'];obs=image.address('observation')
 def fork(terminal=True):
  data=gzip.decompress((a.output/('terminal-ram.gz' if terminal else 'prepared-ram.gz')).read_bytes())
  return USB(image,types,sizes,enums,1,data,v['time'],v['handles'])
 def admit(m,data=request,deadline=None):
  m.cpu.mem_write(0x90d00000,data)
  return m.call('hdmi_audio_observation_request',0x90d00000,len(data),deadline or m.now+100000)
 if 'parser-and-mailbox-ownership' not in record['cases']:
  m=fork();before=bytes(m.cpu.mem_read(obs,600))
  for offset in (0,4,8,12,16,24):
   bad=bytearray(request);bad[offset]^=1
   assert admit(m,bytes(bad))==0 and bytes(m.cpu.mem_read(obs,600))==before
  m.current=v['handles'][1];assert admit(m)==0;m.current=v['handles'][2]
  assert admit(m)==1;held=bytes(m.cpu.mem_read(obs,600));assert admit(m)==0 and bytes(m.cpu.mem_read(obs,600))==held
  m.poll();assert m.word(obs)==2;copied=bytes(m.cpu.mem_read(obs+layouts['hdmi_audio_observation_mailbox']['reply'],512))
  assert admit(m)==0 and bytes(m.cpu.mem_read(obs+layouts['hdmi_audio_observation_mailbox']['reply'],512))==copied
  passed('parser-and-mailbox-ownership',malformed_fields=6,wrong_owner=True,inflight_immutable=True)
 if 'healthy-transport-retained-fault-snapshot' not in record['cases']:
  m=fork();m.write(app+af['state'],3);assert admit(m)==1;m.poll()
  reply=bytes(m.cpu.mem_read(obs+layouts['hdmi_audio_observation_mailbox']['reply'],512));assert struct.unpack_from('<I',reply,40)[0]==2
  assert reply[128:288]==(a.output/'result.bin').read_bytes();passed('healthy-transport-retained-fault-snapshot',all_faults_capturable=False)
 if 'late-terminal-zlp-retains-copy' not in record['cases']:
  m=fork();m.enumerate();m.request(request);m.poll();m.tick();assert 0x81 in m.pending
  m.complete(0x81);m.tick();assert m.pending[0x81]['count']==0 and m.word(obs)==3
  retained=bytes(m.cpu.mem_read(obs+layouts['hdmi_audio_observation_mailbox']['reply'],512));m.now+=1000001
  m.complete(0x81)
  try:m.tick()
  except AssertionError:assert m.stop=='park'
  else:raise AssertionError('late completion must park')
  assert m.word(obs)==3 and bytes(m.cpu.mem_read(obs+layouts['hdmi_audio_observation_mailbox']['reply'],512))==retained
  passed('late-terminal-zlp-retains-copy',no_reclaim=True)
 record['passed']=True;path.write_text(json.dumps(record,indent=2)+'\n')
if __name__=='__main__':main()
