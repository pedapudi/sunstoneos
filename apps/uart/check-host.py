#!/usr/bin/env python3
"""Check production UART preparation and copied-result capture without USB."""
import hashlib,importlib.util,json,struct,sys
from pathlib import Path
def module(name,path):
 spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
root=Path(__file__).resolve().parent
prep=module('uart_check_prepare',root/'prepare.py')
host=module('uart_check_capture',root/'capture.py')
def run(bundle,output):
 prepared=prep.validate_bundle(bundle);session=(bundle/'session.bin').read_bytes();prep.session_check(session)
 for offset,value in ((780,5),(774,1),(796,31)):
  bad=bytearray(session);bad[offset]=value
  try:prep.session_check(bad)
  except ValueError:pass
  else:raise AssertionError('Unsupported route/extent admitted')
 def frame(status,payload=b''):
  header=struct.pack('<4I',0x31545255,1,status,len(payload))+session[24:40]+hashlib.sha256(session).digest()
  return header+(hashlib.sha256(payload).digest() if payload else bytes(32))+bytes(32)+payload
 payload=struct.pack('<Q6I',100,115384,32,32,0,2,3)+session[800:832]*2+bytes(32)
 success=frame(1,payload);pending=frame(0)
 assert host.decoder.decode(success,session)['resources_released']
 bad=bytearray(payload);bad[64]^=1
 try:host.decoder.decode(frame(1,bad),session)
 except ValueError:pass
 else:raise AssertionError('Incorrect echo accepted')
 for malformed in (success[:-1],success+bytes(1),frame(2,payload)):
  try:host.decoder.decode(malformed,session)
  except ValueError:pass
  else:raise AssertionError('Malformed result accepted')
 output.mkdir(parents=True,exist_ok=False);record={};frames=iter([pending,success]);calls=[]
 def exchange(packet,timeout):calls.append((packet,timeout));return next(frames)
 result=host.capture(exchange,session,output,record,lambda:None,10,now=lambda:1,pause=lambda n:None)
 assert result['status']==1 and len(calls)==2 and (output/'output.bin').read_bytes()==payload
 assert record['copied_uart_output']['bytes']==128 and record['uart_result']=='observed-success'
 failures=output/'transport-failure';failures.mkdir();count=0
 def failed_exchange(packet,timeout):
  nonlocal count;count+=1;raise OSError('Modeled first transport failure')
 try:host.capture(failed_exchange,session,failures,{},lambda:None,10,now=lambda:1,pause=lambda n:None)
 except OSError:pass
 else:raise AssertionError('Transport failure hidden')
 assert count==1
 (output/'result.json').write_text(json.dumps(dict(physical_access=False,prepared_image=prepared['image_sha256'],launcher_sha256=prepared['files']['launch.bin'],production_bundle_validation=True,pin_and_extent_refusals=True,decoder_and_capture=True,stop_on_first_transport_failure=True),indent=2)+'\n')
 print('UART production bundle/launcher, pin/extent refusals, copied echo decoder, durable pending/success capture and first-transport-failure stop passed.')
if __name__=='__main__':run(Path(sys.argv[1]),Path(sys.argv[2]))
