#!/usr/bin/env python3
"""Validate DMA preparation and actual copied output with bounded fake USB."""
import argparse,hashlib,importlib.util,json,struct,time
from pathlib import Path
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
HERE=Path(__file__).resolve().parent
host=module('dma_host_capture',HERE/'capture.py');prep=host.preparation

def run(bundle,frame_path,out):
 prepared=prep.validate_bundle(bundle);session=(bundle/'session.bin').read_bytes();frame=frame_path.read_bytes()
 result=host.decoder.decode(frame,session);assert result['bytes']==2144 and result['dma_channels_closed']
 out.mkdir(parents=True,exist_ok=True)
 # Production capture receives transport fragments smaller than its header.
 class Device:
  def __init__(self):self.data=bytearray(frame);self.reads=0;self.writes=0
  def write(self,ep,data,timeout):assert ep==2 and data==b'\0';self.writes+=1;return 1
  def read(self,ep,count,timeout):
   assert ep==0x81;self.reads+=1;n=min(count,73 if self.reads<3 else 512,len(self.data));part=bytes(self.data[:n]);del self.data[:n];return part
 if not (out/'output.bin').exists():
  device=Device();record={};deadline=time.monotonic()+5
  observed=host.capture_device(device,lambda:None,deadline,session,out,record,lambda:None)
  assert observed==result and device.writes==1 and device.reads>4 and not device.data
  assert len(record['uart_fragments'])==device.reads
 assert (out/'output.bin').read_bytes()==frame[128:]
 fragments=sorted(out.glob('uart-001-fragment-*.bin'));assert len(fragments)>4 and b''.join(p.read_bytes() for p in fragments)==frame
 for field,value in ((792,0),(792,1025),(1908,3)):
  bad=bytearray(session);struct.pack_into('<I' if field==792 else '<H',bad,field,value)
  try:prep.session_check(bad)
  except ValueError:pass
  else:raise AssertionError('Malformed DMA session accepted')
 for offset in (4,12,32,64,128+32,128+56,128+60,128+64,128+80,128+96):
  bad=bytearray(frame)
  if offset==160:struct.pack_into('<Q',bad,offset,0)
  else:bad[offset]^=1
  if offset>=128:bad[64:96]=hashlib.sha256(bad[128:]).digest()
  try:host.decoder.decode(bad,session)
  except ValueError:pass
  else:raise AssertionError(('Malformed DMA output accepted',offset))
 # Stop at the first transport error, preserving the fragment already copied.
 failure=out/'failure';failure.mkdir()
 class Broken(Device):
  def read(self,ep,count,timeout):
   if self.reads==1:self.reads+=1;raise OSError('Modeled transport loss')
   return super().read(ep,count,timeout)
 broken=Broken();failed={}
 try:host.capture_device(broken,lambda:None,time.monotonic()+5,session,failure,failed,lambda:None)
 except OSError:pass
 else:raise AssertionError('Transport failure hidden')
 assert broken.reads==2 and broken.writes==1 and len(failed['uart_fragments'])==1
 (out/'result.json').write_text(json.dumps(dict(result='pass',physical_access=False,prepared_image=prepared['image_sha256'],launcher_sha256=prepared['files']['launch.bin'],production_preparation=True,actual_native_frame=True,fragment_capture=True,malformed_and_stale_refused=True,first_transport_failure_stops=True),indent=2)+'\n')
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__)
 for n in ('bundle','frame','output'):p.add_argument('--'+n,type=Path,required=True)
 a=p.parse_args();run(a.bundle,a.frame,a.output)
