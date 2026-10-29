#!/usr/bin/env python3
"""Exercise extended native-produced response frames, durable host capture and admission refusals."""
import argparse,hashlib,importlib.util,json,pathlib,struct,subprocess
from harness import refused,environment
p=argparse.ArgumentParser()
for name in ('native','vectors','output'):p.add_argument('--'+name,type=pathlib.Path,required=True)
a=p.parse_args();root=pathlib.Path(__file__).resolve().parents[1];a.output.mkdir(parents=True,exist_ok=False)
def load(name,path):
 spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
tool=load('crypto_modes_host',root/'tools/check-a733-crypto-supervised.py');request=tool.preparation.operation
cases={}
for directory in sorted(p for p in a.native.iterdir() if p.is_dir() and (p/'runtime-frame.bin').is_file()):
 raw=(directory/'operation.bin').read_bytes();session=(directory/'runtime-session.bin').read_bytes();frame=(directory/'runtime-frame.bin').read_bytes();admitted=request.decode(raw,session)
 result=tool.decoder.decode(frame,session,request=raw);assert result['request_version']==2
 method=next((k for k,v in request.HASHES.items() if v==admitted['method']),'sha256')
 mode=next(k for k,v in request.MODES.items() if v==admitted['mode'])
 assert request.encode_extended(admitted['operation'],admitted['id'],raw[48:48+admitted['input_bytes']],method=method,mode=mode,
  key=raw[2120:2120+admitted['key_bytes']],iv=raw[2248:2248+admitted['iv_bytes']],aad=raw[2312:2312+admitted['aad_bytes']],tag_bytes=admitted['tag_bytes'])==raw
 if directory.name=='gcm-auth-failure':
  assert result['status']==2 and len(frame)==144 and not result['bytes']
  assert not any(frame[112:144]);cases[directory.name]=True;continue
 assert result['bytes']==admitted['output_bytes']
 expected=(a.vectors/directory.name/'expected.bin').read_bytes();assert frame[144:]==expected
 out=a.output/directory.name;out.mkdir();record={};saved=[]
 def exchange(packet,timeout):
  assert packet==b'\1'+struct.pack('<I',len(raw))+raw and timeout>0;return frame
 result=tool.capture(exchange,session,out,record,lambda:saved.append(record.copy()),10,now=lambda:1,reference=None,request=raw)
 assert result['status']==1 and record['crypto_result']=='observed-success' and (out/'output.bin').read_bytes()==expected
 altered=bytearray(frame);struct.pack_into('<I',altered,12,admitted['output_bytes']+1)
 refused(lambda:tool.decoder.decode(altered,session,request=raw),'Crypto copied output differs')
 failed=bytearray(frame[:144]);struct.pack_into('<II',failed,8,2,0);failed[112:144]=bytes(32)
 assert tool.decoder.decode(failed,session,request=raw)['status']==2
 # A failed authenticated decryption cannot carry any output bytes or digest.
 failed.extend(b'\1');refused(lambda:tool.decoder.decode(failed,session,request=raw),'Non-success carries operation output')
 cases[directory.name]=True
# Mutations are re-committed by the C admission fixture, so shape refusals cannot
# accidentally pass because a digest mismatch was noticed first.
base=(a.vectors/'gcm-256-encrypt/request.bin').read_bytes()
mutations={}
for name,at,value in [('reserved',28,1),('operation',12,99),('input-overflow',24,2049),('hash-on-cipher',2096,5),('mode',2100,4),('key-width',2104,17),('iv-empty',2108,0),('iv-overflow',2108,65),('aad-overflow',2112,513),('tag-small',2116,11),('tag-large',2116,17)]:
 bad=bytearray(base);struct.pack_into('<I',bad,at,value);mutations[name]=bad
for name,at in [('legacy-key',32),('input-tail',48+64),('secret-tail',2120+32),('iv-tail',2248+12),('aad-tail',2312+len(b'authenticated application context'))]:
 bad=bytearray(base);bad[at]=1;mutations[name]=bad
mutations['truncated']=base[:-1]
commands=[]
for name,bad in mutations.items():
 try:request.decode(bad)
 except ValueError:pass
 else:raise AssertionError('Host accepted '+name)
 path=a.output/(name+'.bin');path.write_bytes(bad)
 command=[a.vectors/'model',a.vectors/'tables.txt',path,a.vectors/'gcm-256-encrypt/expected.bin',a.output/'unused.bin','parse-refused'];commands.append(list(map(str,command)))
 r=subprocess.run(list(map(str,command)),cwd=root,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,env=environment());assert not r.returncode,(name,r.stdout)
(a.output/'results.json').write_text(json.dumps({'result':'PASS extended native response to durable host output and re-committed malformed requests rejected by host and actual C parser','native_cases':cases,'refusals':list(mutations),'commands':commands},indent=2)+'\n')
print('PASS extended native frames to durable host capture, empty authenticated output and seventeen matching host/native admission refusals')
