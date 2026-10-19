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
 for name,value in [('head',accepted),('tail',accepted),('committed',min(4097,accepted*2048)),('consumed',consumed)]:cpu.mem_write(rt+fields['i2s_runtime'][name],struct.pack('<Q',value))
 for name,value in [('flags',flags),('drained',drained),('closed',state in (2,3)),('file_mode',1),('file_done',state==2),('eof',op==8 or state==2)]:cpu.mem_write(rt+fields['i2s_runtime'][name],struct.pack('<I',value))
 cpu.mem_write(rt+fields['i2s_runtime']['file_frames'],struct.pack('<Q',4097))
 for fn,args in [('pcm_response',(op,upload_result)),('exchange',())]:
  cpu.reg_write(LR,0x50000000)
  for register,value in zip((X0,X1),args):cpu.reg_write(register,value)
  cpu.emu_start(symbols[fn],0,count=300000)
 extent=int.from_bytes(cpu.mem_read(symbols['wire_bytes'],4),'little');assert extent==160
 value=bytes(cpu.mem_read(symbols['wire'],extent));serial+=1;(a.output/f'producer-{serial:04}.bin').write_bytes(value);return value

class Clock:
 value=100.
 def now(self):self.value+=.0001;return self.value
 def pause(self,n):self.value+=max(.001,n)
cases={};pcm=b'1'*4096+b'2'*4096+b'33'
for name in ('nominal','wrong-session','wire-overclaim','tail-overclaim','reserved-nonzero','durable-result-failure'):
 dest=a.output/name;dest.mkdir();record={};accepted=0;clock=Clock();sealed=False
 def exchange(packet,timeout):
  global accepted,sealed
  op,low,high=struct.unpack_from('<III',packet);seq=low|high<<32
  if op==6:
   assert seq==accepted+1 and packet[12:]==pcm[accepted*4096:(accepted+1)*4096]
   accepted+=1
  elif op in (7,8):
   assert seq==4097 and len(packet)==12
   if op==8:sealed=True
  else:assert op==0 and len(packet)==12
  value=bytearray(produced(op,accepted,4097 if sealed else 0,state=2 if sealed else 1,
      drained=int(sealed),upload_result=1 if op in (6,7,8) else 0))
  if name=='wrong-session':value[40]^=1
  if name=='wire-overclaim':struct.pack_into('<I',value,140,1)
  if name=='tail-overclaim' and sealed:struct.pack_into('<Q',value,88,6144)
  if name=='reserved-nonzero':value[152]=1
  return bytes(value)
 def save():
  if name=='durable-result-failure' and record.get('durable_result'):raise OSError('modeled terminal journal failure')
 try:host.capture_runtime_file(exchange,session,dest,record,save,130,pcm,now=clock.now,pause=clock.pause)
 except (ValueError,RuntimeError,OSError):assert name!='nominal',name
 else:
  assert name=='nominal' and record['i2s_result']=='observed-success'
  assert record['runtime_pcm_output']['consumed']==4097 and not record['runtime_pcm_output']['wire_drain_established']
 if name=='durable-result-failure':assert record['i2s_result']=='unobserved' and 'durable_result' not in record
 cases[name]=True
(a.output/'result.json').write_text(json.dumps({'passed':True,'cases':cases,'fixture_sha256':hashlib.sha256(a.fixture.read_bytes()).hexdigest(),'session_sha256':hashlib.sha256(session).hexdigest(),'physical_access':False,'scope':'Actual compiled USB metadata producer; effects modeled. Not hardware execution.'},indent=2)+'\n')
print('PASS file host actual native response binding, exact tail accounting, wire-unknown and durability refusals')
