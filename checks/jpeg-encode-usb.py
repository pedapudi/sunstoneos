#!/usr/bin/env python3
"""Resume copied native output through actual application publication and private USB ACK."""
import argparse,gzip,hashlib,importlib.util,json,pickle,struct
from pathlib import Path
from unicorn import UC_HOOK_CODE
ROOT=Path(__file__).resolve().parents[1]
def module(n,p):
 s=importlib.util.spec_from_file_location(n,p);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m

def main():
 p=argparse.ArgumentParser(description=__doc__)
 for n in ('image','session','checkpoint','output'):p.add_argument('--'+n,type=Path,required=True)
 p.add_argument('--case',action='append',choices=['nominal','wrong-generation','wrong-digest','early-ack','duplicate-ack'])
 a=p.parse_args();a.output.mkdir(exist_ok=False);native=module('encoder_native',ROOT/'checks/jpeg-encode-native.py');im=native.Image(a.image);state=pickle.loads(gzip.decompress(a.checkpoint.read_bytes()));session=a.session.read_bytes()
 assert state['image']==hashlib.sha256(a.image.read_bytes()).hexdigest() and state['session']==hashlib.sha256(session).hexdigest()
 observe=module('encoder_observe',ROOT/'apps/jpeg-encode/observe.py');reports=[]
 for case in (a.case or ['nominal','wrong-generation','wrong-digest','early-ack','duplicate-ack']):
  m=native.Machine(im,session)
  for at,data in state['memory']:m.u.mem_write(at,data)
  m.now=state['now'];m.put(m.base+m.a['stage'],1)
  shared=im.address('providers')+im.layouts['board_provider']['shared'];m.put(shared+im.layouts['a733_native_usb']['pmic_ready'],1)
  assert m.call('jpeg_encode_task',0)==1 or m.get(m.base+m.a['result'],4)==1
  assert m.get(m.base+m.a['result'],4)==1
  failed=[]
  def failure(u,at,size,unused):failed.append(True);m.done=True;u.emu_stop()
  m.u.hook_add(UC_HOOK_CODE,failure,begin=im.address('board_fail'),end=im.address('board_fail'))
  payload=m.get(m.base+m.a['payload']);n=m.get(m.base+m.a['jpeg_bytes']);before=bytes(m.u.mem_read(payload,n))
  m.incoming.extend(b'\0');m.call('board_usb0_task',0)
  for i in range(100):
   previous=len(m.tx);m.call('board_usb0_task',0);accepted=len(m.tx)-previous
   if accepted:
    if case=='early-ack' and len(m.tx)==144+n:break
    m.call('tud_vendor_tx_cb',0,accepted)
   if len(m.tx)==144+n:break
  assert len(m.tx)==144+n and observe.decode(bytes(m.tx),session)['status']==1
  packet=bytearray(observe.ack(bytes(m.tx),session))
  if case=='wrong-generation':packet[1]^=1
  if case=='wrong-digest':packet[-1]^=1
  m.incoming.extend(packet)
  if case=='early-ack':
   for _ in range(3):m.call('board_usb0_task',0)
   assert len(m.incoming)==41 and not failed and not m.get(im.address('acked'),4)
   assert bytes(m.u.mem_read(payload,n))==before
   reports.append(dict(case=case,bytes=len(m.tx),ack_held_by_transfer_backpressure=True,copied_output_preserved=True));continue
  while m.incoming and not failed:m.call('board_usb0_task',0)
  if case=='duplicate-ack' and not failed:
   m.incoming.extend(packet);m.call('board_usb0_task',0)
  assert bool(failed)==(case!='nominal'),case
  assert bytes(m.u.mem_read(payload,n))==before
  if case=='nominal':
   (a.output/'frame.bin').write_bytes(bytes(m.tx));(a.output/'ack.bin').write_bytes(packet)
  reports.append(dict(case=case,bytes=len(m.tx),failed=bool(failed),copied_output_preserved=True))
  (a.output/(case+'-result.json')).write_text(json.dumps(reports[-1],indent=2)+'\n')
 (a.output/'result.json').write_text(json.dumps(dict(passed=True,image_sha256=state['image'],checkpoint_sha256=hashlib.sha256(a.checkpoint.read_bytes()).hexdigest(),physical_access=False,cases=reports,scope='Actual compiled application COPIED publication, bounded31-byte USB FIFO acceptance, serialization, transferred-byte backpressure and generation/digest ACK/refusal. Reuses completed encoding checkpoint. USB/controller transfer completion, readiness/lease/translation attributes modeled; no encode replay or physical USB.'),indent=2)+'\n');print('PASS actual copied-output publication/USB backpressure/ACK')
if __name__=='__main__':main()
