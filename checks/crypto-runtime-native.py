#!/usr/bin/env python3
"""Execute runtime upload, task ownership and copied USB output; CE results are modeled."""
import argparse,hashlib,importlib.util,json,struct
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0 as X0,UC_ARM64_REG_X1 as X1,UC_ARM64_REG_X2 as X2,UC_ARM64_REG_PC as PC,UC_ARM64_REG_SP as SP,UC_ARM64_REG_LR as LR
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
p=argparse.ArgumentParser(description=__doc__)
for n in ('image','session','output'):p.add_argument('--'+n,type=Path,required=True)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
prep=module('prep',ROOT/'apps/crypto/prepare.py');observe=module('observe',ROOT/'apps/crypto/observe.py')
with a.image.open('rb') as f:
 elf=ELFFile(f);segments=[(int(s['p_vaddr']),bytes(s.data())) for s in elf.iter_segments() if s['p_type']=='PT_LOAD']
 candidates={};layouts={};local={}
 for s in elf.get_section_by_name('.symtab').iter_symbols():
  if s['st_value']:candidates.setdefault(s.name,set()).add(int(s['st_value']))
 for cu in elf.get_dwarf_info().iter_CUs():
  unit=cu.get_top_DIE().attributes['DW_AT_name'].value.decode()
  for d in cu.iter_DIEs():
   if d.tag=='DW_TAG_subprogram' and 'DW_AT_name' in d.attributes and 'DW_AT_low_pc' in d.attributes:
    local[unit,d.attributes['DW_AT_name'].value.decode()]=int(d.attributes['DW_AT_low_pc'].value)
   if d.tag=='DW_TAG_structure_type' and 'DW_AT_name' in d.attributes:
    fields={c.attributes['DW_AT_name'].value.decode():int(c.attributes['DW_AT_data_member_location'].value) for c in d.iter_children() if c.tag=='DW_TAG_member' and 'DW_AT_name' in c.attributes and 'DW_AT_data_member_location' in c.attributes}
    if fields:layouts[d.attributes['DW_AT_name'].value.decode()]=fields
 def address(name):
  if name=='eligible':
   found=[v for (unit,n),v in local.items() if unit.endswith('/apps/crypto/native.c') and n==name]
   assert len(found)==1,('Ambiguous native eligibility hook',found);return found[0]
  values=candidates.get(name,set());assert len(values)==1,(name,values);return next(iter(values))
 # Resolve every hook before any runtime case.
 names=('crypto','board','crypto_task','crypto_construct','crypto_run_step','eligible','a733_ce_session_retain','xQueueSemaphoreTake','xQueueGenericSend','vTaskDelay','board_live','board_usb0_task','a733_native_usb_step','a733_native_usb_health','tusb_rhport_init','tud_task_ext','tud_vendor_n_available','tud_vendor_n_read','tud_vendor_n_write','tud_vendor_n_write_flush','crypto_response','crypto_response_bytes','operation_input')
 symbols={n:address(n) for n in names}
app=layouts['crypto_application'];run=layouts['crypto_run'];engine=layouts['a733_ce_session'];cipher=layouts['a733_ce_cipher'];board=layouts['a733_board_session']
base=symbols['crypto'];results={};frames={}
_,_,identity=prep.base.image_parts(a.image)
for variant in ('partial-upload','upload-success','wrong-request','postpublication-write','expired-upload','expired-after-key-copy','producer-owned-cleanup','output-success','late-publication'):
 request=prep.operation.encode(2,0xfedcba9876543210,bytes(range(64)),bytes(range(16)))
 session=bytearray(a.session.read_bytes());session[48:80]=identity;struct.pack_into('<Q',session,600,2)
 session[760:792]=hashlib.sha256(request).digest();session[792:824]=bytes(32);session=bytes(session)
 prep.session_check(session);prep.operation.decode(request,session)
 cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);cpu.mem_map(0x40000000,0x4000000);cpu.mem_map(0x50000000,0x1000000)
 for at,data in segments:cpu.mem_write(at,data)
 cpu.mem_write(base,session);cpu.mem_write(symbols['board'],session[:592])
 cpu.mem_write(symbols['board']+board['deadline_us'],struct.pack('<Q',10**12))
 cpu.mem_write(base+app['mutex'],struct.pack('<Q',1))
 request_at=base+app['run']+run['request'];cipher_key=base+app['run']+run['cipher']+cipher['key']
 pos=[0];limit=[0];live=[True];done=[False];held=[False];calls=[]
 uploaded=b'\1'+struct.pack('<I',len(request))+request
 if variant=='wrong-request':uploaded=uploaded[:-1]+bytes([uploaded[-1]^1])
 if variant=='partial-upload':uploaded=uploaded[:40]
 def write(at,value):cpu.mem_write(at,struct.pack('<I',value))
 def returned(value=0):cpu.reg_write(X0,value&0xffffffffffffffff);cpu.reg_write(PC,cpu.reg_read(LR))
 def hook(machine,at,size,unused):
  name=next((n for n,v in symbols.items() if v==at),None)
  if name is None:return
  if name=='board_live':returned(live[0]);return
  if name=='vTaskDelay':done[0]=True;machine.emu_stop();return
  if name in ('a733_native_usb_step','a733_native_usb_health','tusb_rhport_init','tud_task_ext','tud_vendor_n_write_flush'):returned(1);return
  if name=='tud_vendor_n_available':returned(max(0,min(limit[0]-pos[0],len(uploaded)-pos[0])));return
  if name=='tud_vendor_n_read':
   n=machine.reg_read(X2);assert n<=limit[0]-pos[0]
   machine.mem_write(machine.reg_read(X1),uploaded[pos[0]:pos[0]+n]);pos[0]+=n;returned(n);return
  if name=='tud_vendor_n_write':returned(machine.reg_read(X2));return
  if name=='crypto_construct':
   calls.append(name);assert int.from_bytes(machine.mem_read(base+app['input_ready'],4),'little')==1
   write(base+app['engine']+engine['initialized'],1);returned(1);return
  if name=='crypto_run_step':
   calls.append(name);assert held[0]
   machine.mem_write(base+app['run']+run['payload'],b'\x5a'*64)
   write(base+app['run']+run['output_bytes'],64);write(base+app['run']+run['stage'],8);returned(1);return
  if name=='eligible':returned(variant!='late-publication');return
  if name=='xQueueSemaphoreTake':assert not held[0];held[0]=True;returned(1);return
  if name=='xQueueGenericSend':assert held[0];held[0]=False;returned(1);return
  if name=='a733_ce_session_retain':calls.append(name);assert held[0];returned(5);return
 cpu.hook_add(UC_HOOK_CODE,hook)
 def execute(name):
  done[0]=False;cpu.reg_write(SP,0x50ffeff0);cpu.reg_write(X0,0)
  cpu.emu_start(symbols[name],0,count=3000000);assert done[0],(variant,name,hex(cpu.reg_read(PC)))
 if variant in ('expired-after-key-copy','producer-owned-cleanup'):
  live[0]=False;cpu.mem_write(request_at+32,b'\x33'*16);cpu.mem_write(cipher_key,b'\x44'*16)
  if variant=='expired-after-key-copy':write(base+app['input_ready'],1);write(base+app['engine']+engine['initialized'],1)
  execute('crypto_task')
  assert bytes(cpu.mem_read(cipher_key,16))==bytes(16)
  assert bytes(cpu.mem_read(request_at+32,16))==(bytes(16) if variant=='expired-after-key-copy' else b'\x33'*16)
  assert not calls or calls==['a733_ce_session_retain']
 else:
  for end in range(1,len(uploaded)+16,17):
   limit[0]=min(end,len(uploaded))
   if variant=='expired-upload' and end>30:live[0]=False
   execute('board_usb0_task')
   if not live[0]:break
   if variant=='wrong-request' and pos[0]==len(uploaded):break
  ready=int.from_bytes(cpu.mem_read(base+app['input_ready'],4),'little')
  assert ready==(0 if variant in ('partial-upload','wrong-request','expired-upload') else 1),(variant,ready)
  if not ready:
   execute('crypto_task');assert not calls
  else:
   assert bytes(cpu.mem_read(request_at,len(request)))==request
   assert bytes(cpu.mem_read(symbols['operation_input'],2096))==bytes(2096)
   if variant=='postpublication-write':
    original=bytes(cpu.mem_read(request_at,len(request)));pos[0]=0;limit[0]=len(uploaded)
    execute('board_usb0_task') # Existing pending response is sent first.
    write(symbols['crypto_response_bytes'],0);execute('board_usb0_task')
    assert bytes(cpu.mem_read(request_at,len(request)))==original
   elif variant in ('output-success','late-publication'):
    execute('crypto_task');assert calls==['crypto_construct','crypto_run_step']+(['a733_ce_session_retain'] if variant=='late-publication' else [])
    status=int.from_bytes(cpu.mem_read(base+app['result'],4),'little');assert status==(2 if variant=='late-publication' else 1)
    if status==1:
     uploaded=b'\0';pos[0]=0;limit[0]=1;write(symbols['crypto_response_bytes'],0);execute('board_usb0_task')
     frame=bytes(cpu.mem_read(symbols['crypto_response'],208));assert observe.decode(frame,session,request=request)['request_id']==0xfedcba9876543210
     (a.output/'runtime-frame.bin').write_bytes(frame);(a.output/'runtime-session.bin').write_bytes(session);(a.output/'operation.bin').write_bytes(request)
 results[variant]={'passed':True,'provider_calls':calls}
(a.output/'result.json').write_text(json.dumps({'passed':True,'physical':False,'image_sha256':hashlib.sha256(a.image.read_bytes()).hexdigest(),'cases':results},indent=2)+'\n')
print('PASS runtime crypto linked upload/ownership, deferred construction, copied identity/output, key cleanup and late retention')
