from pathlib import Path
import importlib.util,json,struct,hashlib,types,subprocess
import argparse
parser=argparse.ArgumentParser(description="NPU single-core immutable packaging and capture; no physical facts.")
for name in ('image','manifest','session','reference','output','clang','lld','objcopy'):parser.add_argument('--'+name,type=Path,required=True)
args=parser.parse_args();r=Path(__file__).resolve().parents[1];out=args.output;out.mkdir(parents=True,exist_ok=False)
def mod(n,p):
 s=importlib.util.spec_from_file_location(n,p);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
prep=mod('prep',r/'apps/npu-convolution/prepare-supervised.py');fixture=mod('fixture',r/'checks/npu-supervised-host.py')
image=args.image;manifest=args.manifest;body=image.read_bytes();parts,address,build,layout=prep.image_data(body)
raw=bytearray(args.session.read_bytes());struct.pack_into('<Q',raw,8,2);raw[48:80]=build
session,admission=fixture.admission_fixture(prep,body,bytes(raw),layout,build,out)
profile=prep.image_manifest_check(json.loads(manifest.read_text()),body,session.read_bytes());assert profile=='npu-single-core-supervised-v1'
checks={}
for key,value in [('target','a733-npu-convolution-supervised'),('execution_profile','npu-supervised-v1'),('workload','other')]:
 m=json.loads(manifest.read_text());m[key]=value
 try:prep.image_manifest_check(m,body,session.read_bytes())
 except ValueError:checks[key+'-refused']=True
 else:raise AssertionError(key)
wrong=bytearray(session.read_bytes());struct.pack_into('<Q',wrong,8,1)
try:prep.image_manifest_check(json.loads(manifest.read_text()),body,wrong)
except ValueError:checks['session-version-refused']=True
else:raise AssertionError('session')
prepared=prep.prepare(image,manifest,session,admission,out/'bundle',args.clang,args.lld,args.objcopy)
checks['actual-image-package-and-frozen-validation']=True
host=mod('host',r/'tools/check-a733-npu-supervised.py');reference=args.reference.read_bytes();challenge=bytes(range(32))
def frame(sequence,status):
 p=bytearray(1024);complete=status==0;armed=1000
 struct.pack_into('<QII8Q',p,0,host.protocol.FRAME_MAGIC,2,1024,status,prepared['epoch'],prepared['generation'],prepared['lease'],2000 if complete else 0,3000 if complete else 0,armed+480000000,8 if complete else 0)
 p[80:112]=challenge;p[112:144]=bytes.fromhex(prepared['build_id'])
 struct.pack_into('<5Q',p,144,0x03600000,prepared['npu_evidence'],prepared['command_admission'],prepared['usb_evidence'],armed)
 artifacts=host.protocol.selected_artifacts(prepared);p[184:280]=b''.join(bytes.fromhex(h) for n,h in artifacts.values())
 if complete:
  p[280:280+len(reference)]=reference;p[792:824]=hashlib.sha256(reference).digest()
  struct.pack_into('<4Q',p,824,len(reference),0,2,7);p[856:888]=hashlib.sha256(reference).digest()
 p[992:]=hashlib.sha256(p[:992]).digest()
 return struct.pack('<4I',host.protocol.MESSAGE_MAGIC,sequence,0x80000033,1024)+p
frames=iter((frame(1,1),frame(2,0)));clock=fixture.Clock();capture=out/'capture';capture.mkdir();record={}
result=host.capture(lambda *a:next(frames),prepared,challenge,capture,record,lambda:None,110,now=clock.now,pause=clock.pause)
assert result['status']=='success' and (capture/'output.bin').read_bytes()==reference and len(record['npu_frames'])==2
checks['pending-and-two-copied-jobs-captured']=True
from unittest.mock import patch
calls=[]
with patch.object(host.supervisor,'run_session',lambda *a,**kw:calls.append(kw) or 0):
 assert host.main(['--bundle',str(out/'bundle'),'--fel','unused','--evidence',str(out/'capture-unused'),'--database','unused','--claim','fixture','--owner','fixture','--supervised-manual-reconnect'])==0
assert calls[0]['bcd_device']==0x0103
checks['host-matches-native-usb-device-version']=True
(out/'result.json').write_text(json.dumps({'passed':True,'physical_access':False,'fixture_not_physical':True,'execution_profile':profile,'cases':checks,'image_sha256':hashlib.sha256(body).hexdigest(),'launcher_sha256':prepared['launcher_sha256']},indent=2)+'\n');print('PASS',checks)
