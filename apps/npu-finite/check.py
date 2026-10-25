#!/usr/bin/env python3
"""Check new linked NPU session/cleanup/return boundaries with modeled system registers."""
import argparse,hashlib,importlib.util,json,struct
from pathlib import Path
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--evidence',type=Path,required=True);a=p.parse_args();e=a.evidence
root=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('layout',root/'checks/storage-read-native.py');layout=importlib.util.module_from_spec(spec);spec.loader.exec_module(layout);im=layout.Image(e/'image.elf',layout_names=('npu_finite_receipt','a733_observe_identity','a733_npu_application_result','a733_smp_contract'))
def machine():
 u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0x02000000,0x07000000);u.mem_map(0x40000000,0x04000000)
 for at,data in im.parts:
  if data:u.mem_write(at,data)
 u.reg_write(UC_ARM64_REG_SP,0x43ffe000);u.reg_write(UC_ARM64_REG_LR,0x43fff000);return u
def put(u,at,v,n=8):u.mem_write(at,int(v).to_bytes(n,'little'))
def get(u,at,n=8):return int.from_bytes(u.mem_read(at,n),'little')
def ret(u,v=0):u.reg_write(UC_ARM64_REG_X0,v);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
start,end=im.address('__npu_clean_start'),im.address('__device_pool_end');receipt=im.address('npu_finite_receipt');rl=im.layouts['npu_finite_receipt'];results=[]
# Actual production mapping constructor; only privileged register observations modeled.
u=machine();registers={0xd5384240:12,0xd53e1000:0,0xd5381000:0,0xd53b4220:0x3c0,0xd53800a0:0}
def mapping(cpu,at,size,cookie):
 word=get(u,at,4);kind=word&0xffffffe0
 if kind in registers:u.reg_write(UC_ARM64_REG_X0+(word&31),registers[kind]);u.reg_write(UC_ARM64_REG_PC,at+4)
u.hook_add(UC_HOOK_CODE,mapping);u.emu_start(im.address('a733_cached_memory_prepare'),0x43fff000,count=1000000);assert u.reg_read(UC_ARM64_REG_X0)==1
l2=im.address('a733_cached_el1_l2')
for page in range(start,end,4096):
 desc=get(u,l2+8*((page-0x40000000)>>21));assert desc&1 and ((desc>>2)&7)==1 and ((desc>>8)&3)==3 and (desc&0x0000ffffffe00000)==page&~0x1fffff
assert get(u,l2+8*16)==0 # Private monitor/table block inaccessible through EL1.
results.append({'case':'actual-mapping-covers-complete-owned-WB-and-protects-monitor','passed':True,'start':start,'end':end,'pages':(end-start)//4096})
# Normal exit publication: execute every CVAC, compare exact owned interval and tail order.
receipts={}
for variant in ('success','numerical-mismatch','not-quiescent','clean-deadline'):
 u=machine();identity=im.address('a733_observe_identity');ids=im.layouts['a733_observe_identity'];state={'time':24000000,'clean':[],'tail':[],'smc':None,'after_disable_writes':[]}
 for name,v in [('epoch',7),('generation',8),('lease',9),('deadline',480000000)]:put(u,identity+ids[name],v)
 status=int(variant=='numerical-mismatch');put(u,im.address('a733_npu_application_result')+im.layouts['a733_npu_application_result']['complete'],2 if status else 1,4)
 hooks={im.address(n):n for n in ['a733_npu_application_quiescent','aarch64_device_cache_stride']}
 def code(cpu,at,size,cookie):
  word=get(u,at,4);kind=word&0xffffffe0;reg=word&31;name=hooks.get(at)
  if name:ret(u,64 if name=='aarch64_device_cache_stride' else int(variant!='not-quiescent'));return
  if kind==0xd53be020:
   if variant=='clean-deadline' and state['clean']:state['time']=480000000
   u.reg_write(UC_ARM64_REG_X0+reg,state['time']);u.reg_write(UC_ARM64_REG_PC,at+4);return
  if kind==0xd50b7a20:state['clean'].append(u.reg_read(UC_ARM64_REG_X0+reg));u.reg_write(UC_ARM64_REG_PC,at+4);return
  if kind==0xd5381000:u.reg_write(UC_ARM64_REG_X0+reg,0x30d01805);u.reg_write(UC_ARM64_REG_PC,at+4);return
  if kind==0xd5181000:state['tail'].append(['SCTLR',u.reg_read(UC_ARM64_REG_X0+reg)]);u.reg_write(UC_ARM64_REG_PC,at+4);return
  if word==0xd5033f9f:state['tail'].append(['DSB'])
  if word==0xd5033fdf:state['tail'].append(['ISB'])
  if word==0xd4000003:state['smc']=[u.reg_read(UC_ARM64_REG_X0),u.reg_read(UC_ARM64_REG_X0+1)];u.emu_stop()
 def writes(cpu,access,at,size,value,cookie):
  if any(x[0]=='SCTLR' for x in state['tail']):state['after_disable_writes'].append(at)
 u.hook_add(UC_HOOK_CODE,code);u.hook_add(UC_HOOK_MEM_WRITE,writes);u.reg_write(UC_ARM64_REG_X0,status);u.emu_start(im.address('platform_exit'),0,count=2000000)
 assert state['smc']==[0x733,4 if variant in ('not-quiescent','clean-deadline') else status],(variant,state['smc'])
 assert not state['after_disable_writes']
 if variant in ('success','numerical-mismatch'):
  assert state['clean']==list(range(start,end,64)),(variant,len(state['clean']))
  assert state['tail'][-3:]==[['DSB'],['SCTLR',0x30d01805&~0x1005],['ISB']],state['tail'][-5:]
  raw=bytes(u.mem_read(receipt,88));receipts[status]=raw;assert get(u,receipt+rl['clean_lines'])==(end-start)//64
 results.append({'case':variant,'passed':True,'clean_lines':len(state['clean']),'smc':state['smc'],'no_stores_after_EL1_disable':True})
# EL3 receipt/refusal + bounded GIC/secondary cleanup. Reuse the actual successful receipt.
# SYSCTRL 0x03000008 reads 0 (shared SRAM routed to the NPU) unless a case sets it.
SRAM_ROUTING=0x03000008;REPORT=0x42000000  # A733_SMP_RESULT_ADDRESS, the monitor report.
variants=['success','numerical-mismatch','SRAM-already-CPU','SRAM-restore-ignored','SRAM-reset-released','SRAM-module-gate-open','wrong-magic','wrong-epoch','wrong-generation','wrong-lease','wrong-status','wrong-complete','wrong-start','wrong-end','wrong-lines','wrong-deadline','long-log','unexpected-SMC','recovery-IRQ','late','EL1-cache-live','secondary-on','GICD-timeout','GICR-timeout']
for variant in variants:
 u=machine();status=int(variant=='numerical-mismatch');u.mem_write(receipt,receipts[status]);admission=im.address('admission');c=im.layouts['a733_smp_contract'];state={'time':24000000,'return':False,'retained':False,'writes':[]}
 for name,v in [('epoch',7),('generation',8),('lease',9),('diagnostic_deadline',480000000)]:put(u,admission+c[name],v)
 # Original saved GIC record has eight uint32 fields followed by 32 enable words.
 saved=im.address('saved');u.mem_write(saved,struct.pack('<8I',1,4,0x33,0x123,0x456,0,0,0))
 change={'wrong-magic':('magic',1),'wrong-epoch':('epoch',10),'wrong-generation':('generation',10),'wrong-lease':('lease',10),'wrong-status':('status',10),'wrong-complete':('complete',10),'wrong-start':('clean_start',start+4096),'wrong-end':('clean_end',end+4096),'wrong-lines':('clean_lines',1),'wrong-deadline':('deadline',1),'long-log':('log_length',16384)}
 if variant in change:name,v=change[variant];put(u,receipt+rl[name],v)
 if variant=='secondary-on':put(u,0x07053008,8,4)
 if variant=='late':state['time']=480000000
 if variant=='SRAM-already-CPU':put(u,SRAM_ROUTING,2,4)
 if variant=='SRAM-reset-released':put(u,0x02002b04,0x00090000,4)  # Core reset released.
 if variant=='SRAM-module-gate-open':put(u,0x02002b00,0x80000000,4)
 hooks={im.address(n):n for n in ['a733_smp_stop','npu_finite_rom_return']}
 def code(cpu,at,size,cookie):
  word=get(u,at,4);kind=word&0xffffffe0;reg=word&31;name=hooks.get(at)
  if kind in (0xd51fe220,0xd51be220,0xd51be320):u.reg_write(UC_ARM64_REG_PC,at+4);return
  if name:
   state['return']=name=='npu_finite_rom_return';state['retained']=name=='a733_smp_stop'
   state['stop']=[u.reg_read(UC_ARM64_REG_X0),u.reg_read(UC_ARM64_REG_X0+1)];u.emu_stop();return
  value=None
  if kind==0xd53be020:value=state['time'];state['time']+=240000
  elif kind==0xd53e5200:value=0x17<<26
  elif kind==0xd5381000:value=4 if variant=='EL1-cache-live' else 0
  if value is not None:u.reg_write(UC_ARM64_REG_X0+reg,value);u.reg_write(UC_ARM64_REG_PC,at+4)
 def writes(cpu,access,at,size,value,cookie):state['writes'].append([at,value])
 def reads(cpu,access,at,size,value,cookie):
  if variant=='SRAM-restore-ignored' and at==SRAM_ROUTING:put(u,at,0,4)  # A word that ignores writes.
  if variant=='GICD-timeout' and at==0x03400000:put(u,at,1<<31,4)
  if variant=='GICR-timeout' and at==0x03460000:put(u,at,8,4)
 from unicorn import UC_HOOK_MEM_READ
 u.hook_add(UC_HOOK_CODE,code);u.hook_add(UC_HOOK_MEM_WRITE,writes,begin=0x02000000,end=0x08ffffff);u.hook_add(UC_HOOK_MEM_READ,reads,begin=0x02000000,end=0x08ffffff)
 u.hook_add(UC_HOOK_MEM_WRITE,writes,begin=REPORT,end=REPORT+0xfff)
 u.reg_write(UC_ARM64_REG_X0,0x734 if variant=='unexpected-SMC' else 0x733);u.reg_write(UC_ARM64_REG_X0+1,status);u.reg_write(UC_ARM64_REG_X0+2,2 if variant=='recovery-IRQ' else 1)
 
 try:u.emu_start(im.address('sd_single_core_exception'),0,count=2000000)
 except Exception:
  print('monitor failure',variant,hex(u.reg_read(UC_ARM64_REG_PC)),hex(get(u,u.reg_read(UC_ARM64_REG_PC),4)));raise
 normal=variant in ('success','numerical-mismatch','SRAM-already-CPU');assert state['return']==normal and state['retained']!=normal,(variant,state)
 assert not any(at in (0x08001004,0x08001008) for at,v in state['writes'])
 # The shared SRAM goes back to the CPU before the report and the ROM return;
 # a refused receipt or a failed cleanup parks before that step.
 routing=[i for i,(at,v) in enumerate(state['writes']) if at==SRAM_ROUTING]
 report=[i for i,(at,v) in enumerate(state['writes']) if REPORT<=at<REPORT+0x1000]
 if variant in ('success','numerical-mismatch'):
  assert len(routing)==1 and state['writes'][routing[0]][1]==2 and report and all(i>routing[0] for i in report),(variant,state['writes'])
 elif variant=='SRAM-already-CPU':assert not routing
 elif variant in ('SRAM-reset-released','SRAM-module-gate-open'):
  # The held-reset check parks before the routing write and the report.
  assert not routing and state['stop']==[7,SRAM_ROUTING] and not report,(variant,state)
 elif variant=='SRAM-restore-ignored':
  # a733_smp_stop is stopped at its entry, so it writes no report here.
  assert len(routing)==1 and state['stop']==[7,SRAM_ROUTING] and not report,(variant,state)
 else:assert not routing,(variant,routing)
 results.append({'case':'EL3-'+variant,'passed':True,'normal_return':state['return'],'retained':state['retained'],
                 'sram_routing_writes':len(routing)})
# Execute the accepted local ROM tail up to the modeled warm-reset request.
u=machine();state={'cache_calls':0,'writes':[],'reset':False}
def rom(cpu,at,size,cookie):
 word=get(u,at,4)
 if at==im.address('clean_data_cache'):state['cache_calls']+=1;ret(u);return
 if word&0xffffffe0==0xd51ec040:
  assert u.reg_read(UC_ARM64_REG_X0+(word&31))==2;state['reset']=True;u.emu_stop()
def rom_write(cpu,access,at,size,value,cookie):state['writes'].append([at,size,value])
u.hook_add(UC_HOOK_CODE,rom);u.hook_add(UC_HOOK_MEM_WRITE,rom_write,begin=0x02000000,end=0x08ffffff)
u.emu_start(im.address('npu_finite_rom_return'),0,count=10000)
assert state=={'cache_calls':1,'writes':[[0x08001004,4,0x20],[0x08001008,4,0]],'reset':True},state
results.append({'case':'accepted-forced-FEL-tail-cache-completion-and-exact-reset-writes','passed':True,**state})
(e/'return-check.json').write_text(json.dumps({'elf_sha256':hashlib.sha256((e/'image.elf').read_bytes()).hexdigest(),'cases':results,'scope':'New owned-cache publication, identity tail and EL3 receipt/cleanup instructions. System registers, cache effects, timer, GIC and CPU status modeled. Existing session validator, client quiescence and normal NPU shutdown evidence reused; no inference or physical ROM return.'},indent=2)+'\n');print('PASS new NPU mapping/cache/return cases:',len(results))
