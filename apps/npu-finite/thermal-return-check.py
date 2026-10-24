#!/usr/bin/env python3
"""Execute finite thermal receipt publication and final monitor writes into host parsing.

The pre-client platform header initialization is modeled. Client log bytes come from the separately executed native client. Quiescence,
cache effects, system registers, counter, GIC and secondary status are modeled.
No complete native admission, physical thermal execution or ROM return is claimed.
"""
import argparse,hashlib,importlib.util,json,struct,sys
from pathlib import Path
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_WRITE,UC_HOOK_MEM_READ
from unicorn.arm64_const import *
ROOT=Path(__file__).resolve().parents[2];sys.path.insert(0,str(ROOT/'checks'));import harness
p=argparse.ArgumentParser(description=__doc__)
for n in ('native','client','initial','output'):p.add_argument('--'+n,type=Path,required=True)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
spec=importlib.util.spec_from_file_location('finite_thermal_host',ROOT/'apps/npu-finite/host.py');h=importlib.util.module_from_spec(spec);spec.loader.exec_module(h)
e=harness.Image(a.native/'image.elf',layout_names=('a733_observe_identity','a733_npu_application_result','npu_finite_receipt','a733_smp_contract'))
data=(a.native/'image.elf').read_bytes();g=h.image_profile(data,json.loads((a.native/'manifest.json').read_bytes()))[4];results=[];receipts={}
def machine():
 u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0x02000000,0x07000000);u.mem_map(0x40000000,0x04000000)
 for at,data in e.parts:
  if data:u.mem_write(at,data)
 u.reg_write(UC_ARM64_REG_SP,0x43ffe000);u.reg_write(UC_ARM64_REG_LR,0x43fff000);return u
def put(u,at,v,n=8):u.mem_write(at,int(v).to_bytes(n,'little'))
def get(u,at,n=8):return int.from_bytes(u.mem_read(at,n),'little')
def ret(u,v=0):u.reg_write(UC_ARM64_REG_X0,v);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
receipt=e.address('npu_finite_receipt');rl=e.layouts['npu_finite_receipt'];assert rl['thermal_deadline']==104
cases=[('matched',0,2,0),('never-started',2,0,2),('one-job',2,1,2),('one-mismatch',1,1,2)]
for variant,status,jobs,reason in cases+[('cache-drain-expired',2,0,2)]:
 u=machine();identity=e.address('a733_observe_identity');ids=e.layouts['a733_observe_identity'];result=e.address('a733_npu_application_result');rs=e.layouts['a733_npu_application_result'];state={'time':24000000,'clean':[],'smc':None}
 for name,v in [('epoch',7),('generation',8),('lease',9),('deadline',480000000)]:put(u,identity+ids[name],v)
 for name,v in [('jobs',jobs),('thermal_reason',reason),('complete',status+1)]:put(u,result+rs[name],v,4)
 raw=(a.client/(('never-started' if variant=='cache-drain-expired' else variant)+'-platform.bin')).read_bytes();u.mem_write(g['log'],raw)
 hooks={e.address(n):n for n in ('a733_npu_application_quiescent','a733_npu_application_thermal_deadline','aarch64_device_cache_stride')}
 def code(cpu,at,size,cookie):
  word=get(u,at,4);kind=word&0xffffffe0;reg=word&31;name=hooks.get(at)
  if name:ret(u,120000000 if name=='a733_npu_application_thermal_deadline' else 64 if name=='aarch64_device_cache_stride' else 1);return
  if kind==0xd53be020 and variant=='cache-drain-expired' and state['clean']:state['time']=120000000
  if kind==0xd53be020:u.reg_write(UC_ARM64_REG_X0+reg,state['time']);u.reg_write(UC_ARM64_REG_PC,at+4);return
  if kind==0xd50b7a20:state['clean'].append(u.reg_read(UC_ARM64_REG_X0+reg));u.reg_write(UC_ARM64_REG_PC,at+4);return
  if kind==0xd5381000:u.reg_write(UC_ARM64_REG_X0+reg,0x30d01805);u.reg_write(UC_ARM64_REG_PC,at+4);return
  if kind==0xd5181000:u.reg_write(UC_ARM64_REG_PC,at+4);return
  if word==0xd4000003:state['smc']=[u.reg_read(UC_ARM64_REG_X0),u.reg_read(UC_ARM64_REG_X1)];u.emu_stop()
 u.hook_add(UC_HOOK_CODE,code);u.reg_write(UC_ARM64_REG_X0,status);u.emu_start(e.address('platform_exit'),0,count=2000000)
 if variant=='cache-drain-expired':
  assert state['smc']==[0x733,4] and 0<len(state['clean'])<=64;results.append({'case':variant,'passed':True,'clean_lines':len(state['clean'])});continue
 assert state['smc']==[0x733,status] and state['clean']==list(range(g['clean_start'],g['clean_end'],64)),(variant,state['smc'])
 receipts[variant]=bytes(u.mem_read(receipt,112));(a.output/(variant+'-receipt.bin')).write_bytes(receipts[variant]);results.append({'case':'publication-'+variant,'passed':True,'clean_lines':len(state['clean'])})
for variant,status,jobs,reason in cases+[('late-IRQ-before',2,0,2),('late-IRQ-after-restore',2,0,2),('late-drain-after-restore',2,0,2),('fault-reason',2,0,4),('GICD-drain-timeout',2,0,2),('GICR-drain-timeout',2,0,2)]:
 normal=variant in receipts;source=variant if normal else 'never-started';u=machine();u.mem_write(receipt,receipts[source]);raw=(a.client/(source+'-platform.bin')).read_bytes();u.mem_write(g['log'],raw);put(u,g['log'],0x534b4552);put(u,g['log']+64,16000000)
 initial=bytearray(a.initial.read_bytes());struct.pack_into('<3Q',initial,8,7,8,1);struct.pack_into('<Q',initial,48,12000000);u.mem_write(0x42000000,bytes(initial))
 c=e.layouts['a733_smp_contract'];admission=e.address('admission')
 for name,v in [('epoch',7),('generation',8),('lease',9),('diagnostic_deadline',480000000)]:put(u,admission+c[name],v)
 put(u,e.address('npu_finite_thermal_irq_admitted'),1,4);u.mem_write(e.address('saved'),struct.pack('<8I',1,4,0x33,0x123,0x456,0,0,0)+bytes(128))
 if variant=='late-IRQ-before':put(u,0x03400208,128,4)
 if variant=='fault-reason':put(u,receipt+rl['thermal_reason'],4)
 state={'time':24000000,'return':False,'retained':False,'writes':[]};hooks={e.address(n):n for n in ('a733_smp_stop','npu_finite_rom_return')}
 def code(cpu,at,size,cookie):
  word=get(u,at,4);kind=word&0xffffffe0;reg=word&31;name=hooks.get(at)
  if kind in (0xd51fe220,0xd51be220,0xd51be320):u.reg_write(UC_ARM64_REG_PC,at+4);return
  if name:state['return']=name=='npu_finite_rom_return';state['retained']=name=='a733_smp_stop';u.emu_stop();return
  value=None
  if kind==0xd53be020:value=state['time'];state['time']+=2400000 if variant.endswith('drain-timeout') else 24
  elif kind==0xd53e5200:value=0x17<<26
  elif kind==0xd5381000:value=0
  if value is not None:u.reg_write(UC_ARM64_REG_X0+reg,value);u.reg_write(UC_ARM64_REG_PC,at+4)
 def writes(cpu,access,at,size,value,cookie):
  state['writes'].append([at,value])
  if at==0x03400000:state['gic_cleanup']=True
  if at==0x03400108:
   if variant=='late-IRQ-after-restore':put(u,0x03400208,128,4)
   if variant=='late-drain-after-restore':state['time']=120000000
 def reads(cpu,access,at,size,value,cookie):
  if state.get('gic_cleanup') and variant=='GICD-drain-timeout' and at==0x03400000:put(u,at,1<<31,4)
  if variant=='GICR-drain-timeout' and at==0x03460000:put(u,at,8,4)
 u.hook_add(UC_HOOK_MEM_READ,reads,begin=0x02000000,end=0x08ffffff)
 u.hook_add(UC_HOOK_CODE,code);u.hook_add(UC_HOOK_MEM_WRITE,writes,begin=0x02000000,end=0x08ffffff)
 u.reg_write(UC_ARM64_REG_X0,0x733);u.reg_write(UC_ARM64_REG_X1,status);u.reg_write(UC_ARM64_REG_X2,1);u.emu_start(e.address('sd_single_core_exception'),0,count=1000000)
 assert state['return']==normal and state['retained']!=normal,(variant,state)
 if variant.endswith('drain-timeout'):assert 120000000<=state['time']<=122400000,state
 if normal:
  reports=[bytes(u.mem_read(0x42000000,128)),bytes(u.mem_read(g['log'],16384)),bytes(u.mem_read(receipt,112))]
  value=h.validate_reports(*reports,{'geometry':g,'execution_profile':h.THERMAL,'epoch':7,'generation':8,'lease':9});assert value['status']==status and value['jobs']==jobs
  for name,raw in zip(('monitor','platform','receipt'),reports):(a.output/(variant+'-'+name+'.bin')).write_bytes(raw)
 results.append({'case':'monitor-'+variant,'passed':True,'normal_return':state['return']})
(a.output/'result.json').write_text(json.dumps({'image_sha256':hashlib.sha256(data).hexdigest(),'checker_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'host_sha256':hashlib.sha256((ROOT/'apps/npu-finite/host.py').read_bytes()).hexdigest(),'cases':results,'scope':__doc__,'physical_access':False},indent=2)+'\n')
print('PASS thermal native receipt/final writer to host:',len(results),'cases')
