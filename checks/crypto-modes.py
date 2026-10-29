#!/usr/bin/env python3
"""Focused application request/service vectors; existing CE arithmetic tests are reused."""
import argparse,ast,hashlib,hmac,importlib.util,json,pathlib,subprocess,types,shlex,shutil
from harness import SANITIZE,environment
p=argparse.ArgumentParser();p.add_argument('--output',type=pathlib.Path,required=True);p.add_argument('--resume',action='store_true');a=p.parse_args()
root=pathlib.Path(__file__).resolve().parents[1];a.output.mkdir(parents=True,exist_ok=a.resume)
spec=importlib.util.spec_from_file_location('request',root/'apps/crypto/request.py');request=importlib.util.module_from_spec(spec);spec.loader.exec_module(request)
# Reuse only the accepted model's table/vector/oracle helpers; do not run its suite.
names={'primes','k512','sha384','initial','plain','keys','cbc_iv','ctr_iv','nist','k3','p3','a4','iv6','gcm_vectors'}
tree=ast.parse((root/'checks/a733-ce-modes.py').read_text());nodes=[]
for node in tree.body:
 if isinstance(node,ast.FunctionDef) and node.name in ('root','openssl','ghash','gcm'):nodes.append(node)
 elif isinstance(node,ast.Assign) and any(isinstance(t,ast.Name) and t.id in names for t in node.targets):nodes.append(node)
ref={'subprocess':subprocess,'a':types.SimpleNamespace(openssl='/usr/bin/openssl')}
exec(compile(ast.Module(body=nodes,type_ignores=[]),'accepted-ce-mode-helpers','exec'),ref)
tables=a.output/'tables.txt';tables.write_text('k512 '+' '.join('%x'%v for v in ref['k512'])+'\n'+''.join('initial %u '%m+' '.join('%x'%v for v in values)+'\n' for m,values in ref['initial'].items()))
sources=['checks/crypto-modes.c','apps/crypto/run.c','apps/crypto/request.c','apps/crypto/vectors.c','drivers/storage/storage_digest.c','drivers/crypto/a733_ce.c','drivers/crypto/a733_ce_session.c','drivers/crypto/a733_ce_hash.c','drivers/crypto/a733_ce_cipher.c','drivers/clock/a733_clock_leases.c','runtime/device_memory_pool.c']
# Source path discovery is explicit because the pool implementation is shared.
commands=json.loads((a.output/'commands.json').read_text()) if a.resume else [];prior_commands=commands.copy();log=(a.output/'log.txt').read_text() if a.resume else ''
def run(command):
 global log
 command=list(map(str,command));commands.append(command)
 r=subprocess.run(command,cwd=root,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,env=environment());log+=r.stdout
 (a.output/'log.txt').write_text(log);(a.output/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
 if r.returncode:print(r.stdout);raise SystemExit(r.returncode)
objects=[];rebuilt=False
for i,source in enumerate(sources):
 obj=a.output/(str(i)+'.o');objects.append(obj)
 command=['/usr/bin/clang-21','-std=c11','-O1','-g','-ffunction-sections','-fdata-sections',*SANITIZE,'-Iinclude','-Iapps/crypto','-Ichecks','-MMD','-MF',a.output/(str(i)+'.d'),'-c',source,'-o',obj]
 dep=a.output/(str(i)+'.d')
 if a.resume and list(map(str,command)) in prior_commands and obj.is_file() and dep.is_file():
  dependencies=[root/name for name in shlex.split(dep.read_text().replace('\\\n',' ').split(':',1)[1])]
  if all(path.stat().st_mtime_ns<=obj.stat().st_mtime_ns for path in dependencies):continue
 rebuilt=True;run(command)
run(['/usr/bin/clang-21',*SANITIZE,'-Wl,--gc-sections',*objects,'-o',a.output/'model'])
cases=[]
def case(name,raw,expected,auth_failure=False):
 directory=a.output/name
 if a.resume and directory.is_dir():
  assert (directory/'request.bin').read_bytes()==raw and (directory/'expected.bin').read_bytes()==expected
  if not rebuilt and not auth_failure and (directory/'output.bin').is_file() and (directory/'output.bin').read_bytes()==expected:
   cases.append(name);return
  preserved=a.output/'prior'/hashlib.sha256((a.output/'model').read_bytes()).hexdigest()/name
  preserved.parent.mkdir(parents=True,exist_ok=True)
  if not preserved.exists():shutil.copytree(directory,preserved)
 else:
  directory.mkdir();(directory/'request.bin').write_bytes(raw);(directory/'expected.bin').write_bytes(expected)
 run([a.output/'model',tables,directory/'request.bin',directory/'expected.bin',directory/'output.bin','auth-failure' if auth_failure else 'success'])
 cases.append(name)
for method in request.HASHES:
 for kind in ('hash','hmac'):
  # SHA1 HMAC exercises long-key initialization; SHA512 exercises inner-pad
  # partial consumption of a maximum-size message.
  message=b'abc' if kind=='hash' else bytes(range(256))*8
  key=bytes(range(128)) if kind=='hmac' else b''
  expected=hmac.new(key,message,method).digest() if kind=='hmac' else hashlib.new(method,message).digest()
  case(kind+'-'+method,request.encode_extended(kind,1,message,method=method,key=key),expected)
for mode in ('ecb','cbc','ctr','gcm'):
 for width,key in ref['keys'].items():
  plain=ref['plain'];iv=b'' if mode=='ecb' else ref['ctr_iv'] if mode=='ctr' else ref['cbc_iv'] if mode=='cbc' else bytes(12)
  aad=b'authenticated application context' if mode=='gcm' else b''
  if mode=='gcm':cipher,tag=ref['gcm'](key,iv,aad,plain,16);cipher+=tag
  else:cipher=ref['openssl']('aes-%u-%s'%(width*8,mode),key,plain,iv or None)
  for direction,data,expected in [('encrypt',plain,cipher),('decrypt',cipher,plain)]:
   case('%s-%u-%s'%(mode,width*8,direction),request.encode_extended('cipher-'+direction,2,data,mode=mode,key=key,iv=iv,aad=aad),expected)
key=bytes(16);iv=bytes(12);ct,tag=ref['gcm'](key,iv,b'',b'',16)
case('gcm-empty-decrypt',request.encode_extended('cipher-decrypt',3,tag,mode='gcm',key=key,iv=iv),b'')
case('gcm-empty-encrypt',request.encode_extended('cipher-encrypt',4,b'',mode='gcm',key=key,iv=iv),tag)
bad=tag[:-1]+bytes([tag[-1]^1]);case('gcm-auth-failure',request.encode_extended('cipher-decrypt',5,bad,mode='gcm',key=key,iv=iv),b'',True)
# Legacy wire requests run the same application over unchanged services.
case('legacy-sha256',request.encode(1,6,b'abc'),hashlib.sha256(b'abc').digest())
case('legacy-aes',request.encode(2,7,ref['plain'][:16],ref['keys'][16]),ref['openssl']('aes-128-ecb',ref['keys'][16],ref['plain'][:16]))
(a.output/'results.json').write_text(json.dumps({'result':'PASS actual application/request/CE-service/controller over existing descriptor model; no hardware','cases':cases,'source_inputs':{name:hashlib.sha256((root/name).read_bytes()).hexdigest() for name in sources+['checks/a733-ce-modes.c','checks/a733-ce-modes.py','apps/crypto/request.py']}},indent=2)+'\n')
print(log)
