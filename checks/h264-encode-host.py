#!/usr/bin/env python3
"""Exercise private packaging and two-picture durable capture without device access."""
import argparse,hashlib,importlib.util,json,shutil,struct
from pathlib import Path
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
def module(n,p):
 s=importlib.util.spec_from_file_location(n,p);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m

def main():
 p=argparse.ArgumentParser(description=__doc__)
 for n in ('image','session','input','pictures','manifest','launcher','output'):p.add_argument('--'+n,type=Path,required=True)
 a=p.parse_args();a.output.mkdir(exist_ok=False);host=module('h264_encode_host',ROOT/'tools/check-a733-h264-encode-supervised.py');prep=host.preparation
 raw=a.input.read_bytes();session=a.session.read_bytes();prep.session_check(session);prep.embedded_reference(a.image.read_bytes(),session)
 def reuse(out,clang,lld):
  for source in a.launcher.iterdir():
   if source.is_file():shutil.copy2(source,out/source.name)
 admission=a.output/'MODEL-NOT-PHYSICAL-admission.json';admission.write_text(json.dumps({'reservation':'MODEL-NOT-PHYSICAL','physical_access':False}))
 bundle=a.output/'MODEL-NOT-PHYSICAL-bundle'
 with patch.object(prep,'validate_admission',lambda *x,**kw:{}),patch.object(prep,'build_launcher',reuse):
  prep.prepare(a.image,a.manifest,admission,a.session,bundle,'/usr/bin/clang-21','/home/sunil/.cache/sunstoneos/host/usr/bin/ld.lld-21',a.input)
  frozen=host.freeze_bundle(bundle)
 current=(bundle/'session.bin').read_bytes();frames=[]
 for ordinal in range(2):
  bits=(a.pictures/('picture%d.h264'%ordinal)).read_bytes();header=bytearray(160)
  struct.pack_into('<4I2Q',header,0,0x45433248,1,1,len(bits),*struct.unpack_from('<2Q',current,24));header[32:64]=current[768:800];header[64:96]=hashlib.sha256(bits).digest()
  struct.pack_into('<11I',header,96,ordinal,2,0 if ordinal else 1,640,480,640,66,31,26,30,1);frames.append(bytes(header)+bits)
 results=[]
 for case in ('nominal','output-failure','record-failure','later-failure'):
  where=a.output/case;where.mkdir();state={};acks=[];events=[]
  def exchange(packet,timeout):
   ordinal=packet[1];frame=frames[ordinal]
   if case=='later-failure' and ordinal:
    frame=bytearray(frame[:160]);struct.pack_into('<II',frame,8,2,0);frame[64:96]=bytes(32);frame=bytes(frame)
   return frame
  def ack(data):
   ordinal=struct.unpack_from('<I',data,1)[0];assert (where/f'picture-{ordinal}.h264').read_bytes()==frames[ordinal][160:]
   assert state['h264_encode_outputs'][-1]['ordinal']==ordinal;acks.append(ordinal);events.append('ack%d'%ordinal)
  def save():
   events.append('save')
   if case=='record-failure' and len(state.get('h264_encode_outputs',[]))==2:raise OSError('modeled record persistence failure')
  original=host.supervisor.durable_frame
  def durable(path,data):
   if case=='output-failure' and path.name=='picture-1.h264':raise OSError('modeled output persistence failure')
   original(path,data)
  with patch.object(host.supervisor,'durable_frame',durable):
   try:result=host.capture(exchange,current,where,state,save,200.,now=lambda:100.,pause=lambda x:None,upload=frozen['_upload'],send_upload=lambda x:events.append('upload'),send_ack=ack)
   except OSError:assert case in ('output-failure','record-failure')
   else:
    assert result['status']==(2 if case=='later-failure' else 1)
    assert state['h264_encode_result']==('observed-failure' if case=='later-failure' else 'observed-success')
  assert acks==([0,1] if case=='nominal' else [0]);assert (where/'picture-0.h264').read_bytes()==frames[0][160:]
  results.append(dict(case=case,acked=acks,first_output_preserved=True,events=events))
 for mode in ('hash','generation','oracle','length'):
  bad=bytearray(current);data=raw
  if mode=='hash':bad[768]^=1
  if mode=='generation':bad[24:32]=bytes(8)
  if mode=='oracle':bad[800]=1
  if mode=='length':data=data[:-1]
  try:prep.upload_bytes(data,None,bad)
  except ValueError:pass
  else:raise AssertionError(mode)
 (a.output/'result.json').write_text(json.dumps(dict(passed=True,image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(),launcher_sha256=hashlib.sha256((a.launcher/'launch.bin').read_bytes()).hexdigest(),physical_access=False,cases=results,scope='Actual private producer/session/upload/ELF geometry/request/bundle/freeze/capture/output durability and two-picture ACK sequencing. Physical admission modeled. Reuses actual native copied outputs containing modeled entropy; no numerical encoding, supervisor/queue or physical USB execution.'),indent=2)+'\n');print('PASS private two-picture host preparation/durable ACK')
if __name__=='__main__':main()
