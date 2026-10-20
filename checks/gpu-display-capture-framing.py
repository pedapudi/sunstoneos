#!/usr/bin/env python3
"""Actual copied observation/parser/TX branches with reused terminal evidence.

Accepted provider/GPU/DE execution is not replayed. Restore only writable memory
from its hashed checkpoint after verifying unchanged writable symbol addresses;
keep final code/read-only data, rebind immutable session identity, and invoke
actual final functions from a fresh stack. Provider readiness/polling,
counter/kernel scheduling and TinyUSB effects are modeled. No physical device or USB access.
"""
import argparse,hashlib,json,struct
from pathlib import Path
from elftools.elf.elffile import ELFFile
import importlib.util
ROOT=Path(__file__).resolve().parents[1]
s=importlib.util.spec_from_file_location('capture_native',ROOT/'checks/gpu-display-capture-native.py');n=importlib.util.module_from_spec(s);s.loader.exec_module(n)
def writable(path):
 with path.open('rb') as f:
  e=ELFFile(f);spans=[(int(s['p_vaddr']),int(s['p_vaddr'])+int(s['p_memsz'])) for s in e.iter_segments() if s['p_type']=='PT_LOAD' and s['p_flags']&2]
  symbols={(x.name,int(x['st_value'])):int(x['st_size']) for x in e.get_section_by_name('.symtab').iter_symbols() if x['st_info']['type']=='STT_OBJECT' and any(a<=int(x['st_value'])<z for a,z in spans)}
  return spans,symbols

def main():
 p=argparse.ArgumentParser(description=__doc__)
 for k in ('image','old-image','session','contract','checkpoint','output'):p.add_argument('--'+k,type=Path,required=True)
 p.add_argument('--resume-tail',action='store_true');a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
 spans,symbols=writable(a.image);oldspans,oldsymbols=writable(a.old_image)
 assert spans==oldspans
 assert set(symbols)==set(oldsymbols)
 assert [(k,oldsymbols[k],v) for k,v in symbols.items() if v!=oldsymbols[k]]==[(next(k for k in symbols if k[0]=='sent'),4,1)]
 e=n.n.base.Image(a.image);raw=a.session.read_bytes();contract=a.contract.read_bytes();m=n.Machine(e,a.image,raw,contract)
 record=json.loads(a.checkpoint.read_text());assert record['image_sha256']==hashlib.sha256(a.old_image.read_bytes()).hexdigest()
 saved=[]
 for row in record['memory']:
  data=(a.checkpoint.parent/row['file']).read_bytes();assert hashlib.sha256(data).hexdigest()==row['sha256']
  at=row['address'];end=at+len(data)
  # Main checkpoint also contains old code; restore only writable segments.
  if at==0x41000000:
   for first,last in spans:
    lo=max(first,at);hi=min(last,end)
    if lo<hi:saved.append((lo,data[lo-at:hi-at]))
  elif at==0x43f80000:continue # no old task stack/context is resumed
  else:saved.append((at,data))
 from unicorn import UC_HOOK_CODE
 from unicorn.arm64_const import UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR
 for function,value in [('a733_clock_token_status',0),('a733_rail_status',0),('a733_usb_platform_poll',2)]:
  at=e.address(function);m.u.hook_add(UC_HOOK_CODE,lambda cpu,pc,size,unused,value=value:m.ret(value),begin=at,end=at)
 base_ticks=record['attributes']['ticks'];usbvars=n.n.variables(a.image,'/apps/gpu-display/usb.c')
 ob=m.app+m.af['observation'];ol=e.layouts['gpu_display_observation_mailbox'];request=0x43fa0000
 epoch,generation=struct.unpack_from('<2Q',raw,48);identity=dict(epoch=epoch,generation=generation,lease=struct.unpack_from('<Q',raw,64)[0],build_id=raw[72:104],session_sha256=hashlib.sha256(raw).digest())
 results=json.loads((a.output/'partial.json').read_text()) if a.resume_tail else []
 def reset():
  for at,data in saved:m.u.mem_write(at,data)
  m.u.mem_write(m.sealed+m.sf['bytes'],raw);m.u.mem_write(m.sealed+m.sf['digest'],hashlib.sha256(raw).digest());m.put(m.sealed+m.sf['ready'],1,4)
  m.u.mem_write(m.rc,contract);m.u.mem_write(m.app+m.af['session'],raw[24:568])
  m.scheduler=2;m.task=m.get(m.app+m.af['handles']+24);m.ticks=base_ticks;m.reason='';m.stopped=False
  m.usb_started=False;m.tusb_calls=0;m.phase='usb'
  m.rx=bytearray();m.tx=bytearray();m.tx_limit=m.tx_capacity=1024;m.write_calls=[];m.flushes=0
  m.put(ob,0,4);m.put(ob+ol['request_id'],0,4);m.put(ob+ol['requests'],0,4)
  for name in ('received','sent','transferred','tx_active','tx_zlp','exchange_deadline'):m.put(usbvars[name][0] if name in usbvars else e.address(name),0,8 if name=='exchange_deadline' else 4)
 def exchange():
  m.phase='usb';m.reason='';m.stopped=False
  if not m.usb_started:
   m.u.reg_write(UC_ARM64_REG_SP,0x43fc0000);m.u.reg_write(UC_ARM64_REG_LR,0x43fff000);at=e.address('gpu_display_usb_task');m.usb_started=True
  else:m.u.context_restore(m.usb_context);at=m.u.reg_read(UC_ARM64_REG_PC)
  m.u.emu_start(at,0,count=10000000);assert m.reason in ('delay','suspend'),(m.reason,hex(m.u.reg_read(UC_ARM64_REG_PC)))
  m.usb_context=m.u.context_save()
 def post(packet):
  m.rx.extend(packet)
  for _ in range(8):exchange()
  assert not m.rx and m.get(ob,4)==1
  m.task=m.get(m.app+m.af['handles']+8);m.call('gpu_display_observation_poll');m.task=m.get(m.app+m.af['handles']+24);assert m.get(ob,4)==2
 def finish(packet):
  for size in packet:m.call('tud_vendor_tx_cb',0,size)
  assert m.get(ob,4)==3
  m.call('tud_vendor_tx_cb',0,0);assert m.get(ob,4)==0
 def passed(name):
  results.append(name);(a.output/'partial.json').write_text(json.dumps(results,indent=2)+'\n');print('PASS '+name,flush=True)
 if not a.resume_tail:
  reset();m.put(m.app+m.af['state'],1,4);post(n.host.request(1,epoch,generation))
  m.tx_capacity=1023;exchange();assert not m.tx and not m.write_calls and not m.flushes and m.get(ob,4)==3
  m.tx_capacity=1024;exchange();assert len(m.tx)==1024 and m.write_calls==[1024] and m.flushes==1
  pending=bytes(m.tx);n.host.decode(pending,request_id=1,**identity);(a.output/'pending.bin').write_bytes(pending);finish([512,512]);passed('capacity gate, one exact write and high-speed terminal ZLP')
  m.put(m.app+m.af['state'],2,4);post(n.host.request(2,epoch,generation));exchange();healthy=bytes(m.tx[1024:]);assert len(healthy)==1024
  n.host.decode(healthy,request_id=2,**identity);(a.output/'healthy.bin').write_bytes(healthy)
  # Full-speed callbacks must retain the slot through all 16 packets and ZLP.
  speed=e.address('tud_speed_get')
  hook=m.u.hook_add(UC_HOOK_CODE,lambda cpu,at,size,unused:m.ret(1),begin=speed,end=speed)
  finish([64]*16);m.u.hook_del(hook);passed('next copied reply and full-speed terminal ZLP')
  reset();post(n.host.request(1,epoch,generation));m.tx_limit=17;exchange();assert m.reason=='suspend' and m.get(m.app+m.af['fatal'],4)==1 and m.get(ob,4)==3 and not m.flushes and m.write_calls==[1024];passed('unexpected short FIFO write faults and retains copied ownership')
  for label,packet in [('epoch',n.host.request(1,epoch+1,generation)),('generation',n.host.request(1,epoch,generation+1)),('id',n.host.request(2,epoch,generation))]:
   reset();before=bytes(m.u.mem_read(ob,1100));m.u.mem_write(request,packet);assert m.call('gpu_display_observation_request',request,32,m.get(m.app+m.af['deadline']))==0;assert before==bytes(m.u.mem_read(ob,1100));passed(label+' refusal preserves mailbox')
  reset();m.u.mem_write(request,n.host.request(1,epoch,generation));assert m.call('gpu_display_observation_request',request,31,m.get(m.app+m.af['deadline']))==0;passed('short request refused')
  reset();post(n.host.request(1,epoch,generation));frozen=bytes(m.u.mem_read(ob+ol['reply'],1024));m.put(m.app+m.af['stage'],99,4);m.task=m.get(m.app+m.af['handles']+8);m.call('gpu_display_observation_poll');assert frozen==bytes(m.u.mem_read(ob+ol['reply'],1024));m.task=m.get(m.app+m.af['handles']+24);m.rx.extend(n.host.request(2,epoch,generation));exchange();assert len(m.rx)==32;passed('published reply immutable and no receive during TX')
 reset();m.put(ob+ol['request_id'],199,4);m.put(ob+ol['requests'],199,4);m.u.mem_write(request,n.host.request(200,epoch,generation));assert m.call('gpu_display_observation_request',request,32,m.get(m.app+m.af['deadline']))==1;m.put(ob,0,4);m.u.mem_write(request,struct.pack('<4I2Q',n.host.MAGIC,1,201,32,epoch,generation));assert m.call('gpu_display_observation_request',request,32,m.get(m.app+m.af['deadline']))==0;passed('finite request bound')
 reset();m.ticks=m.get(m.app+m.af['deadline'])*24;exchange();assert m.reason=='suspend' and not m.get(m.app+m.af['fatal'],4);passed('healthy idle expiry parks without revocation')
 reset();m.ticks=m.get(m.app+m.af['deadline'])*24-24;exchange();assert m.reason=='suspend' and not m.get(m.app+m.af['fatal'],4);passed('idle expiry between owner checks stays healthy')
 for state in [1,2,3]:
  reset();m.put(ob,state,4);m.ticks=m.get(m.app+m.af['deadline'])*24;exchange();assert m.reason=='suspend' and m.get(m.app+m.af['fatal'],4)==1 and m.get(ob,4)==state;passed('deadline retains outstanding mailbox state '+str(state))
 reset();m.put(m.app+m.af['state'],1,4);m.ticks=m.get(m.app+m.af['deadline'])*24;exchange();assert m.reason=='suspend' and m.get(m.app+m.af['fatal'],4)==1;passed('unfinished display expiry faults')
 (a.output/'result.json').write_text(json.dumps(dict(passed=True,cases=results,scope=__doc__,image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(),old_image_sha256=record['image_sha256'],checkpoint_sha256=hashlib.sha256(a.checkpoint.read_bytes()).hexdigest(),session_sha256=hashlib.sha256(raw).hexdigest(),physical_access=False),indent=2)+'\n')
if __name__=='__main__':main()
