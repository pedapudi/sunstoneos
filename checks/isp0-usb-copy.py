#!/usr/bin/env python3
"""Run linked ISP0 response/copy/ACK gates; TinyUSB calls and completions are modeled.

This check does not exercise the TinyUSB FIFO or DCD implementation. Its bounded
4096-byte FIFO supplies explicit callback effects to the actual ISP0 serializer.
"""
import argparse,gzip,hashlib,importlib.util,json,pickle,struct
from pathlib import Path
from elftools.elf.elffile import ELFFile
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('isp0_native',ROOT/'checks/isp0-native.py');native=importlib.util.module_from_spec(spec);spec.loader.exec_module(native)
class Transport(native.Machine):
 def __init__(self,*a):
  super().__init__(*a);self.fifo=bytearray();self.wire=bytearray();self.speed=1;self.revoke_write=False;self.fail_calls=0;self.write_calls=0
 def code(self,u,pc,size,unused):
  name=self.hooks.get(pc);x=self.args()
  if name=='board_fail':self.fail_calls+=1
  if name=='tud_vendor_n_write':
   self.write_calls+=1
   n=min(x[2],4096-len(self.fifo));self.fifo.extend(u.mem_read(x[1],n))
   if self.revoke_write:self.failed=True
   self.ret(n);return
  if name=='tud_speed_get':self.ret(self.speed);return
  super().code(u,pc,size,unused)
 def exchange(self):self.call('board_usb0_task',0)
 def symbols(self,path):
  wanted={'response_bytes','delivered','acked','terminal_zlp'};out={}
  with path.open('rb') as f:
   for sym in ELFFile(f).get_section_by_name('.symtab').iter_symbols():
    if sym.name in wanted and sym['st_info']['type']=='STT_OBJECT':
     assert sym.name not in out;out[sym.name]=sym['st_value']
  assert out.keys()==wanted;self.vars=out
 def value(self,name):return self.get(self.vars[name],8 if name=='response_bytes' else 4)
 def drain(self):
  for _ in range(2000):
   if self.fifo:
    n=min(len(self.fifo),512);self.wire.extend(self.fifo[:n]);del self.fifo[:n];self.call('tud_vendor_tx_cb',0,n)
   elif self.value('terminal_zlp'):self.call('tud_vendor_tx_cb',0,0)
   elif not self.value('response_bytes'):return
   else:self.exchange()
  raise AssertionError('Unbounded response callbacks')
def main():
 p=argparse.ArgumentParser(description=__doc__)
 for n in ('image','session','contract','input','checkpoint','output'):p.add_argument('--'+n,type=Path,required=True)
 a=p.parse_args();a.output.mkdir(exist_ok=False);im=native.helper.Image(a.image);state=pickle.loads(gzip.decompress(a.checkpoint.read_bytes()));results={}
 def fresh(speed=1):
  m=Transport(im,a.session.read_bytes(),a.contract.read_bytes(),a.input.read_bytes());m.image_path=a.image;m.output=a.output;m.restore(state);m.symbols(a.image);m.speed=speed
  assert m.get(m.base+m.f['result'],4)==1 and m.get(m.base+m.f['copied'])==614400
  return m
 for label,speed in [('full',1),('high',3)]:
  m=fresh(speed);payload=bytes(m.u.mem_read(m.get(m.base+m.f['payload']),614400))
  # Changing device output after publication must not change the owned copy.
  m.u.mem_write(m.memory(2),bytes(307200));m.u.mem_write(m.memory(3),bytes(307200));m.incoming.extend(b'\0');m.exchange();m.exchange();m.drain()
  wire=bytes(m.wire);assert len(wire)==614592 and wire[192:]==payload and m.value('delivered')==1
  assert struct.unpack_from('<4I',wire)==(0x30495053,1,1,614400)
  assert wire[64:96]==hashlib.sha256(a.session.read_bytes()).digest() and wire[96:128]==hashlib.sha256(payload).digest()
  m.incoming.extend(b'\1'+a.session.read_bytes()[32:40]+wire[96:128]);m.exchange();m.exchange();assert m.value('acked')==1
  m.wire.clear();m.incoming.extend(b'\0');m.exchange();m.exchange();m.drain();receipt=bytes(m.wire)
  assert len(receipt)==192 and struct.unpack_from('<2I',receipt,8)==(3,0) and receipt[16:]==wire[16:192]
  assert all(row['state']==2 for row in m.rows()['uncached_pool']);(a.output/(label+'-result.bin')).write_bytes(wire);(a.output/(label+'-receipt.bin')).write_bytes(receipt)
  results[label]=dict(passed=True,bytes=len(wire),owned_copy=True,ack_receipt=True)
 # Completed full-size FS data still needs its explicit terminal ZLP callback.
 m=fresh();m.incoming.extend(b'\0');m.exchange();m.exchange()
 while m.fifo:
  n=min(len(m.fifo),512);m.wire.extend(m.fifo[:n]);del m.fifo[:n];m.call('tud_vendor_tx_cb',0,n)
 assert m.value('terminal_zlp')==1 and m.value('delivered')==0
 m.incoming.extend(b'\1');m.exchange();assert m.value('acked')==0 and m.incoming==b'\1'
 m.call('tud_vendor_tx_cb',0,0);assert m.value('delivered')==1;results['terminal-gate']=dict(passed=True)
 for label in ('result-revoked','write-revoked'):
  m=fresh();m.incoming.extend(b'\0');m.exchange()
  if label=='write-revoked':m.revoke_write=True;m.exchange()
  else:
   m.exchange();m.put(m.base+m.f['result'],2);n=min(len(m.fifo),512);del m.fifo[:n];m.call('tud_vendor_tx_cb',0,n)
  assert m.failed and m.fail_calls==1 and m.value('delivered')==0 and m.value('acked')==0
  if label=='write-revoked':assert m.write_calls==1 and len(m.fifo)==512
  assert all(row['state']==2 for row in m.rows()['uncached_pool']);m.checkpoint(a.output/(label+'.pkl.gz'));results[label]=dict(passed=True,resources_retained=True)
 (a.output/'result.json').write_text(json.dumps(dict(status='pass',cases=results,image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(),physical_access=False,scope=__doc__),indent=2)+'\n')
if __name__=='__main__':main()
