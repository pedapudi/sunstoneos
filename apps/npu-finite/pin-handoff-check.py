#!/usr/bin/env python3
"""Replay the captured PMIC pin mismatch through the linked PMIC and pin provider."""
import argparse, hashlib, importlib.util, json, struct, sys
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_LR
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--development-source',type=Path);p.add_argument('--image',type=Path);p.add_argument('--run',type=Path,required=True);p.add_argument('--evidence',type=Path,required=True)
a=p.parse_args();a.evidence.mkdir(parents=True,exist_ok=True)
r=Path(__file__).resolve().parents[2]
def load(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
h=load('pin_handoff_harness',r/'checks/harness.py')
image=a.image or a.run/'bundle/image.elf';data=image.read_bytes();raw=(a.run/'bundle/session.bin').read_bytes();live=json.loads((a.run/'state.json').read_text())
im=h.Image(image,layout_names=('a733_pins','a733_pins_operations','a733_rail_leases','a733_power_authority','a733_power_domains','a733_power_service_config'))
# PMIC's defining unit avoids the client's macro-renamed retained member.
with image.open('rb') as f:
 for cu in ELFFile(f).get_dwarf_info().iter_CUs():
  if not cu.get_top_DIE().attributes['DW_AT_name'].value.decode().endswith('/drivers/power/a733_pmic.c'):continue
  for die in cu.iter_DIEs():
   if die.tag=='DW_TAG_structure_type' and die.attributes.get('DW_AT_name') and die.attributes['DW_AT_name'].value==b'a733_pmic':
    fields={c.attributes['DW_AT_name'].value.decode():int(c.attributes['DW_AT_data_member_location'].value) for c in die.iter_children() if c.tag=='DW_TAG_member' and 'DW_AT_name' in c.attributes and 'DW_AT_data_member_location' in c.attributes}
    if fields:im.layouts['a733_pmic']=fields
finite=load('pin_handoff_interrupt',r/'apps/npu-interrupt/host.py').finite
development=a.development_source or r/'apps/npu-finite/development.py'
dev=load('pin_handoff_development',development)
layout=finite.original.image_data(data)[3];assert tuple(layout)==(456,224,232,72,96,128)
boot,pmic,pins=layout[1],layout[4],layout[5];config_offset=boot+pmic;pin_offset=config_offset+24
actual=bytes(v for pin in range(2) for v in ((live['words'][hex(0x07025000)]>>(4*pin))&15,(live['words'][hex(0x07025024)]>>(2*pin))&3,(live['words'][hex(0x07025014)]>>(4*pin))&3))
assert raw[pin_offset:pin_offset+6]!=actual
results=[]
def provider(case,session):
 u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0x02000000,0x07000000);u.mem_map(0x40000000,0x04000000)
 for at,value in im.parts:
  if value:u.mem_write(at,value)
 manager,operations,handoff,pmic_at,rails=0x43000000,0x43020000,0x43021000,0x43022000,0x43023000
 sentinel,callbacks=0x43fff000,0x43ffe000
 L=im.layouts;state={'writes':[],'acquires':0,'time':1,'baseline_model_calls':0,'resume':None,'svc':0};words=dict(live['words'])
 def put(at,value,n=8):u.mem_write(at,int(value).to_bytes(n,'little'))
 def get(at,n=8):return int.from_bytes(u.mem_read(at,n),'little')
 def returned(value=0):u.reg_write(UC_ARM64_REG_X0,value);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
 # Only the pin-provider OS/MMIO callbacks and prior clock admission are modeled.
 def code(cpu,at,size,cookie):
  args=[u.reg_read(UC_ARM64_REG_X0+i) for i in range(3)]
  if at==sentinel:u.emu_stop();return
  if at==im.address('a733_pins_acquire_banked'):state['acquires']+=1
  if at==im.address('baseline',unit='drivers/power/a733_pmic.c'):
   state['baseline_model_calls']+=1;returned(1);return
  instruction=get(at,4);kind=instruction&0xffffffe0;reg=instruction&31
  if instruction&0xffe0001f==0xd4000001:
   state['svc']=(instruction>>5)&0xffff;assert state['svc'] in (0x5c0d,0x5c0e)
   state['resume']=at+4;u.reg_write(UC_ARM64_REG_PC,im.address('npu_finite_svc'));return
  if instruction==0xd69f03e0:
   u.reg_write(UC_ARM64_REG_PC,state['resume']);state['resume']=None;return
  special={0xd5385200:0x15<<26|state['svc'],0xd53800a0:0,0xd53be020:state['time']*24}
  if kind in special:u.reg_write(UC_ARM64_REG_X0+reg,special[kind]);u.reg_write(UC_ARM64_REG_PC,at+4);return
  if at==im.address('clock_live',unit='drivers/power/a733_pmic.c'):returned(1);return
  if not callbacks<=at<callbacks+24:return
  which=(at-callbacks)//4
  if which==0:returned(1)
  elif which==1:returned()
  elif which==2:put(args[1],state['time']);state['time']+=1;returned(1)
  elif which==3:
   assert args[1] in (0x07025000,0x07025014,0x07025024,0x07025210),hex(args[1])
   # PL interrupt enables are modeled clear; mux/pull/drive are captured.
   put(args[2],words.get(hex(args[1]),0),4);returned(1)
  elif which==4:
   address,value=args[1:];mask={0x07025000:0xff,0x07025014:0xff,0x07025024:0xf}[address]
   assert not (words[hex(address)]^value)&~mask,'unowned pin fields changed'
   state['writes'].append({'address':hex(address),'before':hex(words[hex(address)]),'after':hex(value)})
   words[hex(address)]=value;returned(1)
  elif which==5:returned(1)
 u.hook_add(UC_HOOK_CODE,code)
 def call(name,*args):
  for i,value in enumerate(args):u.reg_write(UC_ARM64_REG_X0+i,value)
  u.reg_write(UC_ARM64_REG_SP,0x43ffd000);u.reg_write(UC_ARM64_REG_LR,sentinel)
  u.emu_start(im.address(name),0,count=200000);return u.reg_read(UC_ARM64_REG_X0)&0xffffffff
 for i in range(5):put(operations+8*i,callbacks+4*i)
 u.mem_write(handoff,session[boot+pins:boot+layout[2]])
 assert call('a733_pins_init_banks',manager,operations,0,handoff)==1
 fields=L['a733_pmic'];put(pmic_at+fields['self'],pmic_at)
 u.mem_write(pmic_at+fields['config'],session[config_offset:config_offset+32])
 put(pmic_at+fields['rails'],rails);put(pmic_at+fields['pins'],manager)
 put(pmic_at+fields['stage'],2,4);put(pmic_at+fields['active'],8,4);put(pmic_at+fields['deadline'],1000000)
 ops=pmic_at+fields['pmic_bus_ops'];put(ops,callbacks+8);put(ops+8,callbacks+20);put(ops+16,callbacks);put(ops+24,callbacks+4);put(ops+32,callbacks+4)
 status=call('a733_pmic_poll',pmic_at)
 stages=[get(pmic_at+fields['stage'],4)]
 if status==0:
  status=call('a733_pmic_poll',pmic_at);stages.append(get(pmic_at+fields['stage'],4))
 authority=get(rails+L['a733_rail_leases']['authority'],4)
 assert state['acquires']==1,state
 for address in (0x07025000,0x07025014,0x07025024):assert words[hex(address)]==live['words'][hex(address)],state
 result={'case':case,'pmic_status':status,'pmic_stage':get(pmic_at+fields['stage'],4),'authority':hex(authority),'pin_acquires':state['acquires'],'pin_writes':state['writes'],'stages':stages,'modeled_baseline_successes':state['baseline_model_calls']}
 results.append(result)
 return status,authority
assert provider('captured-stale-session',raw)==(6,0x100)
(a.evidence/'old-session-result.json').write_text(json.dumps(results[-1],indent=2)+'\n')
new,profile,derived,records=dev.session(finite,data,layout,raw,live)
assert new[pin_offset:pin_offset+6]==actual,('development PMIC pin handoff did not adopt captured mux/pull/drive',list(new[pin_offset:pin_offset+6]),list(actual))
assert new[config_offset:pin_offset]==raw[config_offset:pin_offset]
assert new[boot+pins:]==raw[boot+pins:],'typed pin authority changed'
assert provider('live-derived-session',new)==(1,1)
(a.evidence/'results.json').write_text(json.dumps({'passed':True,'image_sha256':hashlib.sha256(data).hexdigest(),'captured_session_sha256':hashlib.sha256(raw).hexdigest(),'development_source_sha256':hashlib.sha256(development.read_bytes()).hexdigest(),'captured_pin_fields':list(actual),'cases':results,'scope':'Real linked PMIC S_PINS poll and banked pin provider; modeled successful baseline, prior clock admission, OS/time callbacks and clear PL IRQ enables; captured RPIO mux/pull/drive with actual guarded write/readback sequence. No hardware access.'},indent=2)+'\n')
print('PASS captured stale PMIC pins refuse; live-derived pins READY with guarded acquisition and unchanged final pin words')
