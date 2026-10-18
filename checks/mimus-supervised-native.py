#!/usr/bin/env python3
"""New supervised mimus boundaries in the actual image; no recognition replay."""
import argparse,importlib.util,json,struct
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_PC,UC_ARM64_REG_LR,UC_ARM64_REG_X0
ROOT=Path(__file__).resolve().parents[1]
def load(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
def main():
 p=argparse.ArgumentParser(description=__doc__)
 for name in ('image','bundle','evidence'):p.add_argument('--'+name,type=Path,required=True)
 a=p.parse_args();a.evidence.mkdir(parents=True,exist_ok=True);results=a.evidence/'result.json'
 old=json.loads(results.read_text()) if results.exists() else {'physical':False,'cases':{}}
 native=load('mimus_native_fixture',ROOT/'checks/a733-storage-maintenance.py');prep=load('mimus_native_prep',ROOT/'apps/mimus/prepare.py')
 prepared=prep.validate_bundle(a.bundle);assert old.get('image_sha256',prepared['image_sha256'])==prepared['image_sha256'];old['image_sha256']=prepared['image_sha256']
 def case(name,fn):
  if old['cases'].get(name)=='PASS':return
  fn();old['cases'][name]='PASS';results.write_text(json.dumps(old,indent=2)+'\n')
 def machine():
  n=native.Native(a.image);n.sys.update(cntpct_el0=24000000,cntfrq_el0=24000000,daif=0,tpidr_el1=1);return n
 def put(n,name,value,size=8):n.u.mem_write(n.symbols[name],value.to_bytes(size,'little'))
 def ready(n):
  put(n,'state',1,4);put(n,'live_until',180000000);n.u.mem_write(n.symbols.get('window',n.symbols.get('window.0')),struct.pack('<Q',180000000))
  counter=n.symbols['providers']+members['a733_native_usb']['counter']
  for key,value in [('initialized',1),('frequency',24000000)]:n.u.mem_write(counter+members['a733_counter_clock'][key],struct.pack('<I',value))
 def stopped(n):
  def step(u,pc,size,unused):
   if pc==n.symbols['vTaskDelay']:n.stop='tick';u.emu_stop()
  n.u.hook_add(UC_HOOK_CODE,step)
 def contract_case(offset=None,value=None,wrong_build=False,late=False):
  n=machine();raw=bytearray(prepared['_frozen']['session.bin']);c=bytearray(prepared['_frozen']['contract.bin'])
  struct.pack_into('<Q',c,0,prep.CONTRACT_MAGIC);struct.pack_into('<4Q',c,80,100,24000100,4320000100,4320000100)
  if offset is not None:struct.pack_into('<Q',raw,offset,value)
  if wrong_build:raw[656]^=1
  c[192:224]=bytes.fromhex(prep.sha(raw));n.u.mem_write(0x41f00000,bytes(raw));n.u.mem_write(0x42004000,bytes(c))
  assert n.call('a733_smp_contract_valid',(0x42004000,24000100 if late else 101))==int(offset is None and not wrong_build and not late)
 case('actual-supervised-contract',lambda:contract_case())
 for offset,value in [(8,1),(112,1),(256,1),(640,1),(160,1),(168,2),(504,5)]:case('refuse-wire-'+str(offset),lambda o=offset,v=value:contract_case(o,v))
 case('refuse-session-build-with-recomputed-digest',lambda:contract_case(wrong_build=True))
 case('refuse-startup-deadline',lambda:contract_case(late=True))
 def usb():
  n=machine();address=n.call('tud_descriptor_device_cb');assert bytes(n.u.mem_read(address+8,4))==struct.pack('<HH',0x1f3a,0xefe8)
  for index,text in [(2,'sunstoneos native mimus'),(3,'sunstoneos-a733-mimus')]:
   address=n.call('tud_descriptor_string_cb',(index,0x409));assert bytes(n.u.mem_read(address+2,len(text)*2)).decode('utf-16le')==text
 case('exact-usb-identity',usb)
 # Derive the real provider/counter locations from this image's DWARF.
 members={}
 with a.image.open('rb') as f:
  elf=ELFFile(f)
  for cu in elf.get_dwarf_info().iter_CUs():
   for d in cu.iter_DIEs():
    name=d.attributes.get('DW_AT_name')
    if d.tag=='DW_TAG_structure_type' and name and 'DW_AT_byte_size' in d.attributes and name.value in (b'a733_native_usb',b'a733_counter_clock'):
     members[name.value.decode()]={x.attributes['DW_AT_name'].value.decode():x.attributes['DW_AT_data_member_location'].value for x in d.iter_children() if x.tag=='DW_TAG_member' and 'DW_AT_name' in x.attributes}
 counter_offset=members['a733_native_usb']['counter'];failed_offset=members['a733_counter_clock']['failed']
 def cutoff(entry,args=(),expired=False):
  n=machine();ready(n);stopped(n)
  if expired:n.sys['cntpct_el0']=180000000*24
  n.call(entry,args);assert n.stop=='tick'
  assert int.from_bytes(n.u.mem_read(n.symbols['state'],4),'little')&4
  counter=n.symbols['providers']+counter_offset
  assert int.from_bytes(n.u.mem_read(counter+failed_offset,4),'little')==1
  assert not any(address<0x41000000 for address,size,value in n.writes),'terminal path touched MMIO'
  n.u.mem_write(0x41e00000,struct.pack('<Q',0xfeed));assert n.call('a733_counter_clock_now',(counter,0x41e00000))==0 and n.q(0x41e00000)==0xfeed
 case('usb-assert-revokes-counter',lambda:cutoff('a733_usb_assert'))
 case('invalid-interface-revokes-counter',lambda:cutoff('tud_vendor_rx_cb',(1,0,0)))
 case('deadline-checkpoint-parks-without-release',lambda:cutoff('mimus_native_checkpoint',expired=True))
 def supervisor():
  n=machine();ready(n);stopped(n);put(n,'inference_progress',100);n.call('supervise');assert n.stop=='tick'
  assert int.from_bytes(n.u.mem_read(n.symbols['state'],4),'little')==1
  assert 'a733_watchdog_hw_reload' not in n.symbols and 'a733_watchdog_hw_admit' not in n.symbols
  assert not any(address<0x41000000 for address,size,value in n.writes)
 case('manual-progress-never-touches-watchdog',supervisor)
 def borrowed(kind):
  n=machine();ready(n);stopped(n);context=0x41e01000;n.u.mem_write(context,b'borrowed context retained')
  prefix='_ZN12_GLOBAL__N_1'+('6worker' if kind=='parallel' else '7capture')+'EPv'
  entry=next(name for name in n.symbols if name.startswith(prefix));events=[]
  def intercept(u,pc,size,unused):
   if pc==n.symbols['ucPortGetCoreID']:
    u.reg_write(UC_ARM64_REG_X0,1);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
   if pc==n.symbols['xQueueReceive']:
    events.append('borrow')
    if kind=='parallel':u.mem_write(n.reg('x1'),struct.pack('<5Q',0x41e02000,context,0,1,71))
    else:u.mem_write(n.reg('x1'),struct.pack('<2Q',context,0))
    put(n,'state',5,4);u.reg_write(UC_ARM64_REG_X0,1);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
   if pc==n.symbols.get('xQueueGenericSend') or pc==0x41e02000:raise AssertionError('borrowed job completed or callback entered after revocation')
  n.u.hook_add(UC_HOOK_CODE,intercept);n.call(entry)
  assert n.stop=='tick' and events==['borrow'] and bytes(n.u.mem_read(context,25))==b'borrowed context retained'
  assert n.reg('sp')<0x41dff000,'borrowed frame unexpectedly unwound'
 case('parallel-borrowed-context-retained-without-completion',lambda:borrowed('parallel'))
 case('replay-borrowed-context-retained-without-completion',lambda:borrowed('replay'))

 print('PASS new supervised mimus native boundaries: '+str(len(old['cases'])))
if __name__=='__main__':main()
