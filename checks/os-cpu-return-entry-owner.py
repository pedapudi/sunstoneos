#!/usr/bin/env python3
"""Execute the actual patched owner ordering and THS readiness accessor.

Clock driver completion/request and counter APIs are modeled. The actual
initial-state guard executes before the first requested transition.
The return cases request the return in each owner phase. A modeled
transition completion writes its domain's PLL and mux words; the actual
owner then lowers what it needs and reads the CPU-B words back. CPU-L and the
DSU are compared with their boot words after a modeled ROM reset of both.
"""
import argparse,importlib.util,struct
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn.arm64_const import UC_ARM64_REG_PC,UC_ARM64_REG_LR,UC_ARM64_REG_SP
from harness import Image,save
GATE_RUNS = [['{elf:ret-npu-load}', '--out', '{output}']]
spec_crumb=importlib.util.spec_from_file_location('owner_cpu_crumbs',Path(__file__).resolve().parents[1]/'tools/a733-crumbs.py')
cpu_crumbs=importlib.util.module_from_spec(spec_crumb);spec_crumb.loader.exec_module(cpu_crumbs)
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('owner_entry_check',ROOT/'checks/os-cpu-return-entry.py')
entry=importlib.util.module_from_spec(spec);spec.loader.exec_module(entry)
spec=importlib.util.spec_from_file_location('owner_return_host',ROOT/'apps/os/cpu-return-host.py')
return_host=importlib.util.module_from_spec(spec);spec.loader.exec_module(return_host)
prior=entry.prior
class Model(entry.Model):
 def __init__(self,image,layout,owner_fields):
  super().__init__(image,layout);self.requests=[];self.crumb_trace=[];self.check_ready_fixture=True;self.owner=layout.address('owner','apps/os/cpu.c')
  self.uc.mem_write(self.owner,bytes(self.uc.mem_read(self.c,owner_fields['phase'])))
  self.c=self.owner;self.put32(self.owner+owner_fields['phase'],2)
  self.hooks={layout.address('a733_cpu_clock_burst'):lambda:0,
   layout.address('os_session_admitted'):lambda:0,
   layout.address('os_granted'):lambda:1,   # the modeled session grants the CPU clock (admission is not under test)
   layout.address('os_cpu_return_requested'):lambda:0,
   layout.address('a733_cpu_clock_request'):self.request,
   layout.address('a733_counter_clock_now'):self.now,
   layout.address('a733_clock_token_status'):lambda:0}
 def now(self):self.put64(self.uc.reg_read(prior.fault.REGS[1]),1000100);return 1
 def request(self):self.requests.append((self.uc.reg_read(prior.fault.REGS[1]),self.uc.reg_read(prior.fault.REGS[2])));return 1
 def code(self,uc,a,size,unused):
  if bytes(uc.mem_read(a,4))==bytes.fromhex('a1810bd4'):
   self.crumb_trace.append((uc.reg_read(prior.fault.REGS[0]),uc.reg_read(prior.fault.REGS[1])))
  if a in self.hooks:
   uc.reg_write(prior.fault.REGS[0],self.hooks[a]());uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_LR));return
  if not self.check_ready_fixture and a==self.layout.address('os_cpu_return_thermal_ready'):
   prior.Model.code(self,uc,a,size,unused);return
  super().code(uc,a,size,unused)
 def step(self):return self.call(0,'os_cpu_clock_step',(self.p,))&0xffffffff

# Boot (boot0) and top-point PLL/mux words of CPU-L, CPU-B and the DSU.
BOOT=((0xf8802600,0x03010000),(0xf8802700,0x03000000),(0xf8802600,0x03010105))
TOP={0:(1794,0xf8804500,0x03000000),1:(2002,0xf8804d00,0x03000000),2:(1274,0xf8803100,0x03000105)}
PENDING,COMPLETE,EXPIRED,RETAINED=1,2,6,7
class ReturnModel(Model):
 """A return requested while the owner is in a given phase.

 Each modeled burst takes the next scripted result. A completion applies the
 in-flight transition: its point in applied_mhz and its words in the clock
 registers. Inspection and requests are recorded; the owner must start no
 inspection after the return request."""
 def __init__(self,image,layout,owner_fields,phase,cluster,raised=(),flight=None):
  super().__init__(image,layout,owner_fields)
  self.fields=owner_fields;self.cf=layout.layouts['a733_cpu_clock']
  self.initial_owner_phase=phase
  self.put32(self.owner+owner_fields['phase'],phase);self.put32(self.owner+owner_fields['cluster'],cluster)
  if phase==3:self.put32(self.owner+owner_fields['ready'],1)
  self.put32(self.c+self.cf['applied_mhz'],494);self.put32(self.c+self.cf['applied_mhz']+4,1014)
  for d in raised:self.apply(d,TOP[d])
  self.flight=flight;self.lower_voltage=False;self.script=[];self.inspections=[];self.guards=0;self.bursts=0;self.closing=1
  self.hooks[layout.address('os_cpu_return_requested')]=lambda:self.closing
  self.hooks[layout.address('a733_cpu_clock_burst')]=self.burst
  self.hooks[layout.address('a733_cpu_clock_inspect')]=self.inspect
  self.hooks[layout.address('os_cpu_return_entry_guard')]=self.guard
 def apply(self,d,point):
  mhz,pll,mux=point;self.put32(self.c+self.cf['applied_mhz']+4*d,mhz)
  self.put32(0x08871000+4096*d,pll);self.put32(0x0887101c+4096*d,mux)
  if d<2 and mhz==TOP[d][0]:self.put32(self.c+self.cf['rail_uv']+4*d,(1000000,1050000)[d])
 def burst(self):
  self.bursts+=1
  r=self.script.pop(0) if self.script else 0
  if r==COMPLETE and self.flight is not None:
   self.apply(*self.flight);self.flight=None
   if self.lower_voltage:self.put32(self.c+self.cf['rail_uv']+4,800000);self.lower_voltage=False
  return r
 def inspect(self):self.inspections.append(self.uc.reg_read(prior.fault.REGS[1]));return PENDING
 def guard(self):self.guards+=1;return 1
 def request(self):
  d,mhz=self.uc.reg_read(prior.fault.REGS[1]),self.uc.reg_read(prior.fault.REGS[2])
  self.requests.append((d,mhz))
  self.flight=(d,(mhz,*BOOT[d])) if (d,mhz)==(1,1014) else (d,TOP[d])
  self.lower_voltage=(d,mhz)==(1,1014)
  self.put32(self.c+self.cf['changes'],self.u32(self.c+self.cf['changes'])-1)
  return PENDING
 def done(self):return self.call(0,'os_cpu_return_clock_done')
 def failed(self):return self.u32(self.layout.address('fail_recorded','os-image/main.c'))
 def words(self):return tuple((self.u32(0x08871000+4096*d),self.u32(0x0887101c+4096*d)) for d in range(3))

def returns(image,layout,members):
 """A return in every phase finishes with no raise after the request."""
 rows=[]
 def run(name,m,steps,requests,changes,readbacks=None):
  """steps: (scripted burst results for this step, expected step result)."""
  for script,expected in steps:
   m.script=list(script);assert m.step()==expected,(name,script,expected)
  assert m.requests==requests and not m.inspections and not m.guards,(name,m.requests,m.inspections,m.guards)
  assert m.u32(m.c+m.cf['changes'])==changes,(name,m.u32(m.c+m.cf['changes']))
  ok=all(e!=0xffffffff for _,e in steps)
  assert m.done()==ok and m.failed()==(not ok),(name,m.done(),m.failed())
  if ok:
   assert m.words()[1]==BOOT[1],(name,[hex(w) for w in m.words()[1]])
   # The ROM return resets CPU-L and the DSU; CPU-B keeps what the image left.
   for d in (0,2):m.put32(0x08871000+4096*d,BOOT[d][0]);m.put32(0x0887101c+4096*d,BOOT[d][1])
   assert m.words()==BOOT,name
   if readbacks is not None:
    assert 'os_cpu_return_rail_uv' in layout.symbols,'return must publish confirmed CPU rail readbacks'
    address=layout.address('os_cpu_return_rail_uv')
    raw=bytes(m.uc.mem_read(address,8));actual=struct.unpack('<2I',raw)
    assert actual==readbacks,(name,actual,readbacks)
    assert return_host.geometry(image.path)['cpu_rails']=={'address':address,'bytes':8}
    decoded=return_host.decode_rails(raw)
    assert (decoded['dcdc5_uv'],decoded['dcdc3_uv'])==tuple(v or None for v in readbacks),(name,decoded)
   # The host reads the owner's publication from image RAM after the return (cpu_clock in the return geometry).
   clock=return_host.geometry(image.path)['cpu_clock']
   assert {n:p['address'] for n,p in clock.items()}=={n:layout.address(n,'apps/os/cpu.c') for n in clock},(name,clock)
   raw={n:bytes(m.uc.mem_read(p['address'],p['bytes'])) for n,p in clock.items()}
   published=return_host.decode_clock(raw['record'],raw['dsu_mhz'],raw['pattern_bits'],raw['parked_max_us'])
   if requests:
    # The close lowered CPU-B: it republishes, so the record names 1014 MHz at the rail's confirmed readback and counts
    # the lowering, as the rails read at close do.
    rail=m.u32(m.c+m.cf['rail_uv']+4)//1000
    assert published['published'] and (published['cpu_b_mhz'],published['cpu_b_mv'])==(1014,rail),(name,published)
    assert published['transitions']==1,(name,published)
   else:
    # These cases enter the owner in a later phase without its publication and change nothing at close: the record
    # reads as never published.
    assert published=={'published':False,'source':'CPU clock owner publication at return'},(name,published)
  trace=[value for stage,value in m.crumb_trace if stage==0x0204]
  assert trace and trace[0]>>12==1,(name,'missing drain breadcrumb',trace)
  assert all((value>>10&3)==m.initial_owner_phase for value in trace),(name,trace)
  assert all(a!=b for a,b in zip(trace,trace[1:])),(name,'repeated unchanged phase',trace)
  if requests:assert any(value>>12==2 for value in trace),(name,'missing lowering request',trace)
  if ok:assert [value>>12 for value in trace[-2:]]==[4,5],(name,'missing verification/completion',trace)
  else:assert all(value>>12!=5 for value in trace),(name,'failed return reported done',trace)
  rows.append({'name':name,'passed':True,'requests':m.requests,'returned':ok,'clock_return_breadcrumbs':trace})
 F=0xffffffff
 # Before the first owner pass: no driver initialization, inspection or poll.
 # Successful setup hooks let the old START path reach its unwanted inspection.
 m=ReturnModel(image,layout,members,0,0)
 m.uc.mem_write(m.c,bytes(members['phase']));setup=[]
 m.hooks[layout.address('os_session_admitted')]=lambda:0x41ed0000
 m.hooks[layout.address('a733_cpu_clock_grant_load')]=lambda:setup.append('grant') or 1
 m.hooks[layout.address('a733_cpu_clock_init')]=lambda:setup.append('init') or 1
 run('return-in-START',m,[((0,),1)],[],0,(0,0))
 assert not m.bursts and not setup,(m.bursts,setup)
 # CPU-L's inspection in flight: it finishes; CPU-B is never inspected.
 run('return-in-INSPECT',ReturnModel(image,layout,members,1,0),[((PENDING,),1),((COMPLETE,),1)],[],4)
 # Inspected, before the entry guard and any request: no guard, no request.
 run('return-in-REQUEST-before-any-request',ReturnModel(image,layout,members,2,0),[((0,),1)],[],4)
 # CPU-B's raise to 2002 MHz in flight: it completes, then CPU-B is lowered.
 run('return-in-REQUEST-with-request-in-flight',
     ReturnModel(image,layout,members,2,1,flight=(1,TOP[1])),
     [((PENDING,),1),((COMPLETE,),1),((PENDING,),1),((COMPLETE,),1)],[(1,1014)],3)
 # CPU-L's raise in flight after CPU-B's: CPU-L is left to the ROM return.
 run('return-in-REQUEST-with-CPU-L-in-flight',
     ReturnModel(image,layout,members,2,2,raised=(1,),flight=(0,TOP[0])),
     [((COMPLETE,),1),((COMPLETE,),1)],[(1,1014)],3)
 # Every domain at its top point: CPU-B lowered, as before.
 run('return-in-FOLLOW',ReturnModel(image,layout,members,3,3,raised=(0,1,2)),
     [((0,),1),((PENDING,),1),((COMPLETE,),1)],[(1,1014)],3,(1000000,800000))
 # Thermal lowering left CPU-B at 1014 MHz: nothing to lower, no change spent.
 m=ReturnModel(image,layout,members,3,3,raised=(0,2));m.put32(m.c+m.cf['changes'],0)
 run('return-in-FOLLOW-with-nothing-to-lower',m,[((0,),1)],[],0)
 # A lowering that cannot finish before its deadline still ends the session.
 run('return-lowering-expired',ReturnModel(image,layout,members,3,3,raised=(0,1,2)),
     [((0,),1),((EXPIRED,),F)],[(1,1014)],3)
 # A raise can confirm 1050 mV, then expire before changing CPU-B's PLL.
 # The applied point stays at 1014 MHz; the external helper restores the rail.
 m=ReturnModel(image,layout,members,2,1)
 m.put32(m.c+m.cf['rail_uv']+4,1050000);m.put32(m.c+m.cf['changes'],3)
 run('return-reports-expired-raise-voltage',m,[((EXPIRED,),1)],[],3,(800000,1050000))
 # A thermal lowering in flight returns at 1014 MHz and keeps its voltage.
 m=ReturnModel(image,layout,members,3,3,raised=(0,1,2),flight=(1,(1014,*BOOT[1])))
 m.put32(m.c+m.cf['changes'],0)
 run('return-reports-thermal-lowering-voltage',m,[((PENDING,),1),((COMPLETE,),1)],[],0,(1000000,1050000))
 return rows

def retained_diagnostics(image,layout,members):
 """Actual driver setters and the real SVC recorder; only failing APIs are modeled."""
 rows=[];defines,_=cpu_crumbs.header()
 # State numbers are private to the frozen driver; guard their mapping below.
 states=('IDLE','CLOCK','RAIL','INSPECT','READ','VOLTAGE','WRITTEN','SETTLE',
         'PARK','UPDATE','LOCK','UNPARK','RELEASE')
 driver=(ROOT/'drivers/clock/a733_cpu_clock.c').read_text()
 enum=driver.split('enum { S_IDLE,',1)[1].split('};',1)[0]
 assert ('S_IDLE,'+enum).replace('\n','').replace(' ','').split(',')==['S_'+x for x in states]
 cases=[('counter-failed-idle',0,2,0,0,None,0xffff),
        ('clock-acquire-fault',1,1,0,1,('a733_clock_acquire',4),4),
        ('rail-acquire-retained',2,1,0,1,('a733_rail_poll',5),5),
        ('inspection-PMIC-invalid',3,1,0,1,('a733_pmic_submit',5),5),
        ('PMIC-result-retained',4,1,0,1,('a733_pmic_result',6),6),
        ('thermal-lowering-PLL-mismatch',9,0,1,1,None,0xffff),
        ('restore-lock-expired',10,0,2,0,None,0xffff),
        ('return-lowering-release-timeout',12,1,0,1,None,0xffff)]
 for name,state,domain,origin,closing,hook,status in cases:
  m=ReturnModel(image,layout,members,3,3,raised=(0,1,2));m.closing=closing
  c=m.cf
  m.put64(m.c+c['self'],m.c)
  m.put64(m.c+c['ops'],layout.address('clock_now','apps/os/cpu.c'))
  m.put64(m.c+c['ops']+8,layout.address('clock_read','apps/os/cpu.c'))
  m.put64(m.c+c['cookie'],m.owner)
  m.put32(m.c+c['state'],state);m.put32(m.c+c['cluster'],domain)
  m.put32(m.c+c['thermal'],origin);m.put32(m.c+c['target_mhz'],1014)
  m.put32(m.c+c['result'],2 if state else 0)
  m.put64(m.c+c['deadline'],2000000)
  # Real tokens remain live. Retain callbacks poison the modeled managers
  # after the observer, so a late snapshot would report false causes.
  for field,value in (('clock',7),('rail',9)):
   m.put64(m.c+c[field],value)
  shared=layout.address('providers')+layout.layouts['board_provider']['shared']
  nf=layout.layouts['a733_native_usb']
  clock_failed=shared+nf['clocks']+layout.layouts['a733_clock_leases']['failed']
  authority=shared+nf['rails']+layout.layouts['a733_rail_leases']['authority']
  counter_failed=shared+nf['counter']+layout.layouts['a733_counter_clock']['failed']
  def retain_clock():m.put32(clock_failed,1);return 4
  def retain_rail():m.put32(authority,1<<10);return 5
  m.hooks[layout.address('a733_clock_retain')]=retain_clock
  m.hooks[layout.address('a733_rail_retain')]=retain_rail
  if state==0:m.put32(counter_failed,1)
  m.put64(m.c+c['timer'],1000000) # already expired for RELEASE and LOCK
  if state==0:m.hooks[layout.address('a733_counter_clock_now')]=lambda:0
  if state==3:m.put32(m.c+c['inspect_only'],1)
  if hook:m.hooks[layout.address(hook[0])]=lambda value=hook[1]:value
  # Read actual PLL N=69 for CPU-L: requested N=0 makes S_UPDATE fail.
  # LOCK with the whole request expired distinguishes expiry from a lock read.
  if state==10:m.put64(m.c+c['deadline'],1000000)
  assert m.call(0,'a733_cpu_clock_poll',(m.c,))==RETAINED,name
  words={k:m.u32(0x07090000+off) for k,off in cpu_crumbs.WORDS.items()}
  failure=cpu_crumbs.board_failure(words,defines,image.path)
  if state==9:
   assert failure['group']=='cpu_pll' and failure['expected_n']==0 and failure['observed_n']==69,(name,failure)
   assert failure['domain']==domain and failure['driver_state']==state and failure['closing']==bool(closing),(name,failure)
   rows.append({'name':name,'passed':True,'failure':failure});continue
  assert failure['group']=='cpu_retained',(name,failure)
  assert (failure['driver_state'],failure['domain'],failure['origin'])==(state,domain,origin),(name,failure)
  assert (failure['phase'],failure['request_cursor'],failure['target_mhz'])==(3,3,1014),(name,failure)
  assert failure['prior_status']==(0 if state==0 else 2),(name,failure)
  assert failure['provider_status']==(None if status==0xffff else status),(name,failure)
  assert failure['closing']==bool(closing) and failure['inspection']==(state==3),(name,failure)
  assert not failure['clocks_failed'] and not failure['ledger_failed'],(name,failure)
  assert 'a733_cpu_clock.c:' in failure['caller_source'],(name,failure)
  assert m.u32(m.c+c['failed'])==1,name
  assert failure['clock_held'] and failure['rail_held'],(name,failure)
  assert m.u32(clock_failed)==1 and m.u32(authority)==1<<10,name
  assert failure['counter_failed']==(state==0),(name,failure)
  assert not failure['pmic_failed'],(name,failure)
  assert not [w for w in m.writes if 0x08871000<=w[1]<0x08874000],name
  # Repeated RETAINED and the later owner refusal cannot replace the setter.
  original={k:words[k] for k in ('fault','esr','elr','far')}
  assert m.call(0,'a733_cpu_clock_poll',(m.c,))==RETAINED,name
  m.script=[RETAINED];assert m.step()==0xffffffff,name
  assert {k:m.u32(0x07090000+cpu_crumbs.WORDS[k]) for k in original}==original,name
  rows.append({'name':name,'passed':True,'failure':failure})
 return rows

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('elf',type=Path);p.add_argument('--out',type=Path,required=True);a=p.parse_args();a.out.mkdir(parents=True,exist_ok=False)
 names={'a733_cpu_clock','a733_cpu_clock_grant','a733_cpu_clock_config','a733_cpu_clock_report','a733_thermal_policy','os_thermal_service','a733_thermal','board_provider','a733_native_usb','a733_counter_clock','a733_clock_leases','a733_rail_leases'}
 layout=Image(a.elf,names);image=prior.fault.Image(a.elf)
 with a.elf.open('rb') as f:
  dwarf=ELFFile(f).get_dwarf_info();members=None
  for cu in dwarf.iter_CUs():
   if not cu.get_top_DIE().attributes['DW_AT_name'].value.decode().endswith('apps/os/cpu.c'):continue
   for d in cu.iter_DIEs():
    if d.tag=='DW_TAG_variable' and d.attributes.get('DW_AT_name') and d.attributes['DW_AT_name'].value==b'owner':
     members={c.attributes['DW_AT_name'].value.decode():int(c.attributes['DW_AT_data_member_location'].value) for c in d.get_DIE_from_attribute('DW_AT_type').iter_children() if c.tag=='DW_TAG_member'}
 assert members and members['driver']==0
 rows=[]
 # The publication's packing (pack() in apps/os/cpu.c): 11-bit points and voltages, then a 20-bit transition count.
 word=1794|2002<<11|1000<<22|1050<<33|3<<44
 assert return_host.decode_clock(struct.pack('<Q',word),struct.pack('<I',1274),struct.pack('<I',2),struct.pack('<Q',31))=={
  'published':True,'cpu_l_mhz':1794,'cpu_b_mhz':2002,'cpu_l_mv':1000,'cpu_b_mv':1050,'transitions':3,'dsu_mhz':1274,
  'pattern_bits':2,'parked_max_us':31,'source':'CPU clock owner publication at return'}
 rows.append({'name':'clock-publication-readout','passed':True})
 m=Model(image,layout,members);assert m.step()==1 and m.requests==[(1,2002)]
 m.put32(0x08873000,0);assert m.step()==1 and m.requests==[(1,2002),(0,1794)]
 rows.append({'name':'guard-before-first-request-and-once-only','passed':True})
 m=Model(image,layout,members);m.put32(0x08873000,0);assert m.step()==0xffffffff and m.requests==[]
 rows.append({'name':'bad-initial-DSU-refuses-first-request','passed':True})
 m=Model(image,layout,members);pf=layout.layouts['a733_thermal_policy'];m.put32(m.p+pf['published_seen'],15)
 assert m.step()==1 and not m.requests;m.put32(m.p+pf['published_seen'],31)
 assert m.step()==1 and m.requests==[(1,2002)]
 rows.append({'name':'wait-all-five-before-first-request','passed':True})
 # The formatter must publish the applied DSU point, not its boot snapshot.
 m=Model(image,layout,members);cf=layout.layouts['a733_cpu_clock'];reported=[]
 assert m.call(0,'os_cpu_admit')==1
 def clock_line():
  reported.append(m.u32(m.uc.reg_read(UC_ARM64_REG_SP)))
  return 1
 m.hooks[layout.address('snprintf')]=clock_line
 def formatted_dsu():
  assert m.call(0,'os_cpu_clock_line',(0x41ef0000,512))==1
  return reported[-1]
 m.hooks[layout.address('a733_cpu_clock_burst')]=lambda:2
 # A completion with no applied DSU point keeps the decoded boot value.
 assert m.step()==1 and formatted_dsu()==494
 m.put32(m.owner+members['phase'],2);m.put32(m.owner+members['cluster'],3)
 m.put32(m.owner+members['ready'],0);m.put32(m.owner+members['count'],2)
 m.hooks[layout.address('a733_cpu_clock_burst')]=lambda:1
 assert m.step()==1 and m.call(0,'os_cpu_clock_ready')==0 and formatted_dsu()==494
 m.put32(m.c+cf['applied_mhz']+8,1274)
 m.hooks[layout.address('a733_cpu_clock_burst')]=lambda:2
 assert m.step()==1 and m.call(0,'os_cpu_clock_ready')==1 and formatted_dsu()==1274
 assert m.u32(m.owner+members['count'])==3
 rows.append({'name':'formatter-publishes-completed-DSU-point','passed':True})
 tf=layout.layouts['os_thermal_service'];df=layout.layouts['a733_thermal']
 for name,change,expected in [('ready',lambda m,t:None,1),('uninitialized',lambda m,t:m.put32(t+tf['driver']+df['initialized'],0),0),('faulted',lambda m,t:m.put32(t+tf['driver']+df['fault'],1),0),('retaining',lambda m,t:m.put32(t+tf['retaining'],1),0),('inactive',lambda m,t:m.put32(t+tf['active'],0),0),('failed',lambda m,t:m.put32(t+tf['failed'],1),0)]:
  m=Model(image,layout,members);m.check_ready_fixture=False;t=layout.address('thermal','apps/os/thermal.c')
  m.put32(t+tf['active'],1);m.put32(t+tf['driver']+df['initialized'],1);change(m,t)
  assert m.call(0,'os_cpu_return_thermal_ready')==expected,name
  rows.append({'name':'actual-THS-accessor-'+name,'passed':True})
 rows+=returns(image,layout,members)
 rows+=retained_diagnostics(image,layout,members)
 save(a.out/'results.json',{'scope':'actual patched owner/guard/readiness with modeled driver API, counter and clock-token status; no hardware','cases':rows})
 print('PASS',len(rows),'owner ordering/readiness/return cases')
if __name__=='__main__':main()
