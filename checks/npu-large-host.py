#!/usr/bin/env python3
"""Production large-profile packaging and capture with scripted transport."""
import argparse,hashlib,importlib.util,json,struct,time
from pathlib import Path
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
def main():
 p=argparse.ArgumentParser(description=__doc__)
 for n in ('image','manifest','accepted','output'):p.add_argument('--'+n,type=Path,required=True)
 p.add_argument('--input',type=Path,action='append',required=True);a=p.parse_args();out=a.output;out.mkdir(parents=True,exist_ok=False)
 prep=module('large_prep',ROOT/'apps/npu-convolution/prepare-supervised.py');fixture=module('large_fixture',ROOT/'checks/npu-supervised-host.py');tool=module('large_tool',ROOT/'tools/check-a733-npu-supervised.py');host=module('large_host',ROOT/'apps/npu-runtime-large/host.py')
 raw=bytearray((Path.home()/'.cache/sunstoneos/evidence/npu-interrupt-service-20261019/host/bundle/session.bin').read_bytes());parts,address,build,layout=prep.image_data(a.image.read_bytes());raw[48:80]=build
 session,admission=fixture.admission_fixture(prep,a.image.read_bytes(),raw,layout,build,out)
 prep.prepare(a.image,a.manifest,session,admission,out/'bundle','/usr/bin/clang-21','/home/sunil/.cache/sunstoneos/host/usr/bin/ld.lld-21','/usr/lib/llvm-21/bin/llvm-objcopy');prepared=prep.validate_bundle(out/'bundle')
 assert prepared['execution_profile']=='npu-runtime-large-single-core-supervised-v1'
 model=next(v for v in tool.protocol.CATALOGUE if v['name']==prepared['model_name']);inputs=[v.read_bytes() for v in a.input];outputs=[(a.accepted/f'output-{i}.bin').read_bytes() for i in (1,2)]
 record={}
 class Device:
  pending=b'';reads=0;received=0;delivered=0;jobs=0;uploaded=bytearray()
  def write(self,ep,data,timeout):
   assert ep==2 and not self.pending
   magic,seq,op,length,epoch,generation,rid,us,count,flags,offset=struct.unpack_from('<4I4Q2IQ',data)
   assert (magic,length,epoch,generation,flags)==(host.MAGIC,48,prepared['epoch'],prepared['generation'],0)
   state=host.DONE;chunk=b'';total=0
   if op==host.BEGIN:self.received=self.delivered=0;self.uploaded=bytearray();state=host.UPLOADING
   elif op==host.UPLOAD:
    assert offset==self.received;self.uploaded.extend(data[64:]);self.received+=count;state=host.UPLOADING
   elif op==host.COMMIT:assert bytes(self.uploaded)==inputs[rid-1];self.jobs+=1
   elif op==host.READ:chunk=outputs[rid-1][offset:offset+512];total=len(outputs[rid-1])
   elif op==host.FINISH:assert self.delivered==len(outputs[rid-1])
   elif op==host.CLOSE:state=host.CLOSED
   else:raise AssertionError('Unexpected request')
   frame=bytearray(1024);struct.pack_into('<6Q',frame,0,host.FRAME,1024<<32|2,state,prepared['epoch'],prepared['generation'],prepared['lease']);frame[48:80]=bytes.fromhex(prepared['build_id']);struct.pack_into('<2Q',frame,80,rid,120000000);frame[96:128]=bytes.fromhex(model['model']['sha256'])
   if state in (host.DONE,host.CLOSED):
    struct.pack_into('<5Q',frame,128,24000000,24000024,0 if state==host.CLOSED else rid,self.jobs,self.jobs+1)
    if state==host.DONE:
     frame[168:200]=hashlib.sha256(inputs[rid-1]).digest();frame[200:232]=hashlib.sha256(outputs[rid-1]).digest();struct.pack_into('<3Q',frame,232,len(outputs[rid-1]),offset,len(chunk));frame[256:256+len(chunk)]=chunk
   struct.pack_into('<3Q',frame,776,522240,self.received,self.delivered);self.delivered+=len(chunk);frame[992:]=hashlib.sha256(frame[:992]).digest();self.pending=struct.pack('<4I',host.MAGIC,seq,0x80000034,1024)+frame
   return len(data)
  def read(self,ep,n,timeout):
   assert ep==0x81
   if self.reads:assert len(record['runtime_fragments'])==self.reads
   part=self.pending[:min(n,521)];self.pending=self.pending[len(part):];self.reads+=1;return part
 dest=out/'capture';dest.mkdir();device=Device()
 result=host.capture_device(device,lambda:None,time.monotonic()+120,prepared,dest,record,lambda:None,inputs,[],model,durable=tool.supervisor.durable_frame)
 assert result['jobs']==2 and result['oracle_match'] is None
 assert all((dest/f'output-{i:02}.bin').read_bytes()==output for i,output in enumerate(outputs,1))
 calls=[];bad=out/'wrong-shape.bin';bad.write_bytes(b'x')
 argv=['--bundle',str(out/'bundle'),'--fel','unused','--evidence',str(out/'unused'),'--database','unused','--claim','fixture','--owner','fixture','--supervised-manual-reconnect','--input',str(bad)]
 with patch.object(tool.supervisor,'run_session',lambda *a,**kw:calls.append(kw) or 0):
  try:tool.main(argv)
  except ValueError:pass
  else:raise AssertionError('Wrong shape accepted')
 assert not calls
 argv=argv[:-2]+sum((['--input',str(q)] for q in a.input),[])
 with patch.object(tool.supervisor,'run_session',lambda *a,**kw:calls.append(kw) or 0):assert tool.main(argv)==0
 assert calls[0]['bcd_device']==0x0105 and calls[0]['serial']=='sunstoneos-a733-npu-large'
 (out/'result.json').write_text(json.dumps(dict(result='pass',image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(),cases=['current image preparation and frozen bundle','two arbitrary inputs and copied outputs without mandatory oracle','durable fragments and chunks','healthy CLOSE','wrong shape refuses before transport','large USB identity'],scope='Production host code with scripted transport; output bytes reused from accepted actual-linked copying check, not physical inference.',physical_access=False),indent=2)+'\n');print('PASS large production host preparation/capture')
if __name__=='__main__':main()
