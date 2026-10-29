#!/usr/bin/env python3
"""Execute the actual patched owner ordering and THS readiness accessor.

Clock driver completion/request and counter APIs are modeled. The actual
initial-state guard executes before the first requested transition.
"""
import argparse,importlib.util,struct
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn.arm64_const import UC_ARM64_REG_PC,UC_ARM64_REG_LR
from harness import Image,save
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('owner_entry_check',ROOT/'checks/os-cpu-return-entry.py')
entry=importlib.util.module_from_spec(spec);spec.loader.exec_module(entry)
prior=entry.prior
class Model(entry.Model):
 def __init__(self,image,layout,owner_fields):
  super().__init__(image,layout);self.requests=[];self.check_ready_fixture=True;self.owner=layout.address('owner','return-cpu.c')
  self.uc.mem_write(self.owner,bytes(self.uc.mem_read(self.c,owner_fields['phase'])))
  self.c=self.owner;self.put32(self.owner+owner_fields['phase'],2)
  self.hooks={layout.address('a733_cpu_clock_burst'):lambda:0,
   layout.address('os_session_admitted'):lambda:0,
   layout.address('os_cpu_return_requested'):lambda:0,
   layout.address('a733_cpu_clock_request'):self.request,
   layout.address('a733_counter_clock_now'):self.now,
   layout.address('a733_clock_token_status'):lambda:0}
 def now(self):self.put64(self.uc.reg_read(prior.fault.REGS[1]),1000100);return 1
 def request(self):self.requests.append((self.uc.reg_read(prior.fault.REGS[1]),self.uc.reg_read(prior.fault.REGS[2])));return 1
 def code(self,uc,a,size,unused):
  if a in self.hooks:
   uc.reg_write(prior.fault.REGS[0],self.hooks[a]());uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_LR));return
  if not self.check_ready_fixture and a==self.layout.address('os_cpu_return_thermal_ready'):
   prior.Model.code(self,uc,a,size,unused);return
  super().code(uc,a,size,unused)
 def step(self):return self.call(0,'os_cpu_clock_step',(self.p,))&0xffffffff

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('elf',type=Path);p.add_argument('--out',type=Path,required=True);a=p.parse_args();a.out.mkdir(parents=True,exist_ok=False)
 names={'a733_cpu_clock','a733_cpu_clock_grant','a733_cpu_clock_config','a733_cpu_clock_report','a733_thermal_policy','os_thermal_service','a733_thermal'}
 layout=Image(a.elf,names);image=prior.fault.Image(a.elf)
 with a.elf.open('rb') as f:
  dwarf=ELFFile(f).get_dwarf_info();members=None
  for cu in dwarf.iter_CUs():
   if not cu.get_top_DIE().attributes['DW_AT_name'].value.decode().endswith('return-cpu.c'):continue
   for d in cu.iter_DIEs():
    if d.tag=='DW_TAG_variable' and d.attributes.get('DW_AT_name') and d.attributes['DW_AT_name'].value==b'owner':
     members={c.attributes['DW_AT_name'].value.decode():int(c.attributes['DW_AT_data_member_location'].value) for c in d.get_DIE_from_attribute('DW_AT_type').iter_children() if c.tag=='DW_TAG_member'}
 assert members and members['driver']==0
 rows=[]
 m=Model(image,layout,members);assert m.step()==1 and m.requests==[(1,2002)]
 m.put32(0x08873000,0);assert m.step()==1 and m.requests==[(1,2002),(0,1794)]
 rows.append({'name':'guard-before-first-request-and-once-only','passed':True})
 m=Model(image,layout,members);m.put32(0x08873000,0);assert m.step()==0xffffffff and m.requests==[]
 rows.append({'name':'bad-initial-DSU-refuses-first-request','passed':True})
 m=Model(image,layout,members);pf=layout.layouts['a733_thermal_policy'];m.put32(m.p+pf['published_seen'],15)
 assert m.step()==1 and not m.requests;m.put32(m.p+pf['published_seen'],31)
 assert m.step()==1 and m.requests==[(1,2002)]
 rows.append({'name':'wait-all-five-before-first-request','passed':True})
 tf=layout.layouts['os_thermal_service'];df=layout.layouts['a733_thermal']
 for name,change,expected in [('ready',lambda m,t:None,1),('uninitialized',lambda m,t:m.put32(t+tf['driver']+df['initialized'],0),0),('faulted',lambda m,t:m.put32(t+tf['driver']+df['fault'],1),0),('retaining',lambda m,t:m.put32(t+tf['retaining'],1),0),('inactive',lambda m,t:m.put32(t+tf['active'],0),0),('failed',lambda m,t:m.put32(t+tf['failed'],1),0)]:
  m=Model(image,layout,members);m.check_ready_fixture=False;t=layout.address('thermal','return-thermal.c')
  m.put32(t+tf['active'],1);m.put32(t+tf['driver']+df['initialized'],1);change(m,t)
  assert m.call(0,'os_cpu_return_thermal_ready')==expected,name
  rows.append({'name':'actual-THS-accessor-'+name,'passed':True})
 save(a.out/'results.json',{'scope':'actual patched owner/guard/readiness with modeled driver API, counter and clock-token status; no hardware','cases':rows})
 print('PASS',len(rows),'owner ordering/readiness cases')
if __name__=='__main__':main()
