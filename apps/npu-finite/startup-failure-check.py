#!/usr/bin/env python3
"""Execute linked startup refusals and require precise first-failure RTC records."""
import argparse, hashlib, json, sys
from pathlib import Path
from unicorn import UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_PC, UC_ARM64_REG_LR
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--image', type=Path, required=True)
p.add_argument('--evidence', type=Path, required=True)
a=p.parse_args(); a.evidence.mkdir(parents=True, exist_ok=True)
root=Path(__file__).resolve().parents[2]
# Reuse the sequencer's existing native-adapter fixture without running its
# independent routing cases or creating another machine/layout implementation.
helper=root/'apps/npu-finite/sram-route-check.py'
namespace={'__file__':str(helper),'__name__':'startup_failure_model'}
saved_argv=sys.argv
sys.argv=[str(helper),'--evidence',str(a.image.parent),'--output',str(a.evidence/'unused-routing-results.json')]
prefix=helper.read_text().split('\nresults = []',1)[0]
try:exec(compile(prefix,str(helper),'exec'),namespace)
finally:sys.argv=saved_argv
Machine,COLD,CCU,RESULT=(namespace[n] for n in ('Machine','COLD','CCU','RESULT'))
im=namespace['im']; RTC=0x07090000
cases=(('fabric-word',CCU+0x500,0x83000003),('reset-word',CCU+0xb04,0xa0080001),('pll-tuning',CCU+0x2ac,0xf1000000),('read-callback-status',CCU+0x500,0x80000001),('domain-callback-status',0,0x80000005))
results=[]
for name,address,value in cases:
 words=dict(COLD)
 if address and not name.endswith('callback-status'):words[address]=value
 m=Machine(words);state={'cleanup':0}
 def code(u,at,size,cookie):
  if name=='read-callback-status' and at==im.address('a733_npu_platform_native_read') and u.reg_read(UC_ARM64_REG_X0+1)==address:
   m.returned(value);return
  if name=='domain-callback-status' and at==im.address('a733_power_domain_check'):
   m.returned(value);return
  if at in (im.address('a733_clock_release'),im.address('a733_rail_release'),im.address('a733_gpu_resources_release')):
   state['cleanup']+=1
   assert m.get(RTC+0x110,4)>>24==0xfa,'startup failure was not captured before token cleanup'
 m.u.hook_add(UC_HOOK_CODE,code)
 # Pretend tokens were already held so every refusal executes cleanup.
 m.put(m.s+namespace['L']['a733_npu_startup']['clock'],1)
 assert m.step()==RESULT['A733_NPU_PLATFORM_RETAINED']
 record=[m.get(RTC+at,4) for at in (0x110,0x114,0x118,0x11c)]
 fault,site,caller,actual=record
 assert fault>>24==0xfa and fault>>16&15==0xb,(name,record)
 assert site>>12&15==8 and site&0xfff,(name,record)
 aux=site>>16&0x7fff
 if address:assert aux&0x4000 and (aux&0x3fff)<<2==address&0xffff,(name,record)
 else:assert aux==0x2000|namespace['STAGE']['A733_NPU_STARTUP_PASS'],(name,record)
 assert actual==value,(name,'full 32-bit readback lost',hex(actual),hex(value))
 assert 0x41000000<=caller<0x41200000 and not site>>31,(name,record)
 assert state['cleanup']
 # A later provider/client first-failure attempt cannot replace the precise one.
 m.svc,m.svc_return=0x5c0e,namespace['SENTINEL']
 for i,v in enumerate((0x8fff,0xdeadbeef,0x41000004)):m.u.reg_write(UC_ARM64_REG_X0+i,v)
 m.done=False;m.u.reg_write(UC_ARM64_REG_LR,namespace['SENTINEL'])
 m.u.emu_start(im.address('npu_finite_svc'),0,count=1000)
 assert [m.get(RTC+at,4) for at in (0x110,0x114,0x118,0x11c)]==record
 results.append({'case':name,'passed':True,'register':hex(address),'actual':hex(actual),'site_line':site&0xfff,'caller':hex(caller),'cleanup_callbacks':state['cleanup']})
assert len({c['site_line'] for c in results[:3]})==3,results
(a.evidence/'results.json').write_text(json.dumps({'passed':True,'image_sha256':hashlib.sha256(a.image.read_bytes()).hexdigest(),'cases':results,'scope':'Actual linked sequencer, native read adapter and RTC SVC handler; modeled MMIO, resource callbacks and scheduler. No hardware qualification.'},indent=2)+'\n')
print('PASS linked startup first-failure diagnostics:',len(results),'register and callback cases')
