#!/usr/bin/env python3
"""Profile-specific admission and host capture of actual native DMA records."""
import argparse,copy,hashlib,importlib.util,io,json,struct,time
from pathlib import Path
from elftools.elf.elffile import ELFFile
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
def rejected(fn):
 try:fn()
 except ValueError:return
 raise AssertionError('Expected refusal')
def run(image,fixture,native,out,emmc,emmc_evidence):
 out.mkdir(parents=True,exist_ok=False);host=module('dma_host',ROOT/'tools/check-a733-storage-read-supervised.py');prep=host.preparation
 manifest=image.parent/'image-input-manifest.json';bundle=out/'MODEL-NOT-PHYSICAL-bundle'
 prep.prepare(image,manifest,fixture/'admission.json',fixture/'session-template.bin',bundle,
              Path('/usr/lib/llvm-21/bin/clang'),Path('/home/sunil/.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld'))
 frozen=host.freeze_bundle(bundle);raw=frozen['_frozen']['session.bin'];source=(fixture/'session-template.bin').read_bytes()
 frames=[]
 for name in ('host-idle-frame.bin','host-frame.bin'):
  body=bytearray((native/name).read_bytes());assert body[16:32]==source[24:40]
  # Only fresh package epoch/generation differs from the actual producer's
  # otherwise identical immutable session. Preserve original frames separately.
  body[16:32]=raw[24:40];frames.append(bytes(body))
 for frame in frames:host.decoder.decode(frame,raw)
 assert host.decoder.decode(frames[1],raw)['blocks']==64
 packet=host.decoder.read_request(19,64,1,2)
 class Device:
  def __init__(self,values):self.frames=list(values);self.pending=b'';self.packets=[b'\0',packet];self.reads=0
  def write(self,ep,data,timeout):
   assert ep==2 and not self.pending and data==self.packets.pop(0)
   self.pending=self.frames.pop(0);return len(data)
  def read(self,ep,n,timeout):
   assert ep==0x81 and 0<timeout<=500 and self.pending
   fragment=self.pending[:min(n,997)];self.pending=self.pending[len(fragment):];self.reads+=1;return fragment
 record={};d=Device(frames);result=host.capture_device(d,lambda:None,time.monotonic()+20,raw,out,record,lambda:None,[(19,64)])
 assert record['storage_read_result']=='observed-success' and result['application_reads']==2
 assert (out/'output-001.bin').read_bytes()==frames[1][288:] and not d.frames and not d.pending
 cases={'actual_native_capture':dict(passed=True,bytes=32768,record_bytes=33056,fragments=d.reads)}
 # Both the packet encoder and decoder retain profile-specific limits.
 for profile in (0,1,2,3):
  maximum=64 if profile in (2,3) else 8
  host.decoder.read_request(0,maximum,1,profile)
  rejected(lambda:host.decoder.read_request(0,maximum+1,1,profile))
  test=bytearray(source);struct.pack_into('<I',test,860,profile);struct.pack_into('<I',test,872,maximum+1)
  rejected(lambda:prep.session_check(test))
 for first,count,rid in ((0xffffffff,2,1),(0,0,1),(0,64,0)):
  rejected(lambda:host.decoder.read_request(first,count,rid,2))
 for offset in (32,248,264,288,len(frames[1])-1):
  bad=bytearray(frames[1]);bad[offset]^=1
  expected={k:result[k] for k in ('ticket','request_id','first_sector','blocks','client')}
  rejected(lambda:host.decoder.decode(bad,raw,expected))
 # Reject oversized framing at the header, before another endpoint read.
 bad=bytearray(frames[1]);struct.pack_into('<I',bad,12,65*512)
 class HeaderDevice:
  def __init__(self):self.reads=0
  def read(self,*args,**kwargs):self.reads+=1;assert self.reads==1;return bytes(bad[:288])
 header_out=out/'oversized-header';header_out.mkdir()
 h=HeaderDevice();rejected(lambda:host.read_frame(h,lambda:None,time.monotonic()+2,header_out,{},lambda:None,payload_maximum=32768));assert h.reads==1
 legacy=bytearray(raw);struct.pack_into('<I',legacy,860,1);rejected(lambda:host.decoder.decode(frames[1],legacy))
 cases['profile_and_frame_bounds']=dict(passed=True)
 # Actual ELF geometry and DMA capability must both be immutable and exact.
 body=image.read_bytes();elf=ELFFile(io.BytesIO(body));symbols={s.name:s for s in elf.get_section_by_name('.symtab').iter_symbols()}
 for name in ('storage_read_geometry','storage_read_dma_geometry'):
  symbol=symbols[name];address=int(symbol['st_value'])
  segment=next(p for p in elf.iter_segments() if p['p_type']=='PT_LOAD' and int(p['p_vaddr'])<=address< int(p['p_vaddr'])+int(p['p_filesz']))
  off=int(segment['p_offset'])+address-int(segment['p_vaddr']);bad=bytearray(body);bad[off]^=1
  rejected(lambda:prep.embedded_reference(bad,source))
 cases['elf_capability_binding']=dict(passed=True)
 admission=json.loads((fixture/'admission.json').read_text());ih=hashlib.sha256(body).hexdigest()
 original=prep.validate_admission(admission,ih,hashlib.sha256(source).hexdigest(),source)
 for fact in prep.DMA_MEMORY_FACTS:
  cache=dict(original);entry=copy.deepcopy(admission);changed=bytearray(source);path=entry['memory']['path']
  memory=json.loads(cache[path]);del memory[fact];data=json.dumps(memory,sort_keys=True).encode();cache[path]=data
  digest=hashlib.sha256(data).digest();changed[664:696]=digest;entry['memory']['sha256']=digest.hex()
  th=hashlib.sha256(changed).hexdigest();entry['session_template_sha256']=th
  rejected(lambda:prep.validate_admission(entry,ih,th,changed,cache))
 cases['dma_memory_authority']=dict(passed=True,required=list(prep.DMA_MEMORY_FACTS))
 # Shared helpers must not raise the eMMC API or actual linked geometry limit.
 emmc_observe=module('emmc_dma_compat',ROOT/'apps/emmc-read/observe.py')
 assert emmc_observe.MAXIMUM==4096
 emmc_observe.read_request(0,8,1);rejected(lambda:emmc_observe.read_request(0,9,1))
 with emmc.open('rb') as f:
  ee=ELFFile(f);symbols={s.name:s for s in ee.get_section_by_name('.symtab').iter_symbols()};s=symbols['storage_read_geometry']
  address=int(s['st_value']);p=next(p for p in ee.iter_segments() if p['p_type']=='PT_LOAD' and int(p['p_vaddr'])<=address<int(p['p_vaddr'])+int(p['p_filesz']))
  off=address-int(p['p_vaddr']);assert struct.unpack('<5Q',p.data()[off:off+40])==(0x4153373345524131,912,288,4096,2)
  assert 'storage_read_dma_geometry' not in symbols
 legacy_host=module('emmc_dma_capture',ROOT/'apps/emmc-read/capture.py').shared
 legacy_raw=(emmc_evidence/'fixture-sdc3-v2/session-template.bin').read_bytes()
 prior=json.loads((emmc_evidence/'native-sdc3-v2/result.json').read_text())
 assert hashlib.sha256(legacy_raw).hexdigest()==prior['session_sha256']
 legacy_frames=[(emmc_evidence/'native-sdc3-v2'/n).read_bytes() for n in
                ('idle-frame.bin','pending-frame.bin','native-frame.bin','pending-last-frame.bin','last-frame.bin')]
 for frame in legacy_frames:legacy_host.decoder.decode(frame,legacy_raw)
 legacy_out=out/'emmc-capture';legacy_out.mkdir()
 d=Device(legacy_frames);d.packets=[b'\0',legacy_host.decoder.read_request(19,2,1),b'\0',
                                legacy_host.decoder.read_request(33554424,8,2),b'\0']
 record={};legacy_host.capture_device(d,lambda:None,time.monotonic()+10,legacy_raw,legacy_out,record,lambda:None,[(19,2),(33554424,8)])
 assert record['storage_read_result']=='observed-success' and record['storage_read_application_reads']==2
 (legacy_out/'session.bin').write_bytes(legacy_raw)
 cases['emmc_geometry_and_default']=dict(passed=True,elf=hashlib.sha256(emmc.read_bytes()).hexdigest(),
     reused_native_elf=prior['image_sha256'],reused_native_session=prior['session_sha256'],captured_blocks=[2,8])
 (out/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,elf=ih,cases=cases),indent=2)+'\n')
 print(json.dumps(cases))
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__)
 for n in ('image','fixture','native','output','emmc','emmc-evidence'):p.add_argument('--'+n,type=Path,required=True)
 a=p.parse_args();run(a.image,a.fixture,a.native,a.output,a.emmc,a.emmc_evidence)
