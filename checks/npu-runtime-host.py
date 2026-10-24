#!/usr/bin/env python3
"""Current runtime image packaging and durable host capture; no hardware access."""
import argparse,hashlib,importlib.util,json,struct,time
from pathlib import Path
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
def module(n,p):
 s=importlib.util.spec_from_file_location(n,p);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
def run(image,manifest,accepted,out):
 out.mkdir(parents=True,exist_ok=False);prep=module('runtime_preparation',ROOT/'apps/npu-convolution/prepare-supervised.py');fixture=module('runtime_fixture',ROOT/'checks/npu-supervised-host.py');tool=module('runtime_tool',ROOT/'tools/check-a733-npu-supervised.py');host=module('runtime_protocol',ROOT/'apps/npu-runtime/host.py')
 # The reviewed 608-byte page, converted to the image's reduced record.
 images=module('runtime_images',ROOT/'checks/npu-image-checks.py');parts,address,build,layout=prep.image_data(image.read_bytes())
 raw=bytearray(images.npu_page((Path.home()/'.cache/sunstoneos/evidence/npu-interrupt-service-20261019/host/bundle/session.bin').read_bytes(),layout,prep.SINGLE_CORE_VERSION));raw[48:80]=build
 session,admission=fixture.admission_fixture(prep,image.read_bytes(),raw,layout,build,out)
 prepared=prep.prepare(image,manifest,session,admission,out/'bundle','/usr/bin/clang-21','/home/sunil/.cache/sunstoneos/host/usr/bin/ld.lld-21','/usr/lib/llvm-21/bin/llvm-objcopy');prepared=prep.validate_bundle(out/'bundle');assert prepared['execution_profile']=='npu-runtime-single-core-supervised-v1'
 model=next(v for v in tool.protocol.CATALOGUE if v['name']==prepared['model_name']);n=model['input']['bytes'];inputs=[bytes(i%256 for i in range(n)),bytes((i*7+31)%256 for i in range(n))]
 def derived(name,seq,rid,pending=False):
  source=(accepted/name).read_bytes();frame=bytearray(source);struct.pack_into('<I',frame,4,seq);raw=frame[16:];struct.pack_into('<3Q',raw,24,prepared['epoch'],prepared['generation'],prepared['lease']);raw[48:80]=bytes.fromhex(prepared['build_id'])
  if pending:struct.pack_into('<Q',raw,16,host.QUEUED);raw[128:752]=bytes(624)
  raw[992:]=hashlib.sha256(raw[:992]).digest();frame[16:]=raw;return bytes(frame)
 frames=[derived('frame-1.bin',1,1,True),derived('frame-1.bin',2,1),derived('frame-2.bin',3,2,True),derived('frame-2.bin',4,2),derived('closed-frame.bin',5,3,True),derived('closed-frame.bin',6,3)]
 class Device:
  def __init__(self,frames):self.frames=list(frames);self.pending=b'';self.reads=0
  def write(self,endpoint,data,timeout):
   assert endpoint==2 and not self.pending;self.pending=self.frames.pop(0);return len(data)
  def read(self,endpoint,n,timeout):
   assert endpoint==0x81;part=self.pending[:min(n,37)];self.pending=self.pending[len(part):];self.reads+=1
   if self.reads>1:assert len(record['runtime_fragments'])==self.reads-1
   return part
 dest=out/'capture';dest.mkdir();record={};device=Device(frames)
 result=host.capture_device(device,lambda:None,time.monotonic()+10,prepared,dest,record,lambda:None,inputs,[],model,durable=tool.supervisor.durable_frame)
 assert result['jobs']==2 and result['oracle_match'] is None and record['npu_result']=='observed-success'
 for i in (1,2):assert (dest/f'output-{i:02}.bin').read_bytes()==(accepted/f'output-{i}.bin').read_bytes()
 # A transport exception after a short result retains the first exact fragment.
 class Short:
  def write(self,*a,**kw):return len(a[1])
  def read(self,*a,**kw):
   if getattr(self,'sent',False):raise OSError('modeled transport loss')
   self.sent=True;return frames[0][:128]
 broken=out/'partial';broken.mkdir();partial={}
 try:host.capture_device(Short(),lambda:None,time.monotonic()+10,prepared,broken,partial,lambda:None,inputs,[],model,durable=tool.supervisor.durable_frame)
 except OSError:pass
 else:raise AssertionError('Transport loss swallowed')
 assert len(partial['runtime_fragments'])==1 and (broken/partial['runtime_fragments'][0]['path']).read_bytes()==frames[0][:128]
 calls=[];bad=out/'wrong-shape.bin';bad.write_bytes(b'x')
 argv=['--bundle',str(out/'bundle'),'--fel','unused','--evidence',str(out/'unused'),'--database','unused','--claim','fixture','--owner','fixture','--supervised-manual-reconnect','--input',str(bad)]
 with patch.object(tool.supervisor,'run_session',lambda *a,**kw:calls.append(kw) or 0):
  try:tool.main(argv)
  except ValueError:pass
  else:raise AssertionError('Wrong local shape accepted')
 assert not calls
 for i,data in enumerate(inputs):
  path=out/f'input-{i}.bin';path.write_bytes(data)
 argv=argv[:-2]+['--input',str(out/'input-0.bin'),'--input',str(out/'input-1.bin')]
 with patch.object(tool.supervisor,'run_session',lambda *a,**kw:calls.append(kw) or 0):assert tool.main(argv)==0
 assert calls[0]['bcd_device']==0x0104 and calls[0]['serial']=='sunstoneos-a733-npu-runtime'
 for offset in (4,40,64,96,1008):
  bad=bytearray(frames[1]);bad[offset]^=1
  try:host.decode(bad,2,prepared,1)
  except ValueError:pass
  else:raise AssertionError('Malformed runtime frame accepted')
 (out/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,image_sha256=hashlib.sha256(image.read_bytes()).hexdigest(),cases=['current runtime marker/profile preparation and frozen bundle','ordinary numerical output without oracle','two inputs/read results then healthy CLOSED','durable fragments before next read','partial transport evidence retained','wrong local shape refused before run_session','correct runtime USB identity selected','stale/corrupt frame refusals'],observation_derivation='Accepted actual linked601e image frames have only envelope sequence/session/build identities and declared pending state derived for scripted transport. No data/model/input/output digests changed. Runtime header session-cap branch alone changed in2892 image; reuse proof deadline-cap-result.json.'),indent=2)+'\n');print('Runtime production packaging/capture/identity and partial-evidence checks passed')
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__)
 for name in ('image','manifest','accepted','output'):p.add_argument('--'+name,type=Path,required=True)
 a=p.parse_args();run(a.image,a.manifest,a.accepted,a.output)
