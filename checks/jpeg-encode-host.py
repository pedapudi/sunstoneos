#!/usr/bin/env python3
"""Check actual private encoder packaging/capture with explicit physical-admission model."""
import argparse,hashlib,importlib.util,json,shutil,struct
from pathlib import Path
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
def module(n,p):
 s=importlib.util.spec_from_file_location(n,p);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m

def main():
 p=argparse.ArgumentParser(description=__doc__)
 for n in ('image','session','input','frame','manifest','launcher','output'):p.add_argument('--'+n,type=Path,required=True)
 a=p.parse_args();a.output.mkdir(exist_ok=False);host=module('encoder_host',ROOT/'tools/check-a733-jpeg-encode-supervised.py');prep=host.preparation
 raw=a.input.read_bytes();session=bytearray(a.session.read_bytes());frame=a.frame.read_bytes();prep.session_check(session);prep.embedded_reference(a.image.read_bytes(),session)
 # Reuse successful launcher bytes instead of rebuilding unchanged AArch32.
 def reuse_launcher(out,clang,lld):
  for source in a.launcher.iterdir():
   if source.is_file():shutil.copy2(source,out/source.name)
 record=a.output/'MODEL-NOT-PHYSICAL-admission.json';record.write_text(json.dumps({'reservation':'MODEL-NOT-PHYSICAL','physical_access':False}))
 bundle=a.output/'MODEL-NOT-PHYSICAL-bundle'
 with patch.object(prep,'validate_admission',lambda *x,**kw:{}),patch.object(prep,'build_launcher',reuse_launcher):
  made=prep.prepare(a.image,a.manifest,record,a.session,bundle,'/usr/bin/clang-21','/home/sunil/.cache/sunstoneos/host/usr/bin/ld.lld-21',a.input)
  frozen=host.freeze_bundle(bundle)
  assert frozen['_upload']==prep.upload_bytes(raw,None,(bundle/'session.bin').read_bytes())
 # Capture checks bind the already copied result to each produced session identity.
 current=(bundle/'session.bin').read_bytes();bound=bytearray(frame);bound[16:32]=current[24:40];frame=bytes(bound)
 events=[];state={};capture=a.output/'capture';capture.mkdir()
 def send_upload(data):assert data==frozen['_upload'];events.append('upload')
 def send_ack(data):
  assert (capture/'output.jpg').read_bytes()==frame[144:]
  assert state['jpeg_encode_result']=='observed-success' and state['durable_result']['status']==1
  assert data==host.decoder.ack(frame,current);events.append('ack')
 def save():events.append('save')
 result=host.capture(lambda packet,timeout:frame,current,capture,state,save,200.,now=lambda:100.,pause=lambda x:None,upload=frozen['_upload'],send_upload=send_upload,send_ack=send_ack)
 assert result['status']==1 and result['arithmetic_qualified'] is False and state['copied_output_ack_sent'] is True
 # Failure to persist output/record must not acknowledge a copied result.
 refused=[]
 for fail in ('output','record'):
  where=a.output/('failed-'+fail);where.mkdir();entry={};acks=[]
  original=host.supervisor.durable_frame
  def durable(path,data):
   if fail=='output' and path.name=='output.jpg':raise OSError('modeled disk full')
   original(path,data)
  def fail_save():
   if fail=='record' and entry.get('jpeg_encode_result')=='observed-success':raise OSError('modeled record failure')
  with patch.object(host.supervisor,'durable_frame',durable):
   try:host.capture(lambda *x:frame,current,where,entry,fail_save,200.,now=lambda:100.,upload=frozen['_upload'],send_upload=lambda x:None,send_ack=acks.append)
   except OSError:pass
   else:raise AssertionError('Persistence failure accepted')
  assert not acks;refused.append(fail)
 # Generation/hash/oracle/length mutations are rejected by actual producers.
 for mode in ('hash','generation','oracle','length'):
  bad=bytearray(current);data=raw
  if mode=='hash':bad[768]^=1
  if mode=='generation':bad[24:32]=bytes(8)
  if mode=='oracle':bad[800]=1
  if mode=='length':data=data[:-1]
  try:prep.upload_bytes(data,None,bad)
  except ValueError:pass
  else:raise AssertionError(mode)
 (a.output/'result.json').write_text(json.dumps(dict(passed=True,image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(),launcher_sha256=hashlib.sha256((a.launcher/'launch.bin').read_bytes()).hexdigest(),physical_access=False,events=events,persistence_refusals=refused,scope='Actual private session/upload/ELF geometry/request/bundle/freeze/capture/durable-output/ACK code. Physical admission alone modeled; accepted generated launcher reused. Copied JPEG is explicit modeled entropy, with no numerical qualification. No supervisor/device/queue access.'),indent=2)+'\n');print('PASS encoder private host bundle/capture/durable ACK')
if __name__=='__main__':main()
