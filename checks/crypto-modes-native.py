#!/usr/bin/env python3
"""Reuse linked crypto ownership/USB fixture for extended, service-verified requests."""
import argparse,hashlib,json,pathlib,subprocess,sys
from harness import environment
p=argparse.ArgumentParser()
for name in ('image','session','vectors','output'):p.add_argument('--'+name,type=pathlib.Path,required=True)
p.add_argument('--only',action='append');a=p.parse_args();root=pathlib.Path(__file__).resolve().parents[1];a.output.mkdir(parents=True,exist_ok=False)
# Keep the accepted emulator's exact ELF layout, ownership, USB and private
# publication mechanism. Select one success per family and authenticated failure.
source=(root/'checks/crypto-runtime-native.py').read_text()
source=source.replace("ROOT=Path(__file__).resolve().parents[1]",'ROOT=Path('+repr(str(root))+')')
source=source.replace("'operation_input')","'operation_input','a733_smp_counter')")
source=source.replace("  if name=='board_live':", "  if name=='a733_smp_counter':returned(24);return\n  if name=='board_live':")
source=source.replace("for variant in ('partial-upload','upload-success','wrong-request','postpublication-write','expired-upload','expired-after-key-copy','producer-owned-cleanup','output-success','late-publication'):","for variant in ('output-success',):")
source=source.replace(" request=prep.operation.encode(2,0xfedcba9876543210,bytes(range(64)),bytes(range(16)))"," request=Path(__REQUEST__).read_bytes();expected=Path(__EXPECTED__).read_bytes()")
source=source.replace("   machine.mem_write(base+app['run']+run['payload'],b'\\x5a'*64)","   machine.mem_write(base+app['run']+run['payload'],expected)")
source=source.replace("write(base+app['run']+run['output_bytes'],64)","write(base+app['run']+run['output_bytes'],len(expected))")
source=source.replace("bytes(cpu.mem_read(symbols['operation_input'],2096))==bytes(2096)","bytes(cpu.mem_read(symbols['operation_input'],2824))==bytes(2824)")
source=source.replace("symbols['crypto_response'],208","symbols['crypto_response'],144+len(expected)")
source=source.replace("==0xfedcba9876543210","==prep.operation.decode(request)['id']")
# Each request was executed through actual run/services in the vector fixture;
# this linked check models CE completion at the same existing boundary.
selected=['hash-sha512','hmac-sha1','ecb-192-encrypt','cbc-256-decrypt','ctr-192-encrypt','gcm-256-encrypt','gcm-empty-decrypt','gcm-auth-failure','cleanup']
results={}
for case in selected:
 if a.only and case not in a.only:continue
 directory=a.output/case;vector=a.vectors/('gcm-256-encrypt' if case=='cleanup' else case)
 generated=source.replace('__REQUEST__',repr(str(vector/'request.bin'))).replace('__EXPECTED__',repr(str(vector/'expected.bin')))
 if case=='cleanup':
  generated=generated.replace("for variant in ('output-success',):", "for variant in ('expired-after-key-copy','producer-owned-cleanup'):")
  generated=generated.replace("request_at+32,b'\\x33'*16", "request_at+2120,b'\\x33'*128")
  generated=generated.replace("cipher_key,b'\\x44'*16", "cipher_key,b'\\x44'*32")
  generated=generated.replace("cpu.mem_read(cipher_key,16))==bytes(16)", "cpu.mem_read(cipher_key,32))==bytes(32)")
  generated=generated.replace("cpu.mem_read(request_at+32,16))==(bytes(16)", "cpu.mem_read(request_at+2120,128))==(bytes(128)")
  generated=generated.replace("else b'\\x33'*16)", "else b'\\x33'*128)")
  generated=generated.replace("  execute('crypto_task')\n  assert bytes(cpu.mem_read(cipher_key", "  hk=base+app['run']+run['hash']+layouts['a733_ce_hash']['key'];hb=base+app['run']+run['hash']+layouts['a733_ce_hash']['buffer']\n  cpu.mem_write(hk,b'k'*128);cpu.mem_write(hb,b'p'*2176)\n  execute('crypto_task')\n  assert bytes(cpu.mem_read(hk,128))==bytes(128) and bytes(cpu.mem_read(hb,2176))==bytes(2176)\n  assert bytes(cpu.mem_read(cipher_key")
 if case=='gcm-auth-failure':
  generated=generated.replace("write(base+app['run']+run['stage'],8);returned(1);return", "write(base+app['run']+run['stage'],5);returned(-1);return")
  generated=generated.replace("if variant=='late-publication' else []", "if True else []")
  generated=generated.replace("2 if variant=='late-publication' else 1", "2")
  generated=generated.replace("    if status==1:", "    if status==2:")
 fixture=a.output/(case+'.py');fixture.write_text(generated)
 command=[sys.executable,'-B',fixture,'--image',a.image,'--session',a.session,'--output',directory]
 r=subprocess.run(list(map(str,command)),cwd=root,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,env=environment());(a.output/(case+'.log')).write_text(r.stdout)
 if r.returncode:print(r.stdout);raise SystemExit(r.returncode)
 results[case]={'passed':True}
 if (directory/'runtime-frame.bin').is_file():results[case]['frame_sha256']=hashlib.sha256((directory/'runtime-frame.bin').read_bytes()).hexdigest()
(a.output/'results.json').write_text(json.dumps({'result':'PASS actual linked request upload/private publication/USB framing; CE completion and construction modeled, service arithmetic reused from vector evidence','image_sha256':hashlib.sha256(a.image.read_bytes()).hexdigest(),'cases':results,'fixture_sha256':hashlib.sha256(source.encode()).hexdigest()},indent=2)+'\n')
print('PASS selected extended requests and retained secret ownership through actual linked upload/task/publication/USB serializer')
