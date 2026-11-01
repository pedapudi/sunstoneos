#!/usr/bin/env python3
"""Execute the linked SDK close wrapper and native quiesce ownership gate.

The gate is actual native code. Operations after authorization are modeled;
this check does not establish provider parking or physical NPU operation.
"""
import argparse
import hashlib
import json
import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'checks'))
from harness import Image
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--image',type=Path,required=True)
p.add_argument('--out',type=Path,required=True)
a=p.parse_args();a.out.mkdir(parents=True,exist_ok=False)
im=Image(a.image,layout_names=('a733_observe_identity',))
with a.image.open('rb') as stream:
 dwarf=ELFFile(stream).get_dwarf_info()
 candidates=[]
 for cu in dwarf.iter_CUs():
  for die in cu.iter_DIEs():
   name=die.attributes.get('DW_AT_name')
   if die.tag!='DW_TAG_variable' or not name or name.value!=b'app' or 'DW_AT_type' not in die.attributes:continue
   typ=die.get_DIE_from_attribute('DW_AT_type')
   members={d.attributes['DW_AT_name'].value.decode():int(d.attributes['DW_AT_data_member_location'].value)
            for d in typ.iter_children() if d.tag=='DW_TAG_member'}
   if all(n in members for n in ('client','runtime','pmic_parked','quiescent')):candidates.append(members)
 assert len(candidates)==1,candidates
 client_offset=candidates[0]['client']
# Find the fallthrough of the actual owner comparison from the linked BL to
# xTaskGetCurrentTaskHandle and its following conditional rejection branch.
entry=im.address('a733_npu_application_quiesce');current_fn=im.address('xTaskGetCurrentTaskHandle')
code_bytes=next(data[entry-at:entry-at+256] for at,data in im.parts if at<=entry<at+len(data))
gates=[]
for offset in range(0,len(code_bytes),4):
 word=int.from_bytes(code_bytes[offset:offset+4],'little')
 if word>>26!=0b100101:continue
 delta=(word&0x3ffffff);delta=delta-(1<<26) if delta&(1<<25) else delta
 if entry+offset+4*delta!=current_fn:continue
 for tail in range(offset+4,min(offset+36,len(code_bytes)),4):
  branch=int.from_bytes(code_bytes[tail:tail+4],'little')
  if branch&0xff00001f==0x54000001:gates.append(entry+tail+4);break
assert len(gates)==1,gates
gate_passed=gates[0]
rows=[]
for case,entry,current,expected in [('wrong-application','a733_npu_application_quiesce',0x1230,0),
                                    ('service-close','sdk_close',0x4560,1)]:
 u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0x40000000,0x04000000)
 for at,data in im.parts:
  if data:u.mem_write(at,data)
 def put(at,v,n=8):u.mem_write(at,int(v).to_bytes(n,'little'))
 def ret(value):u.reg_write(UC_ARM64_REG_X0,value);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
 # Resolve the private app variable from its defining native-runtime unit.
 owners=[addr for (cu,name),addresses in im.variables.items() if name=='app' and 'native-runtime.c' in cu for addr in addresses]
 assert len(owners)==1,owners
 put(owners[0]+client_offset,0x4560)
 put(im.address('a733_observe_identity')+im.layouts['a733_observe_identity']['deadline'],480000000)
 state={'authorized':False,'quiesce_return':None,'closed':False,'result':None}
 sentinel=0x43fff000
 hooks={im.address(n):n for n in ('xTaskGetSchedulerState','xTaskGetCurrentTaskHandle','npu_job_close',
        'a733_npu_application_stopped','a733_npu_application_quiesce')}
 def code(cpu,at,size,cookie):
  if at==sentinel:state['result']=u.reg_read(UC_ARM64_REG_X0);u.emu_stop();return
  if at==gate_passed:
   state['authorized']=True;u.context_restore(state['context']);u.reg_write(UC_ARM64_REG_X0,1);u.reg_write(UC_ARM64_REG_PC,state['quiesce_return']);return
  name=hooks.get(at)
  if name=='xTaskGetSchedulerState':ret(2)
  elif name=='xTaskGetCurrentTaskHandle':ret(current)
  elif name=='npu_job_close':state['closed']=True;ret(1)
  elif name=='a733_npu_application_quiesce':
   state['quiesce_return']=u.reg_read(UC_ARM64_REG_LR);state['context']=u.context_save()
  elif name=='a733_npu_application_stopped':ret(1)
  else:
   word=int.from_bytes(u.mem_read(at,4),'little')
   if word&0xffe0001f==0xd4000001:
    assert (word>>5)&0xffff in (0x5c0d,0x5c0e);u.reg_write(UC_ARM64_REG_PC,at+4);return
   if word&0xffffffe0==0xd53be020:
    u.reg_write(UC_ARM64_REG_X0+(word&31),0);u.reg_write(UC_ARM64_REG_PC,at+4)
 u.hook_add(UC_HOOK_CODE,code)
 u.reg_write(UC_ARM64_REG_SP,0x43ffe000);u.reg_write(UC_ARM64_REG_LR,sentinel)
 try:u.emu_start(im.address(entry),0,count=100000)
 except Exception:
  pc=u.reg_read(UC_ARM64_REG_PC);print(case,hex(pc),bytes(u.mem_read(pc,4)).hex());raise
 assert state['result']==expected,state
 assert state['authorized']==bool(expected),state
 assert state['closed']==bool(expected),state
 state.pop('context',None);rows.append({'case':case,'passed':True,**state})
(a.out/'result.json').write_text(json.dumps({'passed':True,'image_sha256':hashlib.sha256(a.image.read_bytes()).hexdigest(),
 'cases':rows,'physical_access':False,'scope':__doc__},indent=2)+'\n')
print('PASS actual quiesce ownership gate: application refused; SDK close owner admitted')
