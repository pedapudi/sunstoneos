#!/usr/bin/env python3
"""Compare fixed SH/NT native assembly to hash-preserved SDK preparation."""
import argparse,ctypes as c,hashlib,importlib.util,json,struct
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
model=module('sh_nt_model',ROOT/'checks/nbg-model.py');assembly=module('sh_nt_plan',ROOT/'checks/nbg-assembly.py');reference=module('sh_nt_reference',ROOT/'checks/nbg-loader-reference.py')
def main():
 p=argparse.ArgumentParser(description=__doc__)
 for n in ('model','native','capture','output'):p.add_argument('--'+n,type=Path,required=True)
 a=p.parse_args();data=a.model.read_bytes();sha=hashlib.sha256(data).hexdigest()
 assert sha=='327b9dbcaa3cf1b387a7f21e9bd4fef132237e55bb7108b6483a42c54462de69'
 capture=model.Capture(a.capture,reference.SDK_SHA,sha,require_comparison=False)
 n=model.Native(a.native,data);lib=n.lib
 lib.nbg_sh_nt_requirements.argtypes=[c.POINTER(model.View),c.POINTER(assembly.Plan)]
 lib.nbg_sh_nt_assemble.argtypes=[c.POINTER(model.View),c.POINTER(assembly.Buffer),c.POINTER(assembly.Plan)]
 plan=assembly.Plan();actual=assembly.Plan()
 assert lib.nbg_sh_nt_requirements(c.byref(n.view),c.byref(plan))==1
 devices=[0xe0000000,0xe0002000,0xe0004000,0xe0105000,0xe0186000,0xe0207000]
 buffers=(assembly.Buffer*6)();storage=[]
 for i,r in enumerate(plan.buffers):
  raw=c.create_string_buffer(r.allocation_bytes+12288);c.memset(c.addressof(raw),0xa5,c.sizeof(raw));cpu=(c.addressof(raw)+8191)&-4096;storage.append(raw);buffers[i]=assembly.Buffer(cpu,r.allocation_bytes,devices[i])
 assert lib.nbg_sh_nt_assemble(c.byref(n.view),buffers,c.byref(actual))==1 and bytes(plan)==bytes(actual)
 videos=[v for v in capture.allocations if v['video']]
 assert [v['bytes'] for v in videos]==[576,512,1047808,712,522304,262208]
 expected=[bytearray(capture.mem_read(v['cpu'],v['bytes'])) for v in videos]
 for dest,slot,offset in [(236,1,0),(28,4,0),(124,2,256)]:
  assert struct.unpack_from('<I',expected[3],48+dest)[0]==videos[slot]['device']+offset
  struct.pack_into('<I',expected[3],48+dest,devices[slot]+offset)
 offset=560
 for number in range(2,5):
  op=n.operations()[number];index=op[1];word=struct.unpack_from('<I',expected[3],offset+20)[0]
  assert word&~63==videos[0]['device']+index*192
  struct.pack_into('<I',expected[3],offset+20,devices[0]+index*192|(word&63))
  for patch in n.patches()[op[4]+1:op[4]+op[5]]:
   slot={2:5,3:2}[patch[2]];at=index*192+patch[1]
   assert struct.unpack_from('<I',expected[0],at)[0]==videos[slot]['device']+patch[4]
   struct.pack_into('<I',expected[0],at,devices[slot]+patch[4])
  offset+=32
 assert offset==656 and struct.unpack_from('<I',expected[3],offset+28)[0]==0x44
 struct.pack_into('<I',expected[3],offset+28,0x40)
 a.output.mkdir(parents=True,exist_ok=False);results=[]
 for i,(b,r,payload) in enumerate(zip(buffers,plan.buffers,expected)):
  assert r.minimum_bytes==(524280 if i==3 else videos[i]['bytes'])
  padding=r.allocation_bytes-len(payload)
  wanted=bytes(payload)+(struct.pack('<II',0x10000000,0)*(padding//8) if i==3 else bytes(padding))
  value=c.string_at(b.cpu,b.bytes);assert value==wanted,('allocation mismatch',i)
  assert c.string_at(b.cpu-4096,4096)==b'\xa5'*4096 and c.string_at(b.cpu+b.bytes,4096)==b'\xa5'*4096
  f=a.output/f'buffer-{i}.bin';f.write_bytes(value);results.append(dict(slot=i,device=hex(b.device),bytes=b.bytes,sha256=hashlib.sha256(value).hexdigest()))
 report=dict(result='pass',model_sha256=sha,native_sha256=hashlib.sha256(a.native.read_bytes()).hexdigest(),allocations=results,shader_operations=1,nt_operations=3,command_body_bytes=656,command_payload_bytes=712,complete_command_fetch_bytes=524288,physical_access=False,sdk_executed=False)
 (a.output/'comparison.json').write_text(json.dumps(report,indent=2)+'\n');print('PASS all SH/NT allocations match saved SDK preparation after relocation/event selection')
if __name__=='__main__':main()
