#!/usr/bin/env python3
"""Execute checkpoint admission, caller, publication and return with modeled CPU/MMIO."""
import argparse,hashlib,importlib.util,json,struct
from pathlib import Path
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_WRITE,UC_HOOK_MEM_READ,UC_MEM_READ,UC_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--native',type=Path,required=True);p.add_argument('--evidence',type=Path,required=True);a=p.parse_args();a.evidence.mkdir(parents=True,exist_ok=False)
r=Path(__file__).resolve().parents[2]
def module(n,p):
 s=importlib.util.spec_from_file_location(n,p);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
h=module('checkpoint_host',r/'apps/npu-finite/host.py');layout=module('checkpoint_layout',r/'checks/storage-read-native.py');images=module('checkpoint_images',r/'checks/npu-image-checks.py')
e=a.native;im=layout.Image(e/'image.elf',layout_names=('a733_smp_contract','device_memory_pool','device_memory_extent','device_memory_allocation','a733_npu_application_result'))
_,_,build,sl,g=h.image_profile((e/'image.elf').read_bytes(),json.loads((e/'manifest.json').read_text()))
raw=bytearray(images.npu_page((Path.home()/'.cache/sunstoneos/evidence/npu-single-core-20261018/startup-usb-resume/session-fixture.bin').read_bytes(),sl,3));raw[48:80]=build
profile=h.original.session_profile(raw,sl);_,contract=h.original.request_bytes(0,build,raw,profile);contract=bytearray(contract)
cl=im.layouts['a733_smp_contract'];armed=10000000;deadline=armed+480000000
for name,value in [('magic',h.original.CONTRACT_MAGIC),('armed_at',armed),('startup_deadline',armed+24000000),('diagnostic_deadline',deadline),('expires_at',deadline)]:struct.pack_into('<Q',contract,cl[name],value)
for n in ('a733_npu_application_init','npu_convolution_startup_poll','npu_service_activate','vTaskStartScheduler','a733_pmic_poll','a733_npu_startup_step'):
 assert n not in im.symbols,n
results=[]
def machine(mbus=0x81000000, busy_gpu=False):
 u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0x02000000,0x07000000);u.mem_map(0x40000000,0x04000000)
 for at,data in im.parts:
  if data:u.mem_write(at,data)
 u.mem_write(0x41f00000,bytes(raw));u.mem_write(0x42004000,bytes(contract))
 u.mem_write(0x03400000,struct.pack('<II',0x30,3))
 u.mem_map(0x50000,0x30000)
 u.mem_write(0x02002588,struct.pack('<I',mbus));u.mem_write(0x02002594,struct.pack('<I',0x30001))
 if busy_gpu:u.mem_write(0x02002b20,struct.pack('<I',0x80000000))
 state={'el':12,'sctlr':0,'now':11000000,'mode':5,'smc':None,'return':False,'retained':False,'panics':[],'clean':[],'mmio':[],'translations':0,'phase':'monitor','inject':None}
 def put(at,value,n=8):u.mem_write(at,int(value).to_bytes(n,'little'))
 def get(at,n=8):return int.from_bytes(u.mem_read(at,n),'little')
 def ret(value=0):u.reg_write(UC_ARM64_REG_X0,value);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
 hooks={im.address(n):n for n in ('a733_cpu_coherence_check_el1','aarch64_device_translate','vPortEnterCritical','vPortExitCritical','platform_panic','a733_smp_stop','npu_finite_rom_return') if n in im.symbols}
 def code(cpu,at,size,cookie):
  if at==im.address('a733_smp_diagnostic_start') and state['inject']:
   if state['inject']=='app':put(im.address('app'),1,1)
   elif state['inject']=='ledger':put(im.address('boot_pool')+im.layouts['device_memory_pool']['allocations'],1)
  name=hooks.get(at);args=[u.reg_read(UC_ARM64_REG_X0+i) for i in range(5)]
  if name=='a733_cpu_coherence_check_el1':ret(1);return
  if name in ('vPortEnterCritical','vPortExitCritical'):ret();return
  if name=='aarch64_device_translate':
   state['translations']+=1;put(args[2],args[0]);put(args[2]+8,0xff if args[0]>=0x40000000 else 0,4);put(args[2]+12,3 if args[0]>=0x40000000 else 2,4);ret(1);return
  if name=='platform_panic':state['panics'].append(args[1]);state['retained']=True;u.emu_stop();return
  if name in ('a733_smp_stop','npu_finite_rom_return'):
   state['retained']=name=='a733_smp_stop';state['return']=name=='npu_finite_rom_return';u.emu_stop();return
  word=get(at,4);kind=word&0xffffffe0;reg=word&31
  if kind==0xd53be020:state['now']+=24
  values={0xd5384240:state['el'],0xd53800a0:0,0xd53e1000:0,0xd5381000:state['sctlr'],0xd53b4220:0x3c0,
   0xd5384600:0xf0,0xd53be020:state['now'],0xd53b0020:0x8444c004,0xd53e5200:0x17<<26,0xd53e4000:state['mode']|0x3c0}
  if kind in values:
   if reg<31:u.reg_write(UC_ARM64_REG_X0+reg,values[kind])
   u.reg_write(UC_ARM64_REG_PC,at+4);return
  if kind==0xd50b7a20:state['clean'].append(args[reg] if reg<5 else u.reg_read(UC_ARM64_REG_X0+reg));u.reg_write(UC_ARM64_REG_PC,at+4);return
  if kind==0xd5181000:state['sctlr']=u.reg_read(UC_ARM64_REG_X0+reg)
  # Privileged writes/barriers and cache/TLB operations are modeled effects.
  if word&0xfff00000==0xd5100000 or word&0xfffff000 in (0xd5033000,0xd5088000):u.reg_write(UC_ARM64_REG_PC,at+4);return
  if word==0xd4000003:state['smc']=args[:2];u.emu_stop();return
  if word&0xfff00000==0xd5300000:raise AssertionError(('unmodeled register read',hex(word),hex(at)))
 def mmio(cpu,access,at,size,value,cookie):
  state['mmio'].append([state['phase'],access,at,size])
  assert not (0x03600000<=at<0x03700000 or 0x07064000<=at<0x07065000 or 0x07081400<=at<0x07082000 or at==0x03000008),hex(at)
  if 0x02002000<=at<0x02004000:
   assert state['phase']=='monitor',('late CCU access',hex(at))
   if access==UC_MEM_READ:
    assert at in (0x02002588,0x02002594,0x02002b20,0x02002b24,0x02002b00,0x02002b04,0x02002704,0x0200270c,0x02002ac4),hex(at)
    if at==0x02002588 and get(at,4)&(1<<27):put(at,get(at,4)&~(1<<27),4)
   else:
    assert access==UC_MEM_WRITE and at in (0x02002588,0x02002594,0x02003f00),('forbidden CCU write',hex(at))
    if at==0x02003f00:assert value==7 # Existing cold security permission setup.
 u.hook_add(UC_HOOK_CODE,code);u.hook_add(UC_HOOK_MEM_WRITE,mmio,begin=0x02000000,end=0x08ffffff);u.hook_add(UC_HOOK_MEM_READ,mmio,begin=0x02000000,end=0x08ffffff)
 def call(name,*args,count=5000000):
  for i,value in enumerate(args):u.reg_write(UC_ARM64_REG_X0+i,value)
  u.reg_write(UC_ARM64_REG_SP,0x43ffe000);u.reg_write(UC_ARM64_REG_LR,0x43fff000);u.emu_start(im.address(name),0x43fff000,count=count)
  return u.reg_read(UC_ARM64_REG_X0)
 return u,state,put,get,call
# Actual monitor: validates/hashes/copies/seals the session, constructs tables,
# captures GIC state and initializes the real report. No validator is stubbed.
u,state,put,get,call=machine();call('sd_single_core_monitor_init');assert not state['retained'],state
initial=bytes(u.mem_read(0x42000000,128));(a.evidence/'initial-report.bin').write_bytes(initial)
sealed_ram=bytes(u.mem_read(0x41000000,0x1200000))
state['el']=4;state['phase']='kernel';call('kernel_main');assert state['smc']==[0x733,0] and not state['panics'],state
assert state['clean']==list(range(g['clean_start'],g['clean_end'],64))
assert {x[2] for x in state['mmio'] if x[0]=='kernel'}=={0x0347041e,0x03470100,0x03460000},state['mmio']
receipt=bytes(u.mem_read(g['receipt'],88));assert struct.unpack('<11Q',receipt)[:6]==(0x4e505543484b3031,profile['epoch'],profile['generation'],profile['lease'],0,0x4e50554e)
# Snapshot the published state for bounded corruption/late-return cases.
ram=bytes(u.mem_read(0x41000000,0x1200000));results.append({'case':'actual-monitor-session-kernel-pool-checkpoint-publication','passed':True,'clean_lines':len(state['clean']),'translations':state['translations'],'provider_mmio':0})
variants=['success','wrong-caller-mode','stale-secure-generation','late','wrong-receipt-magic','ordinary-completion','app-written','job-published','pool-ledger-live','pool-not-empty','pool-pointer-outside','checkpoint-identity','session-identity']
for variant in variants:
 u,state,put,get,call=machine();u.mem_write(0x41000000,ram);state.update(el=12,sctlr=0,phase='return')
 if variant=='wrong-caller-mode':state['mode']=4
 if variant=='stale-secure-generation':put(im.address('admission')+cl['generation'],profile['generation']+1)
 if variant=='late':state['now']=deadline
 if variant=='wrong-receipt-magic':put(g['receipt'],0x4e50555245543031)
 if variant=='ordinary-completion':put(g['receipt']+40,1)
 if variant=='app-written':put(im.address('app'),1,1)
 if variant=='job-published':put(im.address('a733_npu_application_result')+im.layouts['a733_npu_application_result']['jobs'],1,4)
 pool=get(im.address('checkpoint')+48)
 if variant=='pool-ledger-live':put(pool+im.layouts['device_memory_pool']['allocations'],1)
 if variant=='pool-not-empty':put(pool+im.layouts['device_memory_pool']['next_generation'],2)
 if variant=='pool-pointer-outside':put(im.address('checkpoint')+48,0x43000000)
 if variant=='checkpoint-identity':put(im.address('checkpoint')+8,profile['epoch']+1)
 if variant=='session-identity':put(im.address('session')+24,profile['epoch']+1)
 call('sd_single_core_exception',0x733,0,1)
 assert state['return']==(variant=='success') and state['retained']==(variant!='success'),(variant,state)
 if variant=='success':
  frames=[bytes(u.mem_read(at,size)) for at,size in ((0x42000000,128),(g['log'],16384),(g['receipt'],88))]
  parsed=h.validate_reports(*frames,{'execution_profile':h.geometry_profile(g),'geometry':g,**profile})
  assert parsed['jobs']==0 and not parsed['functional_success'] and parsed['normal_return']
  assert parsed['mbus']=={'before':0x81000000,'restore_result':0,'after':0x81000000}
  for name,data in zip(('monitor','platform','receipt'),frames):(a.evidence/(name+'.bin')).write_bytes(data)
  results.append({'case':'actual-final-writer-to-host-parser','passed':True,'result':parsed})
 results.append({'case':variant,'passed':True,'normal_return':state['return']})
# Exercise the actual existing normalization and its safe skip through the
# checkpoint wrapper. Only MBUS/MSI writes are allowed by the MMIO hook.
for label,busy,expected in [('normalized',False,(0x84000000,0,0x81000000)),('busy-master-skip',True,(0x84000000,8,0x84000000))]:
 u,state,put,get,call=machine(0x84000000,busy);call('sd_single_core_monitor_init');assert not state['retained'],state
 observed=tuple(get(im.address('npu_entry_checkpoint_mbus')+8*i) for i in (1,2,3));assert observed==expected,(label,observed)
 state['el']=4;state['phase']='kernel';call('kernel_main');assert state['smc']==[0x733,0] and not state['panics'],state
 state.update(el=12,sctlr=0,phase='return');call('sd_single_core_exception',0x733,0,1);assert state['return'] and not state['retained'],state
 frames=[bytes(u.mem_read(at,size)) for at,size in ((0x42000000,128),(g['log'],16384),(g['receipt'],88))]
 parsed=h.validate_reports(*frames,{'execution_profile':h.geometry_profile(g),'geometry':g,**profile});assert tuple(parsed['mbus'].values())==expected
 results.append({'case':'actual-MBUS-'+label+'-publication-and-return','passed':True,'diagnostic':parsed['mbus']})
for corruption in ('app','ledger'):
 u,state,put,get,call=machine();u.mem_write(0x41000000,sealed_ram);state.update(el=4,phase='kernel',inject=corruption)
 call('kernel_main');assert state['smc']==[0x733,4] and not state['return'],state
 results.append({'case':'actual-caller-refuses-'+corruption,'passed':True,'normal_receipt':False})
(a.evidence/'result.json').write_text(json.dumps({'passed':True,'image_sha256':h.sha((e/'image.elf').read_bytes()),'check_sha256':h.sha(Path(__file__).read_bytes()),'host_sha256':h.sha((r/'apps/npu-finite/host.py').read_bytes()),'cases':results,'scope':'Actual linked monitor/session validation and seal, mapping constructor, kernel/platform caller, local pool preparation, checkpoint, publication and EL3 final writer. Coherence admission, architectural translation, critical sections and privileged registers/effects modeled. ROM tail is reused; no hardware or provider startup.'},indent=2)+'\n');print('PASS checkpoint cases',len(results))
