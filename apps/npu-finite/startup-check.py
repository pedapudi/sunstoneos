#!/usr/bin/env python3
"""Run the linked startup client, scalar gates, receipt and monitor writer; model backend replies.

The same cases, and the job cases, run against the single-inference image."""
import argparse,hashlib,importlib.util,json,struct
from pathlib import Path
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR
p=argparse.ArgumentParser(description=__doc__)
for n in ('native','evidence'):p.add_argument('--'+n,type=Path,required=True)
p.add_argument('--case',action='append');a=p.parse_args();a.evidence.mkdir(parents=True,exist_ok=True)
r=Path(__file__).resolve().parents[2]
def module(n,path):
 s=importlib.util.spec_from_file_location(n,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
layout=module('startup_layout',r/'checks/storage-read-native.py');host=module('startup_host',r/'apps/npu-interrupt/host.py');h=host.finite
im=layout.Image(a.native/'image.elf',layout_names=('npu_convolution','npu_service','npu_service_reply','a733_observe_identity','a733_smp_contract','a733_npu_application_result','a733_npu_controller'))
_,_,_,_,g=h.image_profile((a.native/'image.elf').read_bytes(),json.loads((a.native/'manifest.json').read_text()))
assert g['startup_only'];infer=bool(g.get('single_inference'));cl=im.layouts['npu_convolution'];sl=im.layouts['npu_service'];reply=im.layouts['npu_service_reply'];ar=im.layouts['a733_npu_application_result']
irq_offset=None
from elftools.elf.elffile import ELFFile
with (a.native/'image.elf').open('rb') as f:
 for cu in ELFFile(f).get_dwarf_info().iter_CUs():
  for die in cu.iter_DIEs():
   if die.tag=='DW_TAG_structure_type':
    fields={c.attributes['DW_AT_name'].value.decode():c.attributes['DW_AT_data_member_location'].value for c in die.iter_children() if c.tag=='DW_TAG_member' and 'DW_AT_name' in c.attributes and 'DW_AT_data_member_location' in c.attributes}
    if 'npu_irq' in fields and 'spi_enabled' in fields:irq_offset=fields['npu_irq']
assert irq_offset is not None
def linked(name):
 with (a.native/'image.elf').open('rb') as f:
  s=next(x for x in ELFFile(f).get_section_by_name('.symtab').iter_symbols() if x.name==name);at,n=int(s['st_value']),int(s['st_size'])
 return next(d[at-base:at-base+n] for base,d in im.parts if d and base<=at and at+n<=base+len(d))
image_input=linked('a733_npu_application_input')
variants=('matched','zero-IRQ','extra-IRQ','job-before-close','output-before-close','close-fault','missing-IRQ-stop','stop-timeout','park-failed','pending-ticket','result-jobs','result-job','service-ticket','unclosed','late-publication','state-lost-before-publication')
if infer:variants=tuple(v for v in variants if v not in ('job-before-close','output-before-close'))+('mismatch','unwritten','zero-ticket','job-not-done','job-one-IRQ','job-three-IRQs','output-copy-failed','reap-not-ready','reap-keeps-job')
reference=bytes.fromhex(g['reference']);poison=h.output_poison(len(reference))
device=bytes([reference[0]^1])+reference[1:]
selected=a.case or variants;assert set(selected)<=set(variants)
identity=hashlib.sha256((a.native/'image.elf').read_bytes()).hexdigest();result_path=a.evidence/'result.json'
report=json.loads(result_path.read_text()) if result_path.exists() else {'passed':True,'elf_sha256':identity,'check_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'cases':{}}
assert report['elf_sha256']==identity and report['check_sha256']==hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
for variant in selected:
 if report['cases'].get(variant,{}).get('passed'):continue
 u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0x02000000,0x07000000);u.mem_map(0x40000000,0x04000000)
 for at,data in im.parts:
  if data:u.mem_write(at,data)
 def put(at,v,n=8):u.mem_write(at,int(v).to_bytes(n,'little'))
 def get(at,n=8):return int.from_bytes(u.mem_read(at,n),'little')
 def ret(v=0):u.reg_write(UC_ARM64_REG_X0,v);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
 c=0x43000000;service=c+cl['service'];app_result=im.address('a733_npu_application_result');deadline=490000000
 put(im.address('instance',unit='/drivers/npu/npu_service.c'),service);put(service+sl['irq_bound'],1,4)
 put(service+sl['irq_completions'],0 if variant=='zero-IRQ' else 2 if variant=='extra-IRQ' else 1,4)
 ids=im.layouts['a733_observe_identity']
 for n,v in [('epoch',7),('generation',8),('lease',9),('armed_at',10000000),('deadline',deadline)]:put(im.address('a733_observe_identity')+ids[n],v)
 put(g['log'],0x534b4552);put(g['log']+8,1);put(g['log']+64,11000000)
 state={'ticks':11000000,'operations':[],'ticket':0,'closed':False,'stopped':False,'parked':False,'delays':0,'retained':False,'return':False,'smc':None,'clean':[],'sctlr':0x1005,'el':4,'phase':'client'}
 names=('a733_npu_application_ready','a733_npu_application_client','npu_convolution_send','npu_convolution_collect','a733_npu_application_stopped','a733_npu_application_quiesce','a733_npu_application_quiescent','aarch64_device_cache_stride','vPortEnterCritical','vPortExitCritical','vTaskSuspend','vTaskDelay','a733_npu_application_fail','a733_smp_stop','npu_finite_rom_return','platform_exit')
 hooks={im.address(n):n for n in names+(('npu_convolution_output',) if infer else ())}
 def code(cpu,at,size,cookie):
  name=hooks.get(at);args=[u.reg_read(UC_ARM64_REG_X0+i) for i in range(4)]
  if name=='a733_npu_application_ready':ret(1);return
  if name=='a733_npu_application_client':ret(c);return
  if name=='npu_convolution_send':
   assert args[0]==c and args[1] in ((1,2,4,5,6) if infer else (1,5,6)),args
   if args[1]==2:assert args[3]==len(image_input) and bytes(u.mem_read(args[2],args[3]))==image_input,args
   state['operation']=args[1];state['operations'].append(args[1]);state['ticket']+=1
   put(u.reg_read(UC_ARM64_REG_X0+6),state['ticket']);ret(1);return
  if name=='npu_convolution_collect':
   op=state['operation'];out=args[2];u.mem_write(out,bytes(40));put(out+reply['ticket'],args[1])
   failed=op==5 and variant=='close-fault';st=9 if op==5 else 2 if op in (4,6) else 1
   job=get(c+cl['job'])
   if op==2:st=3;job=0 if variant=='zero-ticket' else args[1];put(c+cl['job'],job);put(c+cl['ready'],0,4)
   if op==6 and job:
    st=2 if variant=='job-not-done' else 6
    put(service+sl['irq_completions'],1 if variant=='job-one-IRQ' else 3 if variant=='job-three-IRQs' else 2,4)
    if st==6:put(c+cl['output_ready'],1,4)
   for n,v in [('job_ticket',job if op in (2,6) else 0),('result',4 if failed else 0),('state',st),('owns_session',0 if op==5 and not failed else 1)]:put(out+reply[n],v,8 if n=='job_ticket' else 4)
   if op==1:put(c+cl['opened'],1,4)
   if op==4:
    put(c+cl['output_ready'],0,4)
    if variant!='reap-keeps-job':put(c+cl['job'],0)
    put(c+cl['ready'],1,4)
    if variant=='reap-not-ready':put(out+reply['state'],6,4)
   if op==6 and not job:
    put(c+cl['ready'],1,4)
    if variant=='job-before-close':put(c+cl['job'],1)
    if variant=='output-before-close':put(c+cl['output_ready'],1,4)
   if op==5 and not failed:
    state['closed']=True;put(c+cl['ready'],0,4);put(c+cl['closed'],int(variant!='unclosed'),4)
    put(service+sl['controller']+im.layouts['a733_npu_controller']['state'],9,4);put(service+sl['irq_masked'],1,4);put(service+sl['irq_stopped'],int(variant!='missing-IRQ-stop'),4)
   ret(1);return
  if name=='npu_convolution_output':
   assert infer and args[0]==c and args[2]==len(reference) and get(c+cl['output_ready'],4),args
   u.mem_write(args[1],device if variant=='mismatch' else poison if variant=='unwritten' else reference);ret(int(variant!='output-copy-failed'));return
  if name=='a733_npu_application_stopped':
   assert state['closed'];state['stopped']=variant!='stop-timeout';ret(int(state['stopped']));return
  if name=='a733_npu_application_quiesce':
   assert state['stopped'];state['parked']=variant!='park-failed'
   if variant=='pending-ticket':put(c+cl['pending'],1)
   if variant=='service-ticket':put(service+sl['job_ticket'],1)
   if variant=='result-jobs':put(app_result+ar['jobs'],2 if infer else 1,4)
   if variant=='result-job':put(app_result+ar['job'],0 if infer else 1)
   ret(int(state['parked']));return
  if name=='a733_npu_application_quiescent':ret(int(state['parked']));return
  if name=='aarch64_device_cache_stride':ret(64);return
  if name in ('vPortEnterCritical','vPortExitCritical'):ret();return
  if name=='vTaskDelay':state['ticks']+=240000000 if variant=='stop-timeout' else 24000;state['delays']+=1;ret();return
  if name=='a733_npu_application_fail':put(im.address('a733_npu_application_failed'),args[0],4);ret();return
  if name in ('vTaskSuspend','a733_smp_stop'):state['retained']=True;u.emu_stop();return
  if name=='npu_finite_rom_return':state['return']=True;u.emu_stop();return
  if name=='platform_exit':
   if variant=='state-lost-before-publication':put(app_result+ar['jobs'],0 if infer else 1,4)
   if variant=='late-publication':state['ticks']=deadline
  word=get(at,4);kind=word&0xffffffe0;reg=word&31
  values={0xd5384240:state['el'],0xd53800a0:0,0xd5381000:state['sctlr'],0xd53e5200:0x17<<26,0xd53be020:state['ticks']}
  if kind in values:
   if reg<31:u.reg_write(UC_ARM64_REG_X0+reg,values[kind])
   u.reg_write(UC_ARM64_REG_PC,at+4);return
  if kind==0xd50b7a20:state['clean'].append(u.reg_read(UC_ARM64_REG_X0+reg));u.reg_write(UC_ARM64_REG_PC,at+4);return
  if kind==0xd5181000:state['sctlr']=u.reg_read(UC_ARM64_REG_X0+reg)
  if word&0xfff00000==0xd5100000 or word&0xfffff000 in (0xd5033000,0xd5088000):u.reg_write(UC_ARM64_REG_PC,at+4);return
  if word==0xd4000003:state['smc']=args[:2];u.emu_stop();return
 def mmio(cpu,access,at,size,value,cookie):
  assert not (0x03600000<=at<0x03700000 or 0x07064000<=at<0x07065000 or 0x07083000<=at<0x07084000),hex(at)
 u.hook_add(UC_HOOK_CODE,code);u.hook_add(UC_HOOK_MEM_READ,mmio,begin=0x02000000,end=0x08ffffff);u.hook_add(UC_HOOK_MEM_WRITE,mmio,begin=0x02000000,end=0x08ffffff)
 u.reg_write(UC_ARM64_REG_SP,0x43ffe000);u.reg_write(UC_ARM64_REG_LR,0x43fff000);u.emu_start(im.address('npu_startup_client'),0,count=2000000)
 if variant in ('matched','mismatch','unwritten'):
  status=int(variant!='matched')
  assert not state['retained'] and state['smc']==[0x733,status] and state['operations']==([1,6,2,6,4,5] if infer else [1,6,5]),state
  assert state['clean']==list(range(g['clean_start'],g['clean_end'],64))
  state.update(phase='monitor',el=12,sctlr=0)
  contract=im.layouts['a733_smp_contract']
  for n,v in [('epoch',7),('generation',8),('lease',9),('armed_at',10000000),('diagnostic_deadline',deadline)]:put(im.address('admission')+contract[n],v)
  u.mem_write(im.address('saved'),struct.pack('<8I',1,4,0x33,0x123,0x456,0,0,0))
  u.mem_write(im.address('saved')+irq_offset,struct.pack('<Q5I',0x10000,2,0,8,0x5800,2))
  put(0x03400000,0x30,4);put(0x03400004,7,4);put(0x03000008,2,4)
  put(0x42000000,0x415337334e505231);put(0x42000008,7);put(0x42000010,8);put(0x42000030,11000000)
  for i,v in enumerate((0x733,status,1)):u.reg_write(UC_ARM64_REG_X0+i,v)
  u.reg_write(UC_ARM64_REG_SP,0x43ffe000);u.emu_start(im.address('sd_single_core_exception'),0,count=1000000)
  assert state['return'] and not state['retained'],state
  frames=[bytes(u.mem_read(at,size)) for at,size in ((0x42000000,128),(g['log'],16384),(g['receipt'],88))]
  parsed=h.validate_reports(*frames,{'execution_profile':h.INFERENCE if infer else h.STARTUP,'geometry':g,'epoch':7,'generation':8,'lease':9})
  if infer:
   assert parsed['single_inference'] and parsed['jobs']==1 and parsed['interrupt_completions']==2 and parsed['status']==status,parsed
   assert parsed['functional_success']==(variant=='matched') and bytes.fromhex(parsed['output'])==(reference if variant=='matched' else device if variant=='mismatch' else poison)
   assert parsed['output_pattern']['second_unwritten']==(variant=='unwritten')
  else:assert parsed['startup_bootstrap'] and parsed['jobs']==0 and not parsed['functional_success'] and parsed['interrupt_completions']==1
  for n,data in zip(('monitor','platform','receipt'),frames):(a.evidence/(variant+'-'+n+'.bin')).write_bytes(data)
  (a.evidence/(variant+'.log')).write_text(parsed['log'])
 else:assert state['retained'] or state['smc']==[0x733,4],(variant,state)
 report['cases'][variant]={'passed':True,'operations':state['operations'],'normal_return':state['return'],'retained':state['retained'],'smc':state['smc'],'clean_lines':len(state['clean'])}
 report['scope']='Actual linked client and request validation, IRQ receipt queries, zero-job scalar gate, native log writer, receipt/cache clean and EL3 writer/route restore execute. Replies, provider READY/local-stop/park, scheduler, hardware and CPU effects are modeled; unchanged provider/controller/service evidence is separate. No physical startup, IRQ or return qualification.'
 result_path.write_text(json.dumps(report,indent=2)+'\n')
print('PASS selected startup-only client/receipt/monitor cases',len(selected))
