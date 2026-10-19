#!/usr/bin/env python3
"""Feed actual compiled runtime PCM USB producer records to the supervised host."""
import argparse,hashlib,importlib.util,json,struct
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_INTR,UC_HOOK_MEM_READ
from unicorn.arm64_const import UC_ARM64_REG_SP as SP,UC_ARM64_REG_X0 as X0,UC_ARM64_REG_X1 as X1,UC_ARM64_REG_LR as LR
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
for n in ('fixture','session','output'):p.add_argument('--'+n,type=Path,required=True)
p.add_argument('--case',choices=['counter-snapshot-race'])
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
spec=importlib.util.spec_from_file_location('runtime_host',ROOT/'tools/check-a733-i2s-supervised.py');host=importlib.util.module_from_spec(spec);spec.loader.exec_module(host)
session=a.session.read_bytes();fields={}
with a.fixture.open('rb') as f:
 elf=ELFFile(f);segments=[(s['p_vaddr'],bytes(s.data()),s['p_memsz']) for s in elf.iter_segments() if s['p_type']=='PT_LOAD']
 symbols={s.name:int(s['st_value']) for s in elf.get_section_by_name('.symtab').iter_symbols()}
 for cu in elf.get_dwarf_info().iter_CUs():
  for d in cu.iter_DIEs():
   if d.tag=='DW_TAG_structure_type' and d.attributes.get('DW_AT_name') and d.attributes['DW_AT_name'].value in (b'i2s_application',b'i2s_runtime'):
    fields[d.attributes['DW_AT_name'].value.decode()]={c.attributes['DW_AT_name'].value.decode():int(c.attributes['DW_AT_data_member_location'].value) for c in d.iter_children() if c.tag=='DW_TAG_member'}
serial=0

def produced(op,accepted,consumed,*,state=1,flags=0,drained=0,upload_result=0,race=False):
 global serial
 cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);pages=set()
 for at,data,size in segments:
  for page in range(at&~4095,(at+size+4095)&~4095,4096):
   if page not in pages:cpu.mem_map(page,4096);pages.add(page)
  cpu.mem_write(at,data)
 cpu.mem_map(0x50000000,0x10000);cpu.mem_write(0x50000000,b'\0\0\x20\xd4')
 cpu.reg_write(SP,0x5000f000);cpu.hook_add(UC_HOOK_INTR,lambda m,*_:m.emu_stop())
 app=symbols['app'];rt=app+fields['i2s_application']['runtime']
 cpu.mem_write(app+fields['i2s_application']['admitted'],session)
 cpu.mem_write(app+fields['i2s_application']['result'],struct.pack('<I',state))
 for name,value in [('head',accepted),('tail',accepted),('committed',accepted*2048),('consumed',consumed*2048)]:cpu.mem_write(rt+fields['i2s_runtime'][name],struct.pack('<Q',value))
 for name,value in [('flags',flags),('drained',drained),('closed',state in (2,3))]:cpu.mem_write(rt+fields['i2s_runtime'][name],struct.pack('<I',value))
 if race:
  cpu.mem_write(rt+fields['i2s_runtime']['committed'],struct.pack('<Q',(accepted-1)*2048))
  observations=[]
  def interleave(machine,access,address,size,value,context):
   consumed_address=rt+fields['i2s_runtime']['consumed'];committed_address=rt+fields['i2s_runtime']['committed']
   if address not in (consumed_address,committed_address):return
   observations.append(address)
   if len(observations)==1:assert address==consumed_address,'USB read committed before consumed'
   if address==committed_address:
    machine.mem_write(committed_address,struct.pack('<Q',accepted*2048))
    machine.mem_write(consumed_address,struct.pack('<Q',accepted*2048))
  cpu.hook_add(UC_HOOK_MEM_READ,interleave)
 for fn,args in [('pcm_response',(op,upload_result)),('exchange',())]:
  cpu.reg_write(LR,0x50000000)
  for register,value in zip((X0,X1),args):cpu.reg_write(register,value)
  cpu.emu_start(symbols[fn],0,count=300000)
 extent=int.from_bytes(cpu.mem_read(symbols['wire_bytes'],4),'little');assert extent==128
 value=bytes(cpu.mem_read(symbols['wire'],extent));serial+=1;(a.output/f'producer-{serial:04}.bin').write_bytes(value);return value

class Clock:
 value=100.
 def now(self):self.value+=.0001;return self.value
 def pause(self,n):self.value+=max(.001,n)
cases={}
if a.case:
 frame=produced(0,5,3,race=True)
 assert struct.unpack_from('<2Q',frame,80)==(5*2048,3*2048)
 cases['counter-snapshot-race']=True
pcm=b''.join(bytes([0x31+n])*4096 for n in range(12))
for name in (() if a.case else ('nominal','failed-input-drains','wrong-session','overclaimed-consumption','stale-acceptance','reserved-nonzero','durable-result-failure')):
 dest=a.output/name;dest.mkdir();record={};accepted=0;consumed=0;clock=Clock();cancelled=False
 def exchange(packet,timeout):
  global accepted,consumed,cancelled
  op,low,high=struct.unpack_from('<III',packet);seq=low|high<<32
  if op==6:
   assert len(packet)==4108 and seq==accepted+1 and packet[12:]==pcm[accepted*4096:(accepted+1)*4096]
   accepted+=1;consumed=max(0,accepted-4)
  elif op==2:assert len(packet)==12;cancelled=True
  else:assert op==0 and len(packet)==12
  failed=name=='failed-input-drains' and accepted>=6
  value=bytearray(produced(op,accepted,consumed,state=3 if failed else 2 if cancelled else 1,
      flags=int(failed),drained=int(cancelled),upload_result=1 if op==6 else 0))
  if name=='wrong-session':value[40]^=1
  if name=='overclaimed-consumption':struct.pack_into('<Q',value,88,(accepted+1)*2048)
  if name=='stale-acceptance' and op==6:struct.pack_into('<Q',value,72,accepted-1)
  if name=='reserved-nonzero':value[120]=1
  return bytes(value)
 def save():
  if name=='durable-result-failure' and record.get('durable_result'):raise OSError('modeled durable terminal journal failure')
 try:host.capture_runtime(exchange,session,dest,record,save,130,pcm,8,now=clock.now,pause=clock.pause)
 except (ValueError,RuntimeError,OSError):assert name not in ('nominal','failed-input-drains'),name
 else:assert record['i2s_result']==('observed-failure' if name=='failed-input-drains' else 'observed-success') and not record['runtime_pcm_output']['wire_drain_established']
 if name=='durable-result-failure':assert record['i2s_result']=='unobserved' and 'durable_result' not in record
 cases[name]=True
(a.output/'result.json').write_text(json.dumps({'passed':True,'cases':cases,'fixture_sha256':hashlib.sha256(a.fixture.read_bytes()).hexdigest(),'session_sha256':hashlib.sha256(session).hexdigest(),'physical_access':False,'scope':'Actual compiled USB metadata producer; client/service effects modeled. Not native-image or hardware execution.'},indent=2)+'\n')
print('PASS runtime PCM host: actual USB producer binding, distinct input, memory counts, cancellation/failure drain and malformed/durability refusal')
