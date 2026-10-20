#!/usr/bin/env python3
"""Focused actual-image SD0 task/controller/native trace with modeled board services."""
import argparse,hashlib,importlib.util,json,struct
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR,UC_ARM64_REG_PSTATE
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
class Image:
 def __init__(self,path,layout_names=None):
  # Select only layouts consumed by a fixture. Unrelated translation units
  # may legally declare different private structures with the same name.
  with path.open('rb') as f:
   e=ELFFile(f);self.parts=[(int(p['p_vaddr']),p.data()) for p in e.iter_segments() if p['p_type']=='PT_LOAD'];self.symbols={};self.functions={};self.layouts={}
   for s in e.get_section_by_name('.symtab').iter_symbols():self.symbols.setdefault(s.name,set()).add(int(s['st_value']))
   for cu in e.get_dwarf_info().iter_CUs():
    unit=cu.get_top_DIE().attributes['DW_AT_name'].value.decode()
    for d in cu.iter_DIEs():
     if 'DW_AT_name' not in d.attributes:continue
     n=d.attributes['DW_AT_name'].value.decode()
     if d.tag=='DW_TAG_subprogram' and 'DW_AT_low_pc' in d.attributes:self.functions.setdefault((unit,n),set()).add(int(d.attributes['DW_AT_low_pc'].value))
     if d.tag=='DW_TAG_structure_type' and (layout_names is None or n in layout_names):
      fields={c.attributes['DW_AT_name'].value.decode():int(c.attributes['DW_AT_data_member_location'].value) for c in d.iter_children() if c.tag=='DW_TAG_member' and 'DW_AT_name' in c.attributes and 'DW_AT_data_member_location' in c.attributes}
      if fields:
       if n in self.layouts:assert self.layouts[n]==fields,n
       self.layouts[n]=fields
 def address(self,n,unit=None):
  values=self.symbols.get(n,set())-{0}
  if unit:values=set().union(*(v for (u,name),v in self.functions.items() if name==n and u.endswith(unit)))
  assert len(values)==1,(n,unit,values);return next(iter(values))
class Machine:
 def __init__(self,image,session,variant):
  self.image=image;self.session=session;self.variant=variant;self.cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);self.cpu.reg_write(UC_ARM64_REG_PSTATE,5)
  for a,n in [(0x02000000,0x06000000),(0x40000000,0x04000000),(0x50000000,0x01000000)]:self.cpu.mem_map(a,n)
  for a,data in image.parts:self.cpu.mem_write(a,data)
  # This fixture bypasses scheduler entry. The SRE port starts with9999 to
  # prohibit early task-context I/O, then restores initial task nesting0.
  # Model that actual normal-task state only for the single-core port symbols.
  if 'ullCriticalNesting' in image.symbols:
   self.cpu.mem_write(image.address('ullCriticalNesting'),bytes(8))
   self.cpu.mem_write(image.address('ullPortInterruptNesting'),bytes(8))
  self.base=image.address('storage_read');self.layout=image.layouts['storage_read_application'];self.platform=self.base+self.layout['platform'];self.p=image.layouts['a733_mmc_platform'];self.core=self.platform+self.p['core'];self.c=image.layouts['sunxi_mmc'];self.native=self.base+self.layout['native'];self.n=image.layouts['a733_mmc_platform_native'];self.board=image.address('board');self.shared=image.address('providers')+image.layouts['board_provider']['shared'];self.usb=image.layouts['a733_native_usb']
  self.time=1000000;self.held=False;self.appheld=False;self.finished=False;self.retained=False;self.trace=[];self.commands=[];self.fifo=0;self.total=0;self.data_command=0;self.state=0;self.ocr=0;self.tx=[];self.request=None;self.max_tx=768
  self.cpu.mem_write(self.base,session);self.cpu.mem_write(self.board,session[:592]);self.write(self.board+160,61000000,8);self.write(self.base+self.layout['handle'],1,8);self.write(self.base+self.layout['mutex'],1,8)
  self.cpu.mem_write(0x41f00000,session)
  locks=self.shared+self.usb['locks'];ll=image.layouts['a733_platform_locks'];clock=self.shared+self.usb['clocks'];pins=self.shared+self.usb['pins'];rails=self.shared+self.usb['rails'];pmic=self.shared+self.usb['pmic'];gpu=self.shared+self.usb['gpu']
  self.locks=locks;self.clock=clock;self.rails=rails;self.pmic=pmic
  self.write(locks+ll['initialized'],1);self.write(locks+ll['mutex'],1,8);self.write(locks+ll['now'],image.address('a733_counter_clock_now'),8);self.write(locks+ll['context'],self.shared+self.usb['counter'],8)
  cl=image.layouts['a733_clock_leases'];self.write(clock+cl['initialized'],1);self.write(clock+cl['next_id'],1,8);self.write(clock+cl['now'],image.address('a733_counter_clock_now'),8)
  self.write(pins+image.layouts['a733_pins']['initialized'],1)
  rl=image.layouts['a733_rail_leases'];rb=image.layouts['a733_rail_binding'];self.write(rails+rl['self'],rails,8);self.write(rails+rl['binding']+rb['clocks'],clock,8);self.write(rails+rl['binding']+rb['gpu'],gpu,8)
  pl=image.layouts['a733_pmic'];po=image.layouts['a733_pmic_ops'];self.write(pmic+pl['clocks'],clock,8);self.write(pmic+pl['pins'],pins,8);self.write(pmic+pl['rails'],rails,8);self.write(pmic+pl['cookie'],self.native,8);self.write(pmic+pl['ops']+po['outside'],image.address('outside','/drivers/storage/a733_mmc_platform_native.c'),8)
  self.regs={0x07090160:(1<<28)|2,0x02002000:0xf8000000|(99<<8)|(99<<16),0x02002580:1<<31,0x02002588:1<<31,0x020025c0:1<<24,0x020025e0:1<<30,0x0200259c:0x30001,0x02000040:2<<10,0x02000048:2<<10,0x02000070:1,
   0x04020300:0x03050530,0x0402005c:0x81710331,0x0402010c:0x80001234,0x04020054:0x12345676,0x04020058:0xffff,0x04020104:7,0x04020140:0xa5a40000}
  csd=(1<<126)|(9<<80)|(5<<84)|(0x7fff<<48);self.csd=[(csd>>(96-32*i))&0xffffffff for i in range(4)];self.cid=[0x12345678+i for i in range(4)]
  names=['board_live','a733_smp_counter','a733_counter_clock_now','aarch64_device_translate','xTaskGetCurrentTaskHandle','xTaskGetSchedulerState','xQueueGetMutexHolder','xQueueSemaphoreTake','xQueueGenericSend','vTaskDelay','a733_platform_lock_enter','a733_platform_lock_leave','a733_platform_lock_retain','a733_rail_acquire','a733_rail_poll','a733_rail_status','a733_rail_retain','a733_pmic_identity','a733_pmic_live','a733_pmic_submit','a733_pmic_result','a733_pmic_reap','a733_pins_acquire_banked','a733_pins_held_banked','a733_pins_read_inputs_banked','a733_native_usb_step','a733_native_usb_health','tusb_rhport_init','tud_task_ext','tud_vendor_n_available','tud_vendor_n_read','tud_vendor_n_write','tud_vendor_n_write_flush','a733_cpu_coherence_publish']
  self.hooks={image.address(n):n for n in names}
  self.cpu.hook_add(UC_HOOK_CODE,self.code);self.cpu.hook_add(UC_HOOK_MEM_READ,self.read_mmio,begin=0x02000000,end=0x07ffffff);self.cpu.hook_add(UC_HOOK_MEM_WRITE,self.write_mmio,begin=0x02000000,end=0x07ffffff)
 def write(self,a,v,n=4):self.cpu.mem_write(a,int(v).to_bytes(n,'little'))
 def read(self,a,n=4):return int.from_bytes(self.cpu.mem_read(a,n),'little')
 def field(self,n):return self.read(self.base+self.layout[n])
 def ret(self,v=0):self.cpu.reg_write(UC_ARM64_REG_X0,v&((1<<64)-1));self.cpu.reg_write(UC_ARM64_REG_PC,self.cpu.reg_read(UC_ARM64_REG_LR))
 def code(self,cpu,a,size,unused):
  if a==0x50fff000:self.finished=True;cpu.emu_stop();return
  name=self.hooks.get(a)
  if not name:return
  args=[cpu.reg_read(UC_ARM64_REG_X0+i) for i in range(8)]
  if name=='board_live':self.ret(1)
  elif name=='a733_smp_counter':self.ret(self.time*24)
  elif name=='a733_counter_clock_now':self.write(args[1],self.time,8);self.ret(1)
  elif name=='aarch64_device_translate':self.cpu.mem_write(args[2],struct.pack('<Q2I',args[0],0xff if args[0]>=0x40000000 else 0,3 if args[0]>=0x40000000 else 2));self.ret(1)
  elif name=='xTaskGetCurrentTaskHandle':self.ret(1)
  elif name=='xTaskGetSchedulerState':self.ret(2)
  elif name=='xQueueGetMutexHolder':self.ret(1 if self.held else 0)
  elif name=='xQueueSemaphoreTake':assert not self.appheld;self.appheld=True;self.ret(1)
  elif name=='xQueueGenericSend':assert self.appheld;self.appheld=False;self.ret(1)
  elif name=='vTaskDelay':assert not self.appheld;self.finished=True;cpu.emu_stop()
  elif name=='a733_platform_lock_enter':assert not self.held and not self.retained;self.held=True;self.ret(1)
  elif name=='a733_platform_lock_leave':assert self.held;self.held=False;self.ret()
  elif name=='a733_platform_lock_retain':assert self.held;self.held=False;self.retained=True;self.ret(1)
  elif name=='a733_rail_acquire':self.cpu.mem_write(args[5],struct.pack('<4Q',1,args[1],args[2],args[3]));self.ret(1)
  elif name in ('a733_rail_poll','a733_rail_status'):self.ret(0)
  elif name=='a733_rail_retain':self.ret(5)
  elif name=='a733_pmic_identity':self.cpu.mem_write(args[1],struct.pack('<3Q',self.pmic,10,1));self.ret(1)
  elif name=='a733_pmic_live':self.ret(1)
  elif name=='a733_pmic_submit':
   self.pending_rail=args[3];self.write(args[5]+24,1,8);self.ret(0)
  elif name=='a733_pmic_result':
   p=args[2];pl=self.image.layouts['a733_pmic_snapshot'];self.cpu.mem_write(p,bytes(24))
   for f,v in [('rail',self.pending_rail),('chip',3),('enabled',1),('decoded',1),('programmed_uv',1800000 if self.pending_rail==24 else 3300000)]:self.write(p+pl[f],v,1 if f=='chip' else 4)
   self.ret(2)
  elif name=='a733_pmic_reap':self.ret(2)
  elif name=='a733_pins_acquire_banked':self.cpu.mem_write(args[4],struct.pack('<2Q',1,args[1])+bytes(52));self.ret(0)
  elif name=='a733_pins_held_banked':self.write(args[2],1);self.ret(0)
  elif name=='a733_pins_read_inputs_banked':self.write(args[4],0);self.ret(0)
  elif name in ('a733_native_usb_step','a733_native_usb_health','tusb_rhport_init','tud_task_ext','a733_cpu_coherence_publish'):self.ret(1)
  elif name=='tud_vendor_n_available':self.ret(1 if self.request is not None else 0)
  elif name=='tud_vendor_n_read':self.cpu.mem_write(args[1],bytes([self.request]));self.request=None;self.ret(1)
  elif name=='tud_vendor_n_write':n=min(args[2],self.max_tx);self.tx.append(bytes(cpu.mem_read(args[1],n)));self.ret(n)
  elif name=='tud_vendor_n_write_flush':self.ret(0)
  else:raise AssertionError(name)
 def read_mmio(self,cpu,access,a,size,value,unused):
  assert self.held,(hex(a),'MMIO outside native common lock');assert size==4
  v=self.regs.get(a,0)
  if a==0x04020038:
   if self.data_command and self.fifo==self.total:v|=8
   if self.variant=='crc-fault' and self.fifo>=64:v|=1<<7
  elif a==0x0402003c:v=4 if self.fifo==self.total else 0
  elif a==0x04020200:
   assert self.fifo<self.total;v=sum(((self.fifo+i)*37+11)%256<<(8*i) for i in range(4));self.fifo+=4
  self.trace.append(['read',a,v]);self.write(a,v)
 def write_mmio(self,cpu,access,a,size,v,unused):
  assert self.held and size==4,(hex(a),self.held);self.trace.append(['write',a,v]);assert a!=0x04020200
  self.regs[a]=v
  if a==0x04020038:self.regs[a]=self.read(a)&~v
  if a==0x04020000:self.regs[a]=v&~7
  if a==0x03400198:self.regs[0x03400118]=self.regs.get(0x03400118,0)&~v
  if a!=0x04020018:return
  assert not v&(1<<10),'Media write direction reached actual native MMIO'
  self.regs[a]=v&~(1<<31)
  if v&(1<<21):self.regs[a]=0;return
  n=v&63;arg=self.regs.get(0x0402001c,0);self.commands.append([n,arg]);assert n in (0,8,55,41,2,3,9,7,13,17)
  self.regs[0x04020038]=4;self.regs[0x04020020]=(self.state<<9)|0x100;self.data_command=self.total=self.fifo=0
  if n==0:self.state=0
  elif n==8:assert arg==0x1aa;self.regs[0x04020020]=0x1aa
  elif n==55:self.regs[0x04020020]|=1<<5
  elif n==41:
   self.ocr+=1;self.regs[0x04020020]=0x40300000|(0x80000000 if self.ocr>=2 else 0)
   if self.ocr>=2:self.state=1
  elif n in (2,9):
   for i,word in enumerate(self.cid if n==2 else self.csd):self.regs[0x0402002c-4*i]=word
   if n==2:self.state=2
  elif n==3:self.state=3;self.regs[0x04020020]=0x12340700
  elif n==7:self.state=4
  elif n==13:self.regs[0x04020020]=0x900
  elif n==17:
   assert arg==0 and self.regs[0x04020014]==512;self.total=512;self.data_command=17;self.regs[0x04020020]=0x900
 def call(self,name,*args):
  self.finished=False;self.cpu.reg_write(UC_ARM64_REG_SP,0x50ffeff0);self.cpu.reg_write(UC_ARM64_REG_LR,0x50fff000)
  for i,v in enumerate(args):self.cpu.reg_write(UC_ARM64_REG_X0+i,v)
  self.cpu.emu_start(self.image.address(name),0,count=3000000)
  assert self.finished,(name,hex(self.cpu.reg_read(UC_ARM64_REG_PC)),self.field('stage'),self.field('reason'))
  return self.cpu.reg_read(UC_ARM64_REG_X0)
 def frame(self):
  assert self.call('storage_read_copy',0x50010000,0x50011000)==1
  return bytes(self.cpu.mem_read(0x50010000,self.read(0x50011000)))
def run(image,session,out,only=None):
 out.mkdir(parents=True,exist_ok=False);raw=session.read_bytes();prep=module('sd0_native_prep',ROOT/'apps/storage-read/prepare.py');prep.embedded_reference(image.read_bytes(),raw)
 e=Image(image);required=['storage_read_task','storage_read_copy','storage_read_session_valid','storage_read_session_prepare','a733_mmc_platform_retain','a733_mmc_platform_init','sunxi_mmc_poll','board_usb0_task']
 for n in required:e.address(n)
 # Freeze every consumed field/hook mapping before behavioral execution.
 (out/'preflight.json').write_text(json.dumps(dict(image_sha256=hashlib.sha256(image.read_bytes()).hexdigest(),session_sha256=hashlib.sha256(raw).hexdigest(),symbols={n:e.address(n) for n in required},layouts=e.layouts,physical_access=False),indent=2)+'\n')
 decoder=module('sd0_native_decode',ROOT/'apps/storage-read/observe.py');results={}
 for variant in ((only,) if only else ('nominal','cancel-after-take','deadline-after-take','crc-fault')):
  m=Machine(e,raw,variant);assert m.call('storage_read_session_valid',m.base)==1
  for i in range(1200):
   if m.field('stage')==5 and variant in ('cancel-after-take','deadline-after-take'):
    if variant=='cancel-after-take':m.call('storage_read_revoke',1)
    else:m.time=m.read(m.base+m.layout['deadline'],8);assert m.time<61000000
   m.call('storage_read_task',0);m.time+=1000
   if m.field('result'):break
  assert m.field('result')==(1 if variant=='nominal' else 2),(variant,m.field('stage'),m.field('reason'),m.commands)
  frame=m.frame();decoded=decoder.decode(frame,raw);assert decoded['status']==m.field('result')
  if variant=='nominal':
   assert frame[256:]==bytes((i*37+11)%256 for i in range(512))
   assert [c[0] for c in m.commands]==[0,8,55,41,55,41,2,3,9,7,13,17,13],m.commands
   assert decoded['cid']==m.cid and decoded['csd']==m.csd and decoded['sectors']==33554432 and decoded['ocr']==0xc0300000
   (out/'native-frame.bin').write_bytes(frame)
   # Real USB staging/cancel/TX callback: queue64, cancel, complete old64.
   m.max_tx=64;m.request=0;m.call('board_usb0_task',0);assert len(m.tx)==1 and len(m.tx[0])==64
   m.request=1;m.call('board_usb0_task',0);assert len(m.tx)==1
   m.call('tud_vendor_tx_cb',0,64);assert m.field('cancel')==1
   m.request=0;m.max_tx=768;m.call('board_usb0_task',0);assert len(m.tx)==2 and len(m.tx[-1])==256 and m.tx[-1][8]==2
  else:
   assert len(frame)==256 and not any(frame[32:64]);assert m.read(m.platform+m.p['fault'])==1
   assert not any(m.cpu.mem_read(m.base+m.layout['copied'],512))
   if variant=='crc-fault':assert m.read(m.core+m.c['borrowed'],8)==m.base+m.layout['borrowed'] and m.retained
   else:assert m.read(m.core+m.c['borrowed'],8)==0
  results[variant]=dict(passed=True,commands=m.commands,result=decoded,mmio_accesses=len(m.trace),retained_register_mutex=m.retained)
  (out/(variant+'-trace.json')).write_text(json.dumps(m.trace,indent=2)+'\n')
  (out/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,cases=results,scope='Actual linked task, controller/platform/native aperture and clock metadata; shared rail/pin/PMIC returns, scheduler/time, translations, MMIO/card and USB queue modeled. No physical qualification.'),indent=2)+'\n')
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__)
 for n in ('image','session','output'):p.add_argument('--'+n,type=Path,required=True)
 p.add_argument('--case',choices=('deadline-after-take',))
 a=p.parse_args();run(a.image,a.session,a.output,a.case)
