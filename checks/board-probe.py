#!/usr/bin/env python3
"""Check the native probe ABI and execute its actual AArch64 code without USB.

Models PPU/MMIO/timers and cache instruction effects; establishes ordering and
admission, not physical cache visibility, IRQ drain or hardware qualification.
"""
import argparse
import gc
import importlib.util
import json
from pathlib import Path
import struct
import sys
from unittest.mock import patch
sys.dont_write_bytecode=True
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'));sys.path.insert(0,str(ROOT/'checks'))
import board_probe as probe
import harness
from register_latency import RegisterLatency, PostedWrites
from unicorn import UC_HOOK_MEM_WRITE,UC_HOOK_MEM_READ
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC,UC_ARM64_REG_PSTATE,UC_ARM64_REG_FP,UC_ARM64_REG_LR
spec=importlib.util.spec_from_file_location('probe_machine',ROOT/'checks/memtest-machine.py')
machine=importlib.util.module_from_spec(spec);spec.loader.exec_module(machine)
spec=importlib.util.spec_from_file_location('probe_crumbs',ROOT/'tools/a733-crumbs.py')
crumbs=importlib.util.module_from_spec(spec);spec.loader.exec_module(crumbs)
machine.REGISTERS.update({(3,6,1,0,0):'sctlr_el3',(3,0,0,0,5):'mpidr_el1',
 (3,6,5,2,0):'esr_el3',(3,6,4,0,1):'elr_el3',(3,6,6,0,0):'far_el3',(3,6,12,0,2):'rmr_el3'})
ROWS=[]

def passed(name):
 ROWS.append({'name':name,'passed':True});print('PASS',name,flush=True)

class Model(machine.Machine):
 def __init__(self,image,plan):
  super().__init__(image,step=1)
  self.map(0x02050000,4096);self.map(0x08000000,8192)
  self.sys.update(currentel=12,sctlr_el3=0,sctlr_el1=0,mpidr_el1=0,esr_el3=0x17<<26,elr_el3=0x41006000)
  self.ppu_events=[];self.pending_ppu={};self.posted=PostedWrites({})
  self.ppu_delay=6;self.ppu_stuck=False;self.ppu_wake=False;self.request_race=False
  self.events=[];self.stuck=False;self.disarm_seen=False;self.fault=None
  self.uc.reg_write(UC_ARM64_REG_PSTATE,0x3cd)
  self.write(0x02050014,0xb1,4);self.write(0x02050010,1,4);self.write(0x02050018,2,4)
  for c in range(1,8):
   p=0x07052000+c*4096
   for off,value in [(0,0),(4,0),(8,0),(0xfb0,0x10110100),(0xfc8,0x0b60043b),(0xfcc,0x11)]:self.write(p+off,value,4)
   self.write(0x07050200+c*4,2,4)
  for address,path in plan['uploads']:self.uc.mem_write(address,path.read_bytes())
  self.uc.mem_write(self.address('saved'),struct.pack('<8I',1,3,0x33,0,0,0,0,0)+bytes(128))
  self.write(0x03400004,2,4);self.write(0x03400000,0x33,4)
  self.uc.hook_add(UC_HOOK_MEM_WRITE,self.mmio_write,begin=0x02050000,end=0x02050fff)
  self.uc.hook_add(UC_HOOK_MEM_READ,self.mmio_read,begin=0x02050014,end=0x02050017)
  self.uc.hook_add(UC_HOOK_MEM_WRITE,self.ppu_write,begin=0x07050000,end=0x0705ffff)
  self.uc.hook_add(UC_HOOK_MEM_READ,self.ppu_read,begin=0x07050000,end=0x0705ffff)
  sites=[];barriers=[];icaches=[]
  for address,data in self.e.parts:
   for i,(word,) in enumerate(struct.iter_unpack('<I',data[:len(data)&~3])):
    if word in (0xd503205f,0xd503207f,0xd69f03e0):sites.append(address+4*i)
    if word==0xd5033f9f:barriers.append(address+4*i)
    if word==0xd508751f:icaches.append(address+4*i)
  harness.hook_addresses(self.uc,sites,self.stop_instruction)
  harness.hook_addresses(self.uc,barriers,self.barrier)
  harness.hook_addresses(self.uc,icaches,lambda *a:self.events.append('cache'))
 def barrier(self,*args):
  for a,v in self.posted.complete():
   if a in range(0x07053000,0x0705a000,4096):
    old=self.read(a+8,4)
    self.pending_ppu[a]=(RegisterLatency(self.now,self.ppu_delay,0xffffffff,v,stuck=self.ppu_stuck),old,v)
    self.write(a,old,4)
 def ppu_write(self,uc,access,address,size,value,data):
  self.ppu_events.append(('write',address,value))
  self.posted.write(address,value)
 def ppu_read(self,uc,access,address,size,value,data):
  assert not self.posted.pending, ('dependent-read-before-dsb',hex(address))
  for p,(effect,old,wanted) in list(self.pending_ppu.items()):
   if address not in (p,p+8):continue
   state=effect.apply(self.now,0x108 if self.ppu_wake else old)
   self.write(p+8,state,4)
   self.write(p,old if effect.pending else wanted,4)
   if not effect.pending:
    self.ppu_events.append(('ack',p,state));del self.pending_ppu[p]
   elif address==p:raise AssertionError('PWPR verified before PWSR completion')
  if self.request_race and self.pending_ppu:self.write(0x07050204,3,4)
  self.ppu_events.append(('read',address,self.read(address,4)))
 def mmio_write(self,uc,access,address,size,value,data):
  if address==0x02050014:
   self.events.append('disarm');self.disarm_seen=True
  if address==0x0205000c:self.events.append('feed')
 def mmio_read(self,uc,access,address,size,value,data):
  self.write(0x02050014,0xb1 if self.stuck and self.disarm_seen else self.read(0x02050014,4)&0xffff,4)
 def stop_instruction(self,uc,address,size,data):
  uc.emu_stop()
 def _system(self,uc,address,size,data):
  word=int.from_bytes(uc.mem_read(address,4),'little')
  if word==machine.SMC:
   self.events.append('smc');self.smc=[uc.reg_read(UC_ARM64_REG_X0+i) for i in range(5)];uc.emu_stop();return
  name,read,rt=machine.decode_system(word)
  if word==0xd508751f or (isinstance(name,str) and name.startswith('dc ')):self.events.append('cache')
  if name=='dc cvac':self.events.append('publish')
  if name=='sctlr_el1' and not read:self.events.append('disable-el1')
  if name=='rmr_el3':self.events.append('rom');uc.emu_stop();return
  if word&0xfffff0ff==0xd50340df:
   uc.reg_write(UC_ARM64_REG_PSTATE,uc.reg_read(UC_ARM64_REG_PSTATE)|0x3c0)
   uc.reg_write(UC_ARM64_REG_PC,address+4);return
  return super()._system(uc,address,size,data)
 def begin(self):self.call('sunstone_probe_begin',count=200000)
 def rtc(self):return {name:self.read(0x07090000+off,4) for name,off in {'tag':0x100,'last':0x104,'time':0x10c,'fault':0x110,'esr':0x114,'elr':0x118,'far':0x11c}.items()}


def ownership_cases(args,plan):
 for label,delay in ([] if args.only=='remaining' else [('cpu1-delayed',6),('all-seven-delayed',23),('inflight-wake',11),('denied',6),('stopped-counter',6),('deadline',6),('request-race',6),('bad-last-identity',6)]):
  m=Model(args.elf,plan);m.ppu_delay=delay
  cores=range(1,8) if label=='all-seven-delayed' else [1]
  for c in cores:m.write(0x07052000+c*4096,0x100,4);m.write(0x07052008+c*4096,0x100,4)
  m.ppu_wake=label=='inflight-wake';m.ppu_stuck=label in ('denied','stopped-counter','deadline')
  if label=='stopped-counter':m.step=0
  if label=='deadline':m.step=240000
  if label=='request-race':m.request_race=True
  if label=='bad-last-identity':m.write(0x07059fc8,0,4)
  before=[bytes(m.uc.mem_read(a,4096)) for a in (0x07051000,0x07052000)]
  m.call('sunstone_probe_begin',count=12000000)
  writes=[e for e in m.ppu_events if e[0]=='write']
  assert all(a in [0x07052000+c*4096 for c in range(1,8)] for _,a,v in writes),writes
  assert before==[bytes(m.uc.mem_read(a,4096)) for a in (0x07051000,0x07052000)]
  if label in ('cpu1-delayed','all-seven-delayed','inflight-wake'):
   assert not m.rtc()['fault'],m.rtc()
   assert writes==[('write',0x07052000+c*4096,0) for c in cores],writes
   assert len([e for e in m.ppu_events if e[0]=='ack'])==len(cores)
   m.write(probe.SESSION+128,0);m.write(probe.SESSION+136,0);m.write(probe.SESSION+144,0x5944414552505353)
   m.call('sunstone_probe_return',0,count=300000)
   assert m.events[-2:]==['disarm','rom'] and all(m.read(0x07052000+c*4096,4)==0 for c in range(1,8))
   inherited=sum(1<<c for c in cores)
   rtc=m.rtc();assert (rtc['esr'],rtc['elr'],rtc['far'])==(inherited,0x50505531,0xfe),rtc
   decoded=crumbs.decode(rtc,plan['epoch'])['probe_policy']
   assert decoded['inherited_dynamic_mask']==inherited and decoded['normalized_mask']==0xfe
   assert decoded['inherited_policy']=={str(c):0x100 if c in cores else 0 for c in range(1,8)}
  else:
   assert m.rtc()['fault']>>24==0xfa and not m.disarm_seen,m.rtc()
   if label=='bad-last-identity':assert not writes
   if label in ('denied','stopped-counter','deadline'):assert m.rtc()['fault']&255==0x28,m.rtc()
  passed('ownership-'+label);del m;gc.collect()
 # Exercise all seven wake-control writes with upper fields preserved.
 m=Model(args.elf,plan)
 for c in range(1,8):m.write(0x07050200+c*4,0xa0+(c<<8),4)
 m.begin()
 writes=[e for e in m.ppu_events if e[0]=='write']
 assert writes==[('write',0x07050200+c*4,0xa2+(c<<8)) for c in range(1,8)],writes
 assert not m.rtc()['fault'] and not m.posted.pending
 passed('ownership-wake-disable-preserves-upper-fields');del m;gc.collect()
 # Execute the original held-state gate directly: every predicate and every
 # preceding read stays ordered; an early failure prevents all later reads.
 checks=[(0xfc8,0xfff00fff,0x0b60043b),(0xfcc,0xffffffff,0x11),(0xfb0,0x101101ff,0x10110100),
         (0,0xffffffff,0),(4,0xffffffff,0),(8,0xffffffff,0)]
 checks=[(0x07053000+o,m,v) for o,m,v in checks]+[(0x07050204,3,2),(0x07050224,1,0)]
 for i,(a,mask,wanted) in enumerate(checks):
  m=Model(args.elf,plan);m.write(a,wanted^(mask&-mask),4)
  assert m.call('secondaries_off',count=10000)==0
  reads=[e[1] for e in m.ppu_events if e[0]=='read']
  assert reads==[c[0] for c in checks[:i+1]],(i,reads)
  fault=m.address('secondary_fault');assert m.read(fault)==a and m.read(fault+8,4)==wanted^(mask&-mask) and m.read(fault+12,4)==i
  passed('original-held-predicate-'+str(i));del m;gc.collect()
 for key,value,site in [('currentel',4,0x10),('mpidr_el1',0x100,0x11),('sctlr_el3',4,0x12)]:
  m=Model(args.elf,plan);m.sys[key]=value;m.begin();rtc=m.rtc()
  assert rtc['fault']&255==site and rtc['esr']==value and not m.ppu_events,rtc
  m.call('sunstone_probe_fault',0xff,1,2,3,count=1000);assert m.rtc()==rtc
  passed('entry-first-input-'+key);del m;gc.collect()

def native_cases(args,plan):
 for variant in ('normal','bad-nonce','bad-input','bad-watchdog','dynamic-off','bad-ack','wake-enabled'):
  m=Model(args.elf,plan)
  if variant=='bad-nonce':m.write(probe.SESSION+24,0)
  elif variant=='bad-input':m.write(0x41005000,0xff,1)
  elif variant=='bad-watchdog':m.write(0x02050014,0,4)
  elif variant=='dynamic-off':m.write(0x07053000,0x100,4);m.write(0x07053008,0x100,4)
  elif variant=='bad-ack':m.write(0x07053008,0x100,4)
  elif variant=='wake-enabled':m.write(0x07050204,0,4)
  m.begin()
  assert not m.disarm_seen
  if variant in ('normal','dynamic-off','wake-enabled'):
   assert m.rtc()['fault']==0 and m.read(m.address('sunstone_probe_deadline'))>m.now,m.rtc()
   assert all(m.read(0x07052000+c*4096,4)==0 and m.read(0x07050200+c*4,4)&3==2 for c in range(1,8))
   rtc=m.rtc();assert (rtc['esr'],rtc['elr'],rtc['far'])==(2 if variant=='dynamic-off' else 0,0x50505531,0xfe),rtc
  else:assert m.rtc()['fault']>>24==0xfa
  passed('entry-'+variant);del m;gc.collect()
 for variant in ('normal','irq-failure','late-secondary','late-deadline','stuck-disarm'):
  m=Model(args.elf,plan);m.begin()
  payload=b'probe result';m.uc.mem_write(plan['marker']['payload_address'],payload)
  m.write(probe.SESSION+128,len(payload));m.write(probe.SESSION+136,0);m.write(probe.SESSION+144,0x5944414552505353)
  if variant=='irq-failure':m.write(m.address('saved'),0,4)
  elif variant=='late-secondary':m.write(0x07053008,8,4)
  elif variant=='late-deadline':m.now=m.read(m.address('sunstone_probe_deadline'))
  elif variant=='stuck-disarm':m.stuck=True
  m.call('sunstone_probe_return',0,count=300000)
  if variant=='normal':
   assert m.events[-2:]==['disarm','rom'],m.events
   raw=bytes(m.uc.mem_read(probe.RECEIPT,128));r=probe.receipt(raw,plan)
   assert r['payload_hash']==probe.fnv(payload) and m.rtc()['last']==0x05030000
  else:
   assert 'rom' not in m.events and m.rtc()['fault']>>24==0xfa
   if variant!='stuck-disarm':assert 'disarm' not in m.events
  passed('return-'+variant);del m;gc.collect()
 m=Model(args.elf,plan);m.begin();m.sys['currentel']=4;m.sys['sctlr_el1']=0x30d01805
 m.call('sunstone_probe_complete',0,64,count=100000)
 assert m.smc[:2]==[0x733,0] and m.sys['sctlr_el1']&0x1005==0
 assert m.events.index('publish')<m.events.index('disable-el1')<m.events.index('smc')
 passed('el1-publishes-before-disable-and-smc')
 m.write(0x42003000+56,3) # changed below via native DWARF stop offset
 hostspec=importlib.util.spec_from_file_location('probe_memtest_host',ROOT/'apps/memtest/check.py')
 host=importlib.util.module_from_spec(hostspec);hostspec.loader.exec_module(host)
 m.write(0x42003000+host.Record.stop.offset,3)
 m.call('sunstone_probe_complete',0,64,count=100000)
 assert m.smc[:2]==[0x733,0xffffffff]
 passed('damaged-recovery-refuses-normal-completion')
 m.sys.update(currentel=4,esr_el1=0x96000045,elr_el1=0x41008880,far_el1=0xdead0000)
 m.call('platform_exception',count=10000)
 values=[0x735,0x104,0x96000045,0x41008880,0xdead0000]
 assert m.smc==values
 m.sys.update(currentel=12,esr_el3=0x17<<26)
 for i,v in enumerate(values):m.uc.reg_write(UC_ARM64_REG_X0+i,v)
 m.call('monitor_sync',*values,count=10000)
 rtc=m.rtc();assert rtc['esr']==values[2] and rtc['elr']==values[3] and rtc['far']==values[4] and rtc['fault']==0xfa010004,rtc
 m.call('sunstone_probe_fault',8,1,2,3,count=10000)
 assert m.rtc()==rtc
 passed('fatal-smc-preserves-el1-fault-and-first-failure')
 m=Model(args.elf,plan);m.begin()
 for i in range(1,30):m.uc.reg_write(UC_ARM64_REG_X0+i if i<29 else UC_ARM64_REG_FP,0x12340000+i)
 # call() writes LR sentinel, which must also survive the renewal.
 m.call('monitor_sync',0x734,count=10000)
 for i in range(1,30):assert m.uc.reg_read(UC_ARM64_REG_X0+i if i<29 else UC_ARM64_REG_FP)==0x12340000+i,i
 assert m.uc.reg_read(UC_ARM64_REG_LR)==machine.SENTINEL
 assert m.events.count('feed')==1
 m.now=m.read(m.address('sunstone_probe_deadline'))
 m.call('monitor_sync',0x734,count=10000)
 assert m.events.count('feed')==1 and m.rtc()['fault']>>24==0xfa
 passed('renewal-registers-preserved-and-deadline-stops-feed')
 m=Model(args.elf,plan);m.platform_tables()
 before=m.call('recovery_digest',count=200000)
 address=m.address('sunstone_probe_return');m.write(address,m.read(address,4)^1,4)
 after=m.call('recovery_digest',count=200000)
 assert before!=after
 passed('probe-helper-corruption-changes-recovery-digest')
 m.sys['currentel']=4
 m.write(0x42003000+host.Record.digest_admission.offset,before)
 definitions=[v for (unit,name),v in m.e.functions.items() if unit.endswith('apps/memtest/main.c') and name=='finish']
 assert len(definitions)==1 and len(definitions[0])==1
 m.call('finish',1,0,address=next(iter(definitions[0])),count=200000)
 assert m.read(0x42003000+host.Record.stop.offset)==3 and m.smc[:2]==[0x733,0xffffffff]
 passed('last-phase-corruption-is-fatal-at-memtest-finish')


def negative_host_cases(args):
 from elftools.elf.elffile import ELFFile
 import io
 original=args.elf.read_bytes();elf=ELFFile(io.BytesIO(original));table=elf.get_section_by_name('.symtab')
 symbols={v.name:(i,v) for i,v in enumerate(table.iter_symbols()) if v['st_shndx']!='SHN_UNDEF'}
 mark=symbols['sunstone_probe_marker'][1];section=elf.get_section(mark['st_shndx'])
 offset=section['sh_offset']+mark['st_value']-section['sh_addr']
 for variant,reason in [('version','Unsupported'),('power-removal','power removal'),('payload-mmio','outside admitted RAM'),('input-mmio','image-RAM'),('input-unloaded','load coverage'),('read-only-without-sd-core','read-only SD core marker')]:
  data=bytearray(original)
  if variant=='version':struct.pack_into('<Q',data,offset+8,99)
  elif variant=='power-removal':struct.pack_into('<Q',data,offset+13*8,2)
  elif variant=='read-only-without-sd-core':struct.pack_into('<Q',data,offset+13*8,5)
  elif variant=='payload-mmio':struct.pack_into('<Q',data,offset+9*8,0x02050000)
  else:
   address=0x02050000 if variant=='input-mmio' else 0x41efff00
   struct.pack_into('<Q',data,offset+11*8,address)
   i,s=symbols['sunstone_probe_input'];struct.pack_into('<Q',data,table['sh_offset']+i*table['sh_entsize']+8,address)
   struct.pack_into('<Q',data,elf['e_shoff']+s['st_shndx']*elf['e_shentsize']+16,address)
  path=args.out/(variant+'.elf');path.write_bytes(data)
  try:probe.inspect(path)
  except ValueError as error:assert reason in str(error),(variant,error)
  else:raise AssertionError(variant+' admitted')
  passed('host-refuses-'+variant)

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--elf',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
 p.add_argument('--only',choices=('all','remaining'),default='all')
 args=p.parse_args();args.out.mkdir(parents=True,exist_ok=False)
 # This is an offline run input, not a physical residual-acceptance record.
 spec=importlib.util.spec_from_file_location('input_host',ROOT/'apps/memtest/check.py');host=importlib.util.module_from_spec(spec);spec.loader.exec_module(host)
 contract=host.make_contract(1,7,0x1234567812345678,18*24000000,(0,0),[])
 input_path=args.out/'memtest-contract.bin';input_path.write_bytes(bytes(contract))
 info=probe.inspect(args.elf);plan=probe.prepare(args.elf,info,[input_path],args.out/'prepared')
 assert plan['epoch'] and plan['lease'];passed('native-elf-host-admission-and-frozen-session')
 ownership_cases(args,plan)
 native_cases(args,plan)
 negative_host_cases(args)
 (args.out/'results.json').write_text(json.dumps({'physical_access':False,'cases':ROWS},indent=2)+'\n')
 print(len(ROWS),'probe checks passed')

if __name__=='__main__':main()
