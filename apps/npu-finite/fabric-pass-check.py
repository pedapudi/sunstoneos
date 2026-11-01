#!/usr/bin/env python3
"""Run the linked NPU PASS against a development record and post-entry MBUS."""
import argparse, hashlib, importlib.util, json, struct, sys
from pathlib import Path
from unicorn import UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_PC
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--run',type=Path,required=True);p.add_argument('--image',type=Path)
p.add_argument('--evidence',type=Path,required=True);p.add_argument('--require-stages',action='store_true')
a=p.parse_args();a.evidence.mkdir(parents=True,exist_ok=True)
r=Path(__file__).resolve().parents[2];image=a.image or a.run/'bundle/image.elf'
def load(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
# Reuse the existing native sequencer machine and its defining-CU layout selection.
helper=r/'apps/npu-finite/startup-failure-check.py';ns={'__file__':str(helper),'__name__':'fabric_pass_model'}
args=sys.argv;sys.argv=[str(helper),'--image',str(image),'--evidence',str(a.evidence)]
try:exec(compile(helper.read_text().split('\ncases=',1)[0],str(helper),'exec'),ns)
finally:sys.argv=args
Machine,COLD,CCU,RESULT=(ns[n] for n in ('Machine','COLD','CCU','RESULT'));im,L=ns['im'],ns['namespace']['L']
finite=load('fabric_pass_host',r/'apps/npu-interrupt/host.py').finite
dev=load('fabric_pass_development',r/'apps/npu-finite/development.py')
data=image.read_bytes();template=(a.run/'bundle/session.bin').read_bytes();live=json.loads((a.run/'state.json').read_text())
layout=finite.original.image_data(data)[3];raw,_,derived,_=dev.session(finite,data,layout,template,live)
record=struct.unpack_from('<6I',raw,layout[1]+layout[3]);fabric,rates=record[:3],record[3:]
results=[]
expected_stages=list(range(0x0630,0x0637))+[0x0637]*3+list(range(0x0638,0x0644))
def case(name,changes=None,record_changes=None):
 words=dict(COLD);words.update({int(k,16):v for k,v in live['words'].items()})
 # Existing EL3 normalization is separately checked by checks/el3-mbus.py.
 # Model its successful final word, preserving all other captured readbacks.
 words[CCU+0x588]=0x81000000
 if changes:words.update(changes)
 m=Machine(words);state={'svc':0,'resume':None,'stages':[],'stage_values':[],'fault_after':None}
 config=m.s+L['a733_npu_startup']['config']+L['a733_npu_startup_config']['fabric']
 expected=list(fabric)
 if record_changes:
  for index,value in record_changes.items():expected[index]=value
 for i in range(3):
  m.put(config+L['a733_npu_platform_config']['fabric']+4*i,expected[i],4)
  m.put(config+L['a733_npu_platform_config']['rate_hz']+4*i,rates[i],4)
 def code(u,at,size,cookie):
  instruction=m.get(at,4);kind=instruction&0xffffffe0;reg=instruction&31
  if instruction&0xffe0001f==0xd4000001:
   state['svc']=(instruction>>5)&0xffff;assert state['svc'] in (0x5c0d,0x5c0e)
   stage=u.reg_read(UC_ARM64_REG_X0)
   if state['svc']==0x5c0d and 0x0630<=stage<=0x0643:
    state['stages'].append(stage);state['stage_values'].append((stage,u.reg_read(UC_ARM64_REG_X0+1)&0xffff))
   if state['svc']==0x5c0e and state['fault_after'] is None:state['fault_after']=list(state['stages'])
   state['resume']=at+4;u.reg_write(UC_ARM64_REG_PC,im.address('npu_finite_svc'));return
  if instruction==0xd69f03e0:
   u.reg_write(UC_ARM64_REG_PC,state['resume']);state['resume']=None;return
  if kind in (0xd5385200,0xd53800a0):
   u.reg_write(UC_ARM64_REG_X0+reg,(0x15<<26|state['svc']) if kind==0xd5385200 else 0);u.reg_write(UC_ARM64_REG_PC,at+4)
 m.u.hook_add(UC_HOOK_CODE,code)
 first=m.step();healthy=name=='healthy'
 if healthy:
  assert first==RESULT['A733_NPU_PLATFORM_PENDING'],('post-entry fabric record refused',list(map(hex,fabric)),list(rates),hex(m.get(0x0709011c,4)))
  m.answer_row();assert m.step()==RESULT['A733_NPU_PLATFORM_PENDING']
  m.time+=3000;assert m.step()==RESULT['A733_NPU_PLATFORM_READY']
  assert not m.get(0x07090110,4)
  if a.require_stages:
   assert state['stages']==expected_stages,state
   assert [v for stage,v in state['stage_values'] if stage==0x0637]==[0,1,2],state
 else:
  assert first==RESULT['A733_NPU_PLATFORM_RETAINED'],(name,first)
  assert m.get(0x07090110,4)>>24==0xfa,(name,state)
  if a.require_stages:
   assert state['fault_after'] and state['stages']==state['fault_after'],(name,state)
   assert state['stages']==expected_stages[:len(state['stages'])],(name,state)
 results.append({'case':name,'passed':True,'stages':[hex(s) for s in state['stages']],'stage_values':[(hex(stage),value) for stage,value in state['stage_values']],'fault_value':hex(m.get(0x0709011c,4)),'startup_result':first})
case('healthy')
assert fabric[2]==0x81000000 and rates[2]==600000000,(fabric,rates)
case('normalization-skipped',{CCU+0x588:0x84000000})
case('AHB-changed',{CCU+0x500:live['words'][hex(CCU+0x500)]^1})
case('NSI-changed',{CCU+0x580:live['words'][hex(CCU+0x580)]^1})
case('MBUS-update-pending',{CCU+0x588:0x89000000})
case('NSI-update-pending',{CCU+0x580:live['words'][hex(CCU+0x580)]|0x08000000})
case('PERI0-600-NoAuto-missing',{CCU+0x1908:COLD[CCU+0x1908]&~(1<<9)})
case('reference-oscillator-disabled',{0x07090160:2})
case('SRAM-already-NPU',{0x03000008:0})
case('video-encoder-open',{CCU+0xa80:0x80000000})
case('video-decoder-selector-invalid',{CCU+0xa88:0x07000000})
case('video-reset-released',{CCU+0xa8c:0x50005})
case('DRAM-clock-update',{CCU+0xc00:0x88000000})
case('DRAM-PHY-reset-asserted',{CCU+0xc0c:0})
case('adopted-PLL-gate-missing',{CCU+0x2a0:0xf8104600,CCU+0x1920:0})
case('speed-bin-unsupported',{0x0300624c:0})
case('autonomous-NPU-mode',{CCU+0x1b1c:1})
case('NPU-pattern-enabled',{CCU+0x2a4:1})
(a.evidence/'results.json').write_text(json.dumps({'passed':True,'image_sha256':hashlib.sha256(data).hexdigest(),'captured_session_sha256':hashlib.sha256(template).hexdigest(),'fabric_record':list(fabric),'fabric_hz':list(rates),'derived':derived,'cases':results,'scope':'Actual linked sequencer/native adapter/SVC handlers. Captured system words supplemented by existing nominal fixture values for uncaptured prerequisites; post-entry MBUS success or skip is modeled. No physical qualification.'},indent=2)+'\n')
print('PASS linked full startup and discriminating fabric/guard refusals:',len(results))
