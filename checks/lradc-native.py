#!/usr/bin/env python3
"""Execute the linked LRADC ADC-application path with explicit lower models.

Actual session publication, task construction wrapper, registry, register lock,
LRADC driver/native MMIO, collector and copied USB serializer execute. Board
readiness, PMIC/rail service results, RTOS APIs, translations, analog conversion
and USB transport are modeled. No startup, CPU scheduler or physical operation.
"""
import argparse,copy,hashlib,importlib.util,json,pickle,struct,time
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR,UC_ARM64_REG_PSTATE,UC_ARM64_REG_TPIDR_EL1
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def save(p,value):p.write_text(json.dumps(value,indent=2)+'\n')
BaseImage=module('lradc_image',ROOT/'checks/storage-read-native.py').Image
class Image(BaseImage):
 def __init__(self,path,layouts):
  super().__init__(path,layouts);self.sizes={}
  with path.open('rb') as f:
   for cu in ELFFile(f).get_dwarf_info().iter_CUs():
    for d in cu.iter_DIEs():
     if d.tag=='DW_TAG_structure_type' and 'DW_AT_name' in d.attributes and 'DW_AT_byte_size' in d.attributes:
      name=d.attributes['DW_AT_name'].value.decode()
      if name in layouts:
       size=int(d.attributes['DW_AT_byte_size'].value);assert name not in self.sizes or self.sizes[name]==size;self.sizes[name]=size

def produce(image,manifest,out,reuse_launcher=None):
 """Actual production validator/producer, explicitly fictitious electrical facts."""
 out.mkdir(parents=True,exist_ok=False);p=module('lradc_prepare',ROOT/'apps/adc/prepare.py')
 provider=module('lradc_provider',ROOT/'checks/a733-provider-fixture.py');identity=p.base.image_parts(image)[2];ih=sha(image)
 baseline=Path.home()/'.cache/sunstoneos/evidence/adc-application-20261018/fixture/session-template.bin'
 raw=bytearray(provider.nominal_provider_session(baseline.read_bytes(),usb_reference_mode=1));raw[48:80]=identity
 struct.pack_into('<Q',raw,600,2);raw[768]=0
 target=dict(identity='LRADC-SOFTWARE-MODEL',port='NO-PHYSICAL-PORT',image_sha256=ih)
 def record(name,value):
  path=(out/name).resolve();data=json.dumps(value,sort_keys=True).encode();path.write_bytes(data)
  return dict(path=str(path),sha256=hashlib.sha256(data).hexdigest())
 generic=record('MODEL-NOT-PHYSICAL.json',dict(**target,physical=False,purpose='Software fixture only; no electrical determination exists for either board'))
 for off in (80,216,248,280):raw[off:off+32]=bytes.fromhex(generic['sha256'])
 determination=record('MODEL-NOT-PHYSICAL-electrical-determination.json',dict(**target,schema='a733-lradc-idle-electrical-authority-v1',reviewed=True,physical=False,basis='independent-board-electrical-determination',nominal_pullup_uv=1800000,documented_conversion_max_uv=1350000,safe_idle_overrange_at_admitted_avcc=True,not_inferred_from_saturation_or_keymap=True,warning='FICTITIOUS authority for software validation only; forbidden for physical use'))
 refs={}
 for i,(key,required) in enumerate(p.LRADC_FACTS.items()):
  value=dict(**target,schema='a733-adc-'+key+'-v1',reviewed=True,physical=False,**{k:True for k in required})
  if key=='electrical':value.update(cldo3_uv=1800000,cldo3_selector=13,source_impedance_ohms=struct.unpack_from('<I',raw,780)[0],lradc_idle_electrical_authority=determination)
  if key=='clocks':value.update(configure_adc_clock=0,apb1=0,apb1_hz=24000000)
  refs[key]=record(key+'.json',value);raw[632+32*i:664+32*i]=bytes.fromhex(refs[key]['sha256'])
 template=out/'session-template.bin';template.write_bytes(raw);p.session_check(raw);provider.validate_nominal_providers(raw,usb_reference_mode=1)
 conditions={'source-backed-cpu-dsu-transition','cold-cache-and-dirty-alias-exclusion','dual-pas-dram','exclusive-cpus-memory-dma','remote-writers-excluded','usb-clock-power-reset-ownership','usb-device-mapping','physical-counter-continuity'}
 platform=record('platform.json',dict(**target,schema='a733-smp-observer-platform-v1',entry_conditions_established=True,physical_coherence_result='under-test',conditions={k:generic for k in sorted(conditions)}))
 ddr=record('ddr.json',dict(**target,schema='a733-ddr-reservation-v1',exclusive=True,published_cache_disabled=True,remote_writers_excluded=True,ranges=[[0x41000000,0x42200000]],evidence=[generic]))
 admission=dict(**target,schema='a733-adc-supervised-entry-v1',reservation=100,session_template_sha256=sha(template),ddr=ddr,platform=platform,**refs,whole_board_power_removal=True,no_resource_reuse_before_power_removal=True,recovery=dict(mode='manual-usb-reconnect',demonstrated_fel=True,explicit_reconnect_before_resumption=True,preserve_microsd=True,one_attempt=True,evidence=[generic]))
 ap=out/'admission.json';save(ap,admission);p.validate_admission(admission,ih,sha(template),raw)
 bundle=out/'MODEL-NOT-DEPLOYABLE-bundle'
 if reuse_launcher:
  previous=p.validate_bundle(reuse_launcher);assert previous['image_sha256']==ih
  source_pairs={'launch.c':ROOT/'apps/adc/launch.c','launch.S':ROOT/'platform/a733-smp-observe/launch.S','launch.ld':ROOT/'platform/a733-smp-observe/launch.ld','boot.h':ROOT/'platform/a733-smp-cached/boot.h','adc_session.h':ROOT/'apps/adc/adc_session.h','a733_board_application.h':ROOT/'include/a733_board_application.h','base-supervised-launch.c':ROOT/'platform/a733-smp-supervised/launch.c'}
  for name,path in source_pairs.items():assert (reuse_launcher/name).read_bytes()==path.read_bytes(),name
  def reuse(destination,clang,lld):
   names=[*source_pairs,'adc-launch-base.c','launch.c.o','launch.S.o','launch.elf','launch.bin','launcher-build.log','launcher-commands.json']
   for name in names:
    data=(reuse_launcher/name).read_bytes();assert hashlib.sha256(data).hexdigest()==previous['files'][name];(destination/name).write_bytes(data)
   save(destination/'launcher-reuse.json',dict(original_bundle=str(reuse_launcher.resolve()),prepared_sha256=sha(reuse_launcher/'prepared.json'),launch_sha256=sha(reuse_launcher/'launch.bin'),source_inputs_unchanged=True,compiled=False))
  p.build_launcher=reuse
 p.prepare(image,manifest,ap,template,bundle,'/usr/lib/llvm-21/bin/clang','/home/sunil/.cache/sunstoneos/host/usr/bin/ld.lld-21')
 save(out/'producer.json',dict(physical=False,image_sha256=ih,source_template_sha256=sha(baseline),session_sha256=sha(bundle/'session.bin'),launch_sha256=sha(bundle/'launch.bin'),scope='Actual typed producer only; launcher compiled once or reused with unchanged source inputs, never executed here. Electrical authority is fictitious.'))
 return bundle

class Machine:
 def __init__(self,image,session,contract):
  self.image=image;self.session=session;self.contract=contract;self.u=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
  for a,n in [(0x02000000,0x06000000),(0x41000000,0x03000000)]:self.u.mem_map(a,n)
  for a,data in image.parts:self.u.mem_write(a,data)
  self.u.reg_write(UC_ARM64_REG_PSTATE,5);self.u.reg_write(UC_ARM64_REG_TPIDR_EL1,0)
  # Bounded EL1h function calls, not an actual cached-SMP task context.
  for n in ('ullCriticalNestings','ullPortInterruptNestings'):
   self.u.mem_write(image.address(n),bytes(64))
  self.base=image.address('adc');self.l=image.layouts['adc_application'];self.board=image.address('board');self.b=image.layouts['board_application']
  self.shared=image.address('providers')+image.layouts['board_provider']['shared'];self.usb=image.layouts['a733_native_usb']
  self.locks=self.shared+self.usb['locks'];self.registry=self.shared+self.usb['clocks'];self.pmic=self.shared+self.usb['pmic']
  self.native=self.base+self.l['native'];self.driver=self.base+self.l['lradc'];self.d=image.layouts['a733_lradc']
  self.time=struct.unpack_from('<Q',contract,80)[0]//24+1000;self.task=1;self.holders={};self.done=False;self.incoming=bytearray();self.tx=bytearray();self.trace=[];self.mmio=[];self.mmio_in_progress=False;self.task_contexts={};self.call_name=None;self.critical=0;self.saved_daif=0
  self.regs={0x07090160:0x10000003,0x02002000:0xf8636300,0x02002518:0,0x07090000:0,0x07090004:0,0x07090008:15,0x0709000c:0,0x02003024:0xa5000000,0x02524000:0x01000168,0x0252400c:63}
  self.u.mem_write(0x41f00000,session);self.u.mem_write(0x43fa0000,contract)
  self.u.mem_write(self.board,session[:592]);self.u.mem_write(self.board+self.b['admitted_session'],session[:592]);self.put(self.board+160,struct.unpack_from('<Q',contract,104)[0]//24,8)
  self.put(self.shared+self.usb['pmic_ready'],1)
  names=['board_live','a733_smp_counter','a733_counter_clock_now','aarch64_device_translate','a733_cpu_coherence_publish','xTaskGetSchedulerState','xTaskGetCurrentTaskHandle','xQueueCreateMutexStatic','xQueueGenericCreateStatic','xTaskCreateStatic','xQueueGetMutexHolder','xQueueSemaphoreTake','xQueueGenericSend','a733_pmic_identity','a733_pmic_live','a733_pmic_submit','a733_pmic_result','a733_rail_acquire','a733_rail_poll','a733_rail_status','a733_rail_retain','vTaskEnterCritical','vTaskExitCritical','vTaskDelay','platform_panic','a733_native_usb_step','a733_native_usb_health','tusb_rhport_init','tud_task_ext','tud_vendor_n_available','tud_vendor_n_read','tud_vendor_n_write','tud_vendor_n_write_flush']
  self.hooks={image.address(n):n for n in names};assert len(self.hooks)==len(names)
  for a in set(self.hooks)|{0x43fff000}:self.u.hook_add(UC_HOOK_CODE,self.code,begin=a,end=a)
  self.u.hook_add(UC_HOOK_MEM_READ,self.read_mmio,begin=0x02000000,end=0x07ffffff);self.u.hook_add(UC_HOOK_MEM_WRITE,self.write_mmio,begin=0x02000000,end=0x07ffffff)
 def put(self,a,v,n=4):self.u.mem_write(a,int(v).to_bytes(n,'little'))
 def get(self,a,n=4):return int.from_bytes(self.u.mem_read(a,n),'little')
 def ret(self,v=0):self.u.reg_write(UC_ARM64_REG_X0,v&((1<<64)-1));self.u.reg_write(UC_ARM64_REG_PC,self.u.reg_read(UC_ARM64_REG_LR))
 def code(self,u,a,size,unused):
  if a==0x43fff000:self.done=True;u.emu_stop();return
  name=self.hooks[a];x=[u.reg_read(UC_ARM64_REG_X0+i) for i in range(8)];self.trace.append([name,x[:4]])
  if name=='board_live':self.ret(1)
  elif name=='a733_smp_counter':self.ret(self.time*24)
  elif name=='a733_counter_clock_now':self.put(x[1],self.time,8);self.ret(1)
  elif name=='aarch64_device_translate':u.mem_write(x[2],struct.pack('<QII',x[0],0,2));self.ret(1)
  elif name=='a733_cpu_coherence_publish':self.ret(1)
  elif name=='xTaskGetSchedulerState':self.ret(2)
  elif name=='xTaskGetCurrentTaskHandle':self.ret(self.task)
  elif name in ('xQueueCreateMutexStatic','xQueueGenericCreateStatic'):self.ret(x[1] if name=='xQueueCreateMutexStatic' else x[3])
  elif name=='xTaskCreateStatic':self.ret(x[6])
  elif name=='xQueueGetMutexHolder':self.ret(self.holders.get(x[0],0))
  elif name=='xQueueSemaphoreTake':assert not self.holders.get(x[0],0);self.holders[x[0]]=self.task;self.ret(1)
  elif name=='xQueueGenericSend':assert self.holders[x[0]]==self.task;self.holders[x[0]]=0;self.ret(1)
  elif name.startswith('a733_rail_'):
   if name=='a733_rail_acquire':
    assert x[1:4]==[23,1<<22,0];u.mem_write(x[5],struct.pack('<4Q',2,23,1<<22,0));self.ret(1)
   else:self.ret(0)
  elif name.startswith('a733_pmic_'):
   assert x[0]==self.pmic;lifetime=struct.pack('<3Q',self.pmic,struct.unpack_from('<Q',self.session,456)[0],struct.unpack_from('<Q',self.session,32)[0])
   if name=='a733_pmic_identity':u.mem_write(x[1],lifetime);self.ret(1)
   elif name=='a733_pmic_live':assert bytes(u.mem_read(x[1],24))==lifetime;self.ret(1)
   elif name=='a733_pmic_submit':assert x[1]==23 and x[3]==22;u.mem_write(x[5],lifetime+struct.pack('<2Q',1,23));self.ret(0)
   else:
    fields=self.image.layouts['a733_pmic_snapshot'];raw=bytearray(self.image.sizes['a733_pmic_snapshot'])
    for key,value,n in [('rail',22,4),('chip',3,1),('enable',32,1),('selector',13,1),('enabled',1,4),('decoded',1,4),('programmed_uv',1800000,4)]:raw[fields[key]:fields[key]+n]=value.to_bytes(n,'little')
    u.mem_write(x[2],bytes(raw));self.ret(2)
  elif name=='vTaskEnterCritical':
   assert not self.get(self.image.address('ullPortInterruptNestings'),8)
   if not self.critical:self.saved_daif=u.reg_read(UC_ARM64_REG_PSTATE)&0x3c0
   self.critical+=1;self.put(self.image.address('ullCriticalNestings'),self.critical,8);u.reg_write(UC_ARM64_REG_PSTATE,u.reg_read(UC_ARM64_REG_PSTATE)|0x80);self.ret()
  elif name=='vTaskExitCritical':
   assert self.critical;self.critical-=1;self.put(self.image.address('ullCriticalNestings'),self.critical,8)
   assert not self.get(self.image.address('ullPortInterruptNestings'),8)
   if not self.critical:u.reg_write(UC_ARM64_REG_PSTATE,(u.reg_read(UC_ARM64_REG_PSTATE)&~0x3c0)|self.saved_daif)
   self.ret()
  elif name=='vTaskDelay':
   assert not self.critical
   self.ret();self.task_contexts[self.call_name]=u.context_save();self.done=True;u.emu_stop()
  elif name=='platform_panic':raise AssertionError(('native panic',x,self.trace[-12:]))
  elif name in ('a733_native_usb_step','a733_native_usb_health','tusb_rhport_init','tud_task_ext'):self.ret(1)
  elif name=='tud_vendor_n_available':self.ret(len(self.incoming))
  elif name=='tud_vendor_n_read':n=min(x[2],len(self.incoming));u.mem_write(x[1],bytes(self.incoming[:n]));del self.incoming[:n];self.ret(n)
  elif name=='tud_vendor_n_write':self.tx.extend(u.mem_read(x[1],x[2]));self.ret(x[2])
  elif name=='tud_vendor_n_write_flush':self.ret(0)
  else:raise AssertionError(name)
 def read_mmio(self,u,access,a,size,value,unused):
  self.mmio_in_progress=True;assert size==4
  assert self.holders.get(self.get(self.locks+self.image.layouts['a733_platform_locks']['mutex'],8))==self.task,('unlocked MMIO',hex(a))
  u.mem_write(a,self.regs.get(a,0).to_bytes(4,'little'));self.mmio.append(['read',a,self.regs.get(a,0)]);self.mmio_in_progress=False
 def write_mmio(self,u,access,a,size,value,unused):
  self.mmio_in_progress=True;assert size==4 and self.holders.get(self.get(self.locks+self.image.layouts['a733_platform_locks']['mutex'],8))==self.task
  assert a in (0x02003024,0x02524000,0x02524004,0x02524008),hex(a)
  self.regs[a]=(self.regs.get(a,0)&~value) if a==0x02524008 else value;self.mmio.append(['write',a,value]);self.mmio_in_progress=False
 def call(self,name,*args,unit=None):
  self.done=False;self.call_name=name
  for i,v in enumerate(args):self.u.reg_write(UC_ARM64_REG_X0+i,v)
  if name in self.task_contexts:self.u.context_restore(self.task_contexts[name]);entry=self.u.reg_read(UC_ARM64_REG_PC)
  else:
   self.u.reg_write(UC_ARM64_REG_SP,0x43fc0000 if name=='board_usb0_task' else 0x43fd0000);self.u.reg_write(UC_ARM64_REG_LR,0x43fff000);entry=self.image.address(name,unit)
  self.u.emu_start(entry,0,count=2000000);assert self.done,(name,hex(self.u.reg_read(UC_ARM64_REG_PC)))
  return self.u.reg_read(UC_ARM64_REG_X0)&0xffffffff
 def checkpoint(self,out,label):
  # Serializes every mutable model field by value, plus coherent RAM/CPU state.
  state={k:pickle.loads(pickle.dumps(v)) for k,v in self.__dict__.items() if k not in ('u','image')}
  state['cpu']=self.u.context_save();(out/(label+'.pickle')).write_bytes(pickle.dumps(state));(out/(label+'-ram.bin')).write_bytes(bytes(self.u.mem_read(0x41000000,0x03000000)))
  save(out/(label+'-manifest.json'),dict(inputs=json.loads((out/'inputs.json').read_text()),files={p.name:dict(bytes=p.stat().st_size,sha256=sha(p)) for p in [out/(label+'.pickle'),out/(label+'-ram.bin'),out/'executed-check-source.py']},mmio_in_progress=self.mmio_in_progress,scope=__doc__))


def run(image,bundle,contract_path,out):
 out.mkdir(parents=True,exist_ok=False);prep=module('lradc_run_prepare',ROOT/'apps/adc/prepare.py');validated=prep.validate_bundle(bundle)
 assert sha(image)==validated['image_sha256'],'Separate native ELF does not match authenticated bundle image'
 session=(bundle/'session.bin').read_bytes();contract=contract_path.read_bytes();assert len(session)==784 and struct.unpack_from('<Q',session,600)[0]==2
 inputs={str(p.resolve()):sha(p) for p in [image,bundle/'session.bin',bundle/'contract.bin',bundle/'prepared.json',bundle/'image-input-manifest.json']}
 inputs.update({str(p.resolve()):sha(p) for p in [contract_path,ROOT/'checks/lradc-native.py',ROOT/'checks/storage-read-native.py']});save(out/'inputs.json',inputs)
 (out/'executed-check-source.py').write_bytes(Path(__file__).read_bytes())
 layouts=['adc_application','a733_lradc','board_application','board_provider','a733_native_usb','a733_platform_locks','a733_pmic_snapshot']
 m=Machine(Image(image,layouts),session,contract)
 try:
  m.u.mem_write(0x43fa1000,session);assert m.call('adc_session_valid',0x43fa1000)==1
  assert m.call('a733_smp_contract_valid',0x43fa0000,m.time*24)==1
  assert m.call('adc_session_prepare',0x43fa0000)==1
  assert m.call('a733_clock_leases_init',m.registry,m.image.address('a733_counter_clock_now'),0)==1
  assert m.call('a733_platform_locks_init',m.locks,m.image.address('a733_counter_clock_now'),0)==1
  assert m.call('board_application_start')==1;m.task=m.get(m.base+m.l['handle'],8);assert m.task
  m.checkpoint(out,'constructed')
  for i in range(100):
   stage=m.get(m.base+m.l['stage']);sets=m.get(m.base+m.l['sets'])
   if stage==8 and sets<2 and m.time>=m.get(m.base+m.l['next_read'],8):m.regs[0x02524008]=1;m.regs[0x0252400c]=0 if sets==0 else 63
   result=m.call('adc_step');m.time+=1000
   if result:assert result==1;break
  else:raise AssertionError('bounded application completion expired')
  assert m.get(m.base+m.l['sets'])==2 and not m.regs[0x02524004] and not(m.regs[0x02524000]&1)
  assert m.get(m.driver+m.d['stage'])==4 and m.get(m.driver+m.d['sequence'])==2
  assert m.regs[0x02003024]==0xa5010001 and not any(m.holders.values())
  m.checkpoint(out,'terminal')
  assert m.call('a733_platform_lock_enter',m.locks,0,m.time+10000)==1
  before=sum(row[0]=='write' for row in m.mmio)
  for address,value in [(0x02521000,0),(0x07090000,0),(0x02003024,0),(0x02524000,0x0180004d)]:
   assert m.call('write_register',m.native,address,value,unit='/drivers/adc/a733_adc_native.c')==0
  assert sum(row[0]=='write' for row in m.mmio)==before
  m.call('a733_platform_lock_leave',m.locks,0)
  m.task=2;m.incoming=bytearray(1)
  m.call('board_usb0_task');m.call('board_usb0_task');assert len(m.tx)==288
  frame=bytes(m.tx);(out/'reply.bin').write_bytes(frame)
  decode=module('lradc_decoder',ROOT/'apps/adc/observe.py').decode(frame,session);assert [r[0]['raw_code'] for r in decode['observations']]==[0,63]
  host=module('lradc_host',ROOT/'apps/adc/capture.py');record={};exchange=lambda packet,timeout:frame
  (out/'host').mkdir()
  host.capture(exchange,session,out/'host',record,lambda:None,time.monotonic()+5)
  save(out/'nominal-mmio.json',m.mmio);save(out/'result.json',dict(passed=True,physical=False,image_sha256=sha(image),session_sha256=sha(bundle/'session.bin'),actual_linked_path=True,native_unauthorized_writes_refused=4,raw_codes=[0,63],configured_disabled=True,retained=True,decoded=decode,scope=__doc__))
 except BaseException:
  m.checkpoint(out,'stopped');save(out/'failure.json',dict(pc=hex(m.u.reg_read(UC_ARM64_REG_PC)),stage=m.get(m.base+m.l['stage']),mmio_in_progress=m.mmio_in_progress,trace=m.trace[-20:],mmio=m.mmio[-20:]));raise

if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--image',type=Path,required=True);p.add_argument('--manifest',type=Path);p.add_argument('--reuse-launcher',type=Path);p.add_argument('--bundle',type=Path);p.add_argument('--contract',type=Path);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
 if a.manifest:produce(a.image,a.manifest,a.output,a.reuse_launcher)
 else:run(a.image,a.bundle,a.contract,a.output)
