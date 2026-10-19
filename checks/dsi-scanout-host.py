#!/usr/bin/env python3
"""Check the real image marker and rejection of modeled physical DSI authority."""
import argparse,hashlib,importlib.util,json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
def main():
 p=argparse.ArgumentParser(description=__doc__)
 for name in ('image','session','output'):p.add_argument('--'+name,type=Path,required=True)
 a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False);prep=module('dsi_host_admission',ROOT/'apps/dsi-scanout/prepare.py');raw=a.session.read_bytes();image=a.image.read_bytes();ih=hashlib.sha256(image).hexdigest()
 prep.image_manifest_check(dict(target='sunstone-a733-dsi-scanout',execution_profile='dsi-single-core-supervised-v1',final_elf=ih),image,raw)
 # Common board admission is unchanged and has separate accepted evidence.
 # Replace only that boundary to reach the new DSI physical-fact gate. Every
 # model fact remains explicitly false; no success authority is fabricated.
 calls=[];cache={};prep.board.validate_admission=lambda *args:(calls.append(True) or cache)
 record=dict(schema='a733-dsi-supervised-entry-v1',identity='MODEL-ONLY',port='NO-DEVICE',reservation=100)
 for i,(key,required) in enumerate(prep.FACTS.items()):
  fact=dict(schema='a733-dsi-'+key+'-v1',identity='MODEL-ONLY',port='NO-DEVICE',image_sha256=ih,profile_values_sha256=prep.profile.value_digest(raw),reviewed=True,physical_access_performed=False,evidence=[],**{k:True for k in required})
  data=json.dumps(fact,sort_keys=True).encode();path=(a.output/(key+'-MODEL-ONLY.json')).resolve();path.write_bytes(data);cache[str(path)]=data;record[key]=dict(path=str(path),sha256=hashlib.sha256(data).hexdigest())
 # Bind the actual modeled fact bytes so refusal specifically reaches the
 # physical-access requirement rather than an unrelated digest mismatch.
 model=bytearray(raw)
 for i,key in enumerate(prep.FACTS):model[704+32*i:736+32*i]=bytes.fromhex(record[key]['sha256'])
 try:prep.validate_admission(record,ih,hashlib.sha256(model).hexdigest(),model)
 except ValueError as error:assert str(error)=='DSI physical record is not bound to actual selected inputs';refusal=str(error)
 else:raise AssertionError('Modeled physical authority accepted')
 assert calls==[True]
 (a.output/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,image_sha256=ih,actual_image_marker=True,model_authority_refused=refusal,scope='Actual image geometry/manifest validator and new DSI physical-fact gate. Unchanged common board-admission boundary modeled; no launcher or device access.'),indent=2)+'\n')
if __name__=='__main__':main()
