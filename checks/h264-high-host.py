#!/usr/bin/env python3
"""Exercise High host bundles and capture with native copied outputs and modeled USB."""
import argparse,hashlib,importlib.util,json,shutil,struct,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
def main():
 p=argparse.ArgumentParser(description=__doc__)
 for name in ('image','inputs','native','launcher','output'):p.add_argument('--'+name,type=Path,required=True)
 a=p.parse_args();a.output.mkdir(exist_ok=False);host=module('high_host',ROOT/'apps/h264/high.py');prepare=module('high_prepare',ROOT/'apps/h264/prepare.py')
 image_hash=hashlib.sha256(a.image.read_bytes()).hexdigest();native_result=json.loads((a.native/'result.json').read_text());assert native_result['passed'] and native_result['image_sha256']==image_hash;supervisor=module('high_capture_supervisor',ROOT/'tools/check-a733-smp-supervised.py');mf=a.output/'image-manifest.json';mf.write_text(json.dumps(dict(target='sunstone-a733-h264',execution_profile='h264-high-supervised-v1',final_elf=image_hash)))
 admission=a.output/'modeled-admission.json';admission.write_text(json.dumps(dict(reservation='modeled-no-physical-access')));prepare.validate_admission=lambda *unused,**kwargs:{};prepare.build_launcher=lambda out,*unused,**kwargs:shutil.copy2(a.launcher,out/'launch.bin')
 for mode in ('normal','qualification'):
  directory=a.inputs/'fullhd';out=a.output/(mode+'-bundle');bundle=prepare.prepare(a.image,mf,admission,directory/(mode+'-session.bin'),out,'/usr/bin/clang-21','/home/sunil/.cache/sunstoneos/host/usr/bin/ld.lld-21',directory/mode/'manifest.bin');assert len(bundle['_stream_frames'])==12
  modified=out/'frame-003.h264';original=modified.read_bytes();modified.write_bytes(original[:-1])
  try:prepare.validate_bundle(out)
  except (ValueError,RuntimeError):pass
  else:raise AssertionError('Changed sealed High AU admitted')
  modified.write_bytes(original)
 session=(a.inputs/'fullhd/qualification-session.bin').read_bytes();manifest=(a.inputs/'fullhd/qualification/manifest.bin').read_bytes();frames=host.validate_inputs(a.inputs/'fullhd/qualification',manifest);rows=host.descriptors(manifest);native=[(a.native/f'fullhd-qualification-frame{i}.bin').read_bytes() for i in range(12)];reports=[]
 for failed in (False,True):
  folder=a.output/('failed-B' if failed else 'nominal');folder.mkdir();record={};requests=bytearray();queue=[];uploaded=[0];commands=[];reads=[0]
  class Device:
   def write(self,ep,part,timeout):
    assert ep==2 and 0<timeout<=500;requests.extend(part)
    while len(requests)>=32:
     magic,version,index,aux,epoch,generation=struct.unpack_from('<4I2Q',requests);assert version==2 and (epoch,generation)==struct.unpack_from('<2Q',session,24)
     needed=32+(aux if magic==host.M else len(frames[index][0])+len(frames[index][1]) if magic==host.F else 32 if magic==host.A else 0)
     if len(requests)<needed:break
     packet=bytes(requests[:needed]);del requests[:needed];commands.append((magic,index))
     if magic==host.M:assert packet==host.manifest_upload(session,manifest)
     elif magic==host.F:assert index==uploaded[0] and packet==host.frame_upload(session,manifest,index,*frames[index]);uploaded[0]+=1
     elif magic==host.A:
      assert packet[32:]==native[index][96:128] and (folder/f'output-{index:03}.yu12').read_bytes()==native[index][256:]
      assert any(item['picture_index']==index for item in json.loads((folder/'record.json').read_text())['copied_h264_pictures'])
     elif magic==host.Q:
      ready=max(rows[next(i for i,d in enumerate(rows) if d['presentation']==index)]['ordinal']+1,struct.unpack_from('<I',native[index],192)[0]);raw=bytearray(native[index]);completed=min(uploaded[0],2) if failed and uploaded[0]>=3 else uploaded[0]
      if failed and index==1 and uploaded[0]>=3:raw=bytearray((a.native/'failed-B.bin').read_bytes())
      elif uploaded[0]<ready:raw=raw[:256];struct.pack_into('<2I',raw,8,0,0);raw[96:128]=bytes(32);struct.pack_into('<I',raw,212,0)
      struct.pack_into('<I',raw,192,completed)
      # Explicit transport/progress model; retained native pixels and metadata
      # other than the live decode-progress count remain unchanged.
      queue.append(b'');queue.extend(bytes(raw[i:i+2048]) for i in range(0,len(raw),2048));queue.append(b'')
     else:raise AssertionError('Unknown High command')
    return len(part)
   def read(self,ep,n,timeout):
    assert ep==0x81 and 0<n<=4096 and 0<timeout<=500 and queue;data=queue.pop(0);reads[0]+=1
    if len(data)>n:queue.insert(0,data[n:]);data=data[:n]
    return data
  def save():(folder/'record.json').write_text(json.dumps(record,indent=2)+'\n')
  def durable(path,data):supervisor.durable_frame(path,data)
  result=host.capture_device(Device(),lambda:None,time.monotonic()+30,session,folder,record,save,manifest,frames,durable)
  expected=1 if failed else 12;assert len(record['copied_h264_pictures'])==expected and result['status']==(2 if failed else 1);assert [i for magic,i in commands if magic==host.A]==list(range(expected));reports.append(dict(case=folder.name,copied_before_ACK=expected,reads=reads[0]))
 normal_session=(a.inputs/'fullhd/normal-session.bin').read_bytes();normal_manifest=(a.inputs/'fullhd/normal/manifest.bin').read_bytes();normal=host.decode((a.native/'fullhd-normal-frame0.bin').read_bytes(),normal_session,normal_manifest,0,None);assert normal['status']==1 and not normal['oracle_verified']
 (a.output/'result.json').write_text(json.dumps(dict(passed=True,image_sha256=image_hash,physical_access=False,cases=reports,normal=normal,scope='Actual High producer/bundle/capture; separately checked launcher reused. Native copied pixel frames retained; admission, USB fragments/ZLP and live completion counts modeled. Actual fsync-backed output persistence precedes ACK; record callback ordering is checked. Failed capture retains earlier output; manual failed-session QUERY can additionally retrieve completed reorder-private anchors.'),indent=2)+'\n');print('PASS High producer/bundles and presentation capture, durable-before-ACK, failed B and normal output')
if __name__=='__main__':main()
