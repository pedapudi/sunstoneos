#!/usr/bin/env python3
"""Selected new host admission/copy cases; all devices and transport are modeled."""
import argparse,hashlib,importlib.util,json,struct,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--evidence',type=Path,required=True);a=p.parse_args();out=a.evidence
 prep=module('pcie_test_host_prepare',ROOT/'apps/pcie-test/prepare.py');observe=module('pcie_test_host_observe',ROOT/'apps/pcie-test/observe.py');capture=module('pcie_test_host_capture',ROOT/'apps/pcie-test/capture.py')
 raw=(out/'fixture/session-template.bin').read_bytes();record=json.loads((out/'fixture/admission.json').read_text());ih=record['image_sha256']
 prep.validate_admission(record,ih,hashlib.sha256(raw).hexdigest(),raw)
 results={'nominal_full_proof':True}
 path=record['endpoint_test']['path'];original=json.loads(Path(path).read_text())
 for name,key,value,message in [('ordering','completion_before_outbound_unmap',False,'Endpoint DMA/controller ordering proof is incomplete.'),('private-dma','dma_private',True,'Endpoint DMA/controller ordering proof is incomplete.'),('wrong-rx','rx_dma_memcpy',False,'Endpoint DMA/controller ordering proof is incomplete.'),('configuration','dma_configuration_sha256','00'*32,'Controller, DMA source and actual configuration bytes must be retained.')]:
  proof={**original,key:value};blob=json.dumps(proof,sort_keys=True).encode();identity=hashlib.sha256(blob).hexdigest();changed=bytearray(raw);changed[808:840]=bytes.fromhex(identity)
  admission={**record,'endpoint_test':{'path':path,'sha256':identity},'session_template_sha256':hashlib.sha256(changed).hexdigest()}
  try:prep.validate_admission(admission,ih,admission['session_template_sha256'],changed,{path:blob})
  except (ValueError,RuntimeError) as error:assert str(error)==message,(name,str(error))
  else:raise AssertionError(name)
  results[name+'_refused']=True
 # The actual linked producer's copied payload supplies nominal wire bytes.
 frame=(out/'native-frame.bin').read_bytes();decoded=observe.decode(frame,raw);assert decoded['copied_verified']
 for name,offset,value,message in [('dma-flag',12,0,'DMA completion contract differs'),('msi-count',28,0,'DMA completion contract differs'),('source-alias',48,struct.unpack_from('<Q',frame,128+40)[0],'DMA extent geometry differs'),('data',128,0,'Copied DMA bytes differ from original expected pattern')]:
  altered=bytearray(frame);size=8 if name=='source-alias' else 4
  struct.pack_into('<Q' if size==8 else '<I',altered,128+offset,value)
  altered[64:96]=hashlib.sha256(altered[128:]).digest()
  try:observe.decode(altered,raw)
  except ValueError as error:assert str(error)==message,(name,str(error))
  else:raise AssertionError(name)
  results[name+'_refused']=True
 pending=bytearray(frame[:128]);struct.pack_into('<2I',pending,8,0,0);pending[64:96]=bytes(32)
 replies=iter([bytes(pending),frame]);saved=[];model=out/'host-capture';model.mkdir(exist_ok=True);state={}
 result=capture.capture(lambda packet,timeout:next(replies),raw,model,state,lambda:saved.append(dict(state)),time.monotonic()+10,pause=lambda n:None)
 assert result['status']==1 and state['pcie_result']=='observed-success' and (model/'output.bin').read_bytes()==frame[128:]
 calls=0
 def lost(packet,timeout):
  nonlocal calls
  calls+=1;raise OSError('Modeled USB loss')
 try:capture.capture(lost,raw,model,{},lambda:None,time.monotonic()+10,pause=lambda n:None)
 except OSError:pass
 else:raise AssertionError('Transport loss hidden')
 assert calls==1;results.update(durable_capture=True,transport_no_retry=True)
 manifest=out/'build/pcie-test-application/manifest.json'
 bundle=prep.prepare(out/'build/sunstone-a733-pcie-test.elf',manifest,out/'fixture/admission.json',out/'fixture/session-template.bin',out/'bundle',Path('/usr/bin/clang-21'),Path('/home/sunil/.cache/sunstoneos/host/usr/bin/ld.lld-21'))
 assert bundle['physical_access'] is False;prep.validate_bundle(out/'bundle');results['actual_launcher_build_and_frozen_bundle']=True
 results['physical_access']=False;(out/'host-results.json').write_text(json.dumps(results,indent=2)+'\n');print(json.dumps(results))
if __name__=='__main__':main()
