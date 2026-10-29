#!/usr/bin/env python3
"""Actual linked UAC1 board consumer and serializer; scheduling/provider/API returns modeled.

The block input comes from the actual TinyUSB/UAC1/xHCI composition check.
No physical access, native controller MMIO, scheduler or microphone is modeled here.
"""
import argparse,hashlib,importlib.util,json,struct
from pathlib import Path
from unittest.mock import patch
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
Image=module('audio_native_image',ROOT/'checks/usb-host-hid-native.py').Image
sha=lambda data:hashlib.sha256(data).hexdigest()
def fixture(image,out):
 prep=module('audio_prepare',ROOT/'apps/board/prepare.py');provider=module('audio_provider',ROOT/'checks/a733-provider-fixture.py')
 baseline=Path.home()/'.cache/sunstoneos/evidence/board-supervised-20261017/batch1/session-fixture.bin'
 raw=provider.nominal_provider_session(baseline.read_bytes(),usb_reference_mode=1);elf=Image(image);fields=elf.layouts['a733_board_session']
 def put(name,value,size=4):struct.pack_into('<Q' if size==8 else '<I',raw,fields[name],value)
 for n,v in [('latch_identity',20),('latch_owner',21),('typec_owner',70),('usb2_owner',99)]:put(n,v,8)
 for n,v in [('workload',14),('latch_known_mask',0x3fff),('source_min_mv',5000),('source_max_mv',5100),('source_ma',500),('zero_threshold_max_mv',800),('fusb_device_id',0x80),('usb2_revision',0x3139302a),('pck_client',2)]:put(n,v)
 identity=prep.base.image_parts(image)[2];raw[48:80]=identity;ih=sha(image.read_bytes())
 target=dict(identity='USB-AUDIO-SOFTWARE-MODEL',port='NO-PHYSICAL-PORT',image_sha256=ih)
 def record(name,value):
  path=(out/name).resolve();data=json.dumps(value,sort_keys=True).encode();path.write_bytes(data);return dict(path=str(path),sha256=sha(data))
 generic=record('MODEL-NOT-PHYSICAL.json',dict(**target,physical_access=False,purpose='Software fixture only; no microphone or physical authority'))
 for offset in (80,216,248,280):raw[offset:offset+32]=bytes.fromhex(generic['sha256'])
 prep.session_check(raw);template=out/'session-template.bin';template.write_bytes(raw)
 conditions={'source-backed-cpu-dsu-transition','cold-cache-and-dirty-alias-exclusion','dual-pas-dram','exclusive-cpus-memory-dma','remote-writers-excluded','usb-clock-power-reset-ownership','usb-device-mapping','physical-counter-continuity'}
 platform=record('platform.json',dict(**target,schema='a733-smp-observer-platform-v1',entry_conditions_established=True,physical_coherence_result='under-test',conditions={k:generic for k in sorted(conditions)}))
 ddr=record('ddr.json',dict(**target,schema='a733-ddr-reservation-v1',exclusive=True,published_cache_disabled=True,remote_writers_excluded=True,ranges=[[0x41000000,0x42200000]],evidence=[generic]))
 admission=dict(**target,schema='a733-board-supervised-entry-v2',reservation=100,session_template_sha256=sha(raw),ddr=ddr,platform=platform,whole_board_power_removal=True,no_resource_reuse_before_power_removal=True,recovery=dict(mode='manual-usb-reconnect',demonstrated_fel=True,explicit_reconnect_before_resumption=True,preserve_microsd=True,one_attempt=True,evidence=[generic]))
 (out/'admission.json').write_text(json.dumps(admission,indent=2));manifest=dict(target='sunstone-a733-board',execution_profile='board-supervised-v2',final_elf=ih)
 (out/'image-manifest.json').write_text(json.dumps(manifest,indent=2))
 prep.prepare(image,out/'image-manifest.json',out/'admission.json',template,out/'MODEL-NOT-DEPLOYABLE-bundle','/usr/lib/llvm-21/bin/clang','/home/sunil/.cache/sunstoneos/host/usr/bin/ld.lld-21')
 return raw
class Machine:
 def __init__(self,image,session,block,variant):
  self.elf=image;self.variant=variant;self.cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);self.cpu.mem_map(0x40000000,0x04000000);self.cpu.mem_map(0x50000000,0x01000000)
  for a,data in image.parts:self.cpu.mem_write(a,data)
  self.board=image.address('board');self.layout=image.layouts['board_application'];self.host=self.board+self.layout['host'];self.native=image.layouts['a733_usb2_native'];self.core=self.host+self.native['core'];self.cf=image.layouts['a733_xhci'];self.audio=self.host+self.native['audio'];self.af=image.layouts['a733_usb2_audio'];self.block=block
  self.tx=[]
  self.cpu.mem_write(self.board,bytes(session));self.session=bytes(session);self.done=False;self.reads=0;self.polls=0;self.starts=0;self.live=True;self.response=False;self.now=1000
  config=self.host+self.native['platform']+image.layouts['a733_usb2_platform']['config'];self.cpu.mem_write(config+image.layouts['a733_usb2_platform_config']['lifetime_deadline'],struct.pack('<Q',10000000))
  self.cpu.mem_write(self.core+self.cf['generation'],block[:8]);self.cpu.mem_write(self.core+self.cf['ops']+image.layouts['a733_xhci_ops']['live'],struct.pack('<Q',image.address('board_live')));self.cpu.mem_write(self.core+self.cf['ops']+image.layouts['a733_xhci_ops']['now'],struct.pack('<Q',image.address('a733_counter_clock_now')))
  bf=image.layouts['a733_usb2_audio_block']
  for n in ('address','interface','endpoint'):self.put(self.audio+self.af[n],block[bf[n]])
  root=self.core+self.cf['devices'];device=image.layouts['a733_xhci_device']
  self.put(root+device['address'],block[bf['address']]);self.put(root+device['speed'],block[bf['speed']]);self.put(root+device['slot'],1)
  if variant=='hub':self.put(root+device['hub'],1);self.put(root+device['address'],2)
  self.put(self.host+self.native['audio_mounted'],1)
  names=('board_live','a733_counter_clock_now','a733_usb2_native_poll','a733_usb2_device_kind','a733_usb2_audio_start','a733_usb2_audio_read','vTaskDelay','a733_usb2_native_lost','a733_typec_revoke','a733_native_usb_step','a733_native_usb_health','tusb_rhport_init','tud_task_ext','tud_vendor_n_available','tud_vendor_n_read','tud_vendor_n_write','tud_vendor_n_write_flush')
  self.hooks={image.address(n):n for n in names};self.cpu.hook_add(UC_HOOK_CODE,self.hook)
 def put(self,a,value,size=4):self.cpu.mem_write(a,value.to_bytes(size,'little'))
 def result(self):return int.from_bytes(self.cpu.mem_read(self.board+self.layout['result'],4),'little')
 def ret(self,v):self.cpu.reg_write(UC_ARM64_REG_X0,v&((1<<64)-1));self.cpu.reg_write(UC_ARM64_REG_PC,self.cpu.reg_read(UC_ARM64_REG_LR))
 def hook(self,cpu,address,size,unused):
  if address==0x50fff000:self.done=True;cpu.emu_stop();return
  name=self.hooks.get(address)
  if not name:return
  args=[cpu.reg_read(UC_ARM64_REG_X0+i) for i in range(8)]
  if name=='board_live':self.ret(int(self.live))
  elif name=='a733_counter_clock_now':self.now+=100;self.put(args[1],self.now,8);self.ret(1)
  elif name=='a733_usb2_native_poll':
   self.polls+=1
   assert int.from_bytes(cpu.mem_read(self.host+self.native['audio_capture'],4),'little')==1
   if self.variant=='detach' and self.starts:self.put(self.host+self.native['audio_mounted'],0);self.ret(0);return
   if self.variant=='post-copy-detach' and self.result()==1:self.put(self.host+self.native['audio_mounted'],0);self.ret(0);return
   if self.starts and self.polls>2:self.put(self.audio+self.af['started'],1)
   if self.variant=='deadline' and self.starts:self.live=False
   self.ret(1)
  elif name=='a733_usb2_device_kind':self.ret(1 if self.variant=='wrong-kind' else 5)
  elif name=='a733_usb2_audio_start':
   self.starts+=1;assert self.starts==1 and args[1:4]==[16000,1,10000000];self.ret(0 if self.variant=='start-refused' else 1)
  elif name=='a733_usb2_audio_read':
   self.reads+=1
   if self.reads==1:self.ret(0);return
   cpu.mem_write(args[1],self.block)
   if self.variant=='read-failed':self.ret(-1);return
   if self.variant=='wrong-endpoint':self.put(args[1]+self.elf.layouts['a733_usb2_audio_block']['endpoint'],0x83,1)
   if self.variant=='lost-after-copy':self.live=False
   self.ret(1)
  elif name=='vTaskDelay':
   if self.result() in (1,2):self.done=True;cpu.emu_stop()
   else:self.ret(0)
  elif name=='tud_vendor_n_write':self.tx.append(bytes(cpu.mem_read(args[1],args[2])));self.ret(args[2])
  elif name=='tud_vendor_n_available':self.ret(int(self.response))
  elif name=='tud_vendor_n_read':cpu.mem_write(args[1],b'\0');self.response=False;self.ret(1)
  else:self.ret(1)
 def call(self,name,*args):
  self.done=False
  for i,v in enumerate(args):self.cpu.reg_write(UC_ARM64_REG_X0+i,v)
  self.cpu.reg_write(UC_ARM64_REG_SP,0x50ffeff0);self.cpu.reg_write(UC_ARM64_REG_LR,0x50fff000);self.cpu.emu_start(self.elf.address(name),0,count=3000000);assert self.done,(name,hex(self.cpu.reg_read(UC_ARM64_REG_PC)))
  return self.cpu.reg_read(UC_ARM64_REG_X0)&0xffffffff
 def frame(self):
  self.response=True;self.call('board_usb0_task',0);n=int.from_bytes(self.cpu.mem_read(self.elf.address('response_bytes','/apps/board/usb0.c'),4),'little');return bytes(self.cpu.mem_read(self.elf.address('response','/apps/board/usb0.c'),n))
def run(image_path,block_path,out):
 out.mkdir(parents=True,exist_ok=True);image=Image(image_path)
 if (out/'session-template.bin').exists():
  session=(out/'session-template.bin').read_bytes();module('audio_resume_prepare',ROOT/'apps/board/prepare.py').validate_bundle(out/'MODEL-NOT-DEPLOYABLE-bundle')
 else:session=fixture(image_path,out)
 raw=block_path.read_bytes();size=image.sizes['a733_usb2_audio_block'];assert size==1072 and len(raw)==size+1088;block=raw[:size];wire=raw[size:]
 decoder=module('audio_decode',ROOT/'apps/board/observe.py');prep=module('audio_admission',ROOT/'apps/board/prepare.py');results=json.loads((out/'progress.json').read_text()) if (out/'progress.json').exists() else {}
 prep.usb_audio_image_check(image_path.read_bytes())
 for variant in ('success','hub','wrong-kind','start-refused','read-failed','wrong-endpoint','detach','post-copy-detach','deadline','lost-after-copy'):
  if variant in results:continue
  m=Machine(image,session,block,variant);assert m.call('a733_board_session_valid',m.board,0)==1
  for workload in (13,15):
   m.put(m.board+400,workload);assert m.call('a733_board_session_valid',m.board,0)==0
  m.put(m.board+400,14);m.put(m.board+404,1);assert m.call('a733_board_session_valid',m.board,0)==0;m.put(m.board+404,0)
  m.call('board_host_task',0);assert m.result()==(1 if variant=='success' else 2)
  frame=m.frame()
  if variant in ('deadline','lost-after-copy'):assert not frame
  else:
   decoded=decoder.decode(frame,session);assert decoded['usb_state']==(1 if variant=='success' else 2)
  if variant=='success':
   assert frame[72:]==wire and decoded['kind']=='usb-audio-capture';(out/'frame.bin').write_bytes(frame);(out/'session.bin').write_bytes(session)
   for offset in (72,76,80,84,104,108,124,125,126,127,128,129,130,72+1087):
    bad=bytearray(frame);bad[offset]^=0x80
    try:decoder.decode(bad,session)
    except ValueError:pass
    else:raise AssertionError('Invalid audio wire accepted:'+str(offset))
  if variant in ('detach','post-copy-detach'):assert m.starts==1
  results[variant]=dict(passed=True,polls=m.polls,starts=m.starts,reads=m.reads,result=m.result());(out/'progress.json').write_text(json.dumps(results,indent=2))
 # Historical queued bytes are committed; failure revokes new snapshots.
 m=Machine(image,session,block,'success');m.call('board_host_task',0);frame=m.frame();m.put(m.board+m.layout['result'],2);m.call('board_usb0_task',0);assert m.tx[-1]==frame
 host=module('audio_capture',ROOT/'tools/check-a733-board-supervised.py');capture=out/'capture';capture.mkdir(exist_ok=True);record={}
 if not (capture/'audio.json').exists():
  host.capture(lambda packet,timeout:frame,session,None,capture,record,lambda:None,10,now=lambda:1.)
  assert record['board_result']=='observed-success'
 assert (capture/'audio.s16le').read_bytes()==decoder.decode(frame,session)['audio_pcm']
 durable=host.supervisor.durable_frame
 for name in ('audio.s16le','audio.json'):
  failed={};directory=out/('failure-'+name);directory.mkdir(exist_ok=True)
  def write(path,data):
   if path.name==name:raise OSError('modeled '+name+' durability failure')
   return durable(path,data)
  with patch.object(host.supervisor,'durable_frame',side_effect=write):
   try:host.capture(lambda *args:frame,session,None,directory,failed,lambda:None,10,now=lambda:1.)
   except OSError:assert failed.get('board_result')!='observed-success' and 'copied_audio' not in failed
   else:raise AssertionError('Audio durability failure accepted:'+name)
 (out/'result.json').write_text(json.dumps(dict(status='pass',cases=results,physical_access=False,image_sha256=sha(image_path.read_bytes()),block_wire_sha256=sha(raw),scope=__doc__),indent=2)+'\n')
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__)
 for n in ('image','block','output'):p.add_argument('--'+n,type=Path,required=True)
 a=p.parse_args();run(a.image,a.block,a.output)
