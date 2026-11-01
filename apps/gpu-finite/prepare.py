#!/usr/bin/env python3
"""Prepare a returning GPU session without contacting a board."""
import argparse,hashlib,importlib.util,io,json,secrets,shutil,struct,sys,types
from pathlib import Path
from elftools.elf.elffile import ELFFile
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
from host_evidence import new_directory, unique_directory
def module(name,path):
 spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
original=module('gpu_finite_original',ROOT/'apps/gpu-render/prepare.py');base=original.base
require=original.require
PROFILE='gpu-single-core-returning-v1';SCHEMA='a733-gpu-finite-bundle-v1'
RANGES=((0x41000000,0x42200000),(0x48000000,0x79000000))
FINITE_PLATFORM_CONDITIONS={'primary-cache-transition-and-secondary-off','cold-cache-and-dirty-alias-exclusion','dual-pas-dram','exclusive-cpus-memory-dma','remote-writers-excluded','physical-code-publication','physical-counter-continuity','usb-clock-power-reset-ownership','usb-supplies-stable-and-writers-excluded'}
HOST_SOURCES=('apps/gpu-finite/prepare.py','apps/gpu-finite/report.py','apps/gpu-finite/host.py','tools/a733-returning-host.py','tools/a733-crumbs.py','include/sunstone_crumb.h','apps/gpu-render/prepare.py','apps/gpu-render/observe.py','apps/gpu-render/launch.c','apps/board/prepare.py','tools/check-a733-smp.py','tools/check-a733-smp-supervised.py','tools/check-a733-smp-observe.py','tools/check-a733-kernel.py','tools/usb-access.py','platform/a733-smp-supervised/launch.c','platform/a733-smp-observe/launch.S','platform/a733-smp-observe/launch.ld','platform/a733-smp-cached/boot.h','include/a733_board_application.h')
def sha(data):return hashlib.sha256(data).hexdigest()
def save(path,value):path.write_text(json.dumps(value,indent=2)+'\n')
def image_profile(path,manifest):
 data=path.read_bytes();parts,address,build=original.image_parts(path)

 elf=ELFFile(io.BytesIO(data));symbols={};markers=[]
 for s in elf.get_section_by_name('.symtab').iter_symbols():
  symbols[s.name]=(int(s['st_value']),int(s['st_size']))
  if s.name=='gpu_return_wb_geometry':markers.append(symbols[s.name])
 def at(n):require(n in symbols,'Missing GPU return symbol: '+n);return symbols[n][0]
 for n in ('gpu_return_finish','gpu_return_reset','gpu_finite_smc','gpu_finite_return_ready','gpu_return_allocations','gpu_return_device_quiet'):require(at(n),'Missing returning implementation.')
 require(at('sd_single_core_entry')==0x41000000 and not any(n in symbols for n in ('a733_cpu_start_init','a733_cpu_primary_continue','a733_smp_pre_scheduler','FreeRTOS_SGI_Handler','gpu_transport_only_marker')),'Wrong GPU startup/workload.')
 lo,hi=at('__gpu_private_start'),(at('__gpu_private_end')+63)&~63;receipt=at('gpu_return_receipt');log=at('__gpu_platform_log')
 require(lo==0x41200000 and lo<hi<=0x41f00000 and at('__gpu_retained_start')<=receipt<receipt+104<=at('__gpu_retained_end') and symbols['gpu_return_receipt'][1]==104 and lo<=log<log+16384<=hi,'GPU return layout differs.')
 # Only images built before the typed write-back allocator lack the marker and
 # use the historical span. Every typed image needs exactly one valid marker.
 wb=(0x48000000,0x48086000)
 require(bool(markers)==('device_memory_pool_init_typed' in symbols),'GPU WB geometry marker does not match the allocator.')
 if markers:
  require(len(markers)==1 and markers[0][1]==16,'Malformed GPU WB geometry marker.')
  start=markers[0][0]
  loads=[s for s in elf.iter_segments() if s['p_type']=='PT_LOAD' and int(s['p_vaddr'])<=start and start+16<=int(s['p_vaddr'])+int(s['p_filesz'])]
  require(len(loads)==1 and not int(loads[0]['p_flags'])&2,'GPU WB geometry must be immutable and file-backed.')
  offset=int(loads[0]['p_offset'])+start-int(loads[0]['p_vaddr']);wb=struct.unpack_from('<2Q',data,offset)
  require(wb==(0x48000000,0x48088000),'Unknown GPU WB geometry.')
 return parts,address,build,{'clean_start':lo,'clean_end':hi,'receipt':receipt,'log':log,'wb_start':wb[0],'wb_end':wb[1],'packet':0x42000100}

def validate_admission(record,image_hash,template,build,cache=None,frozen=False):
 # The native ABI consumes the session's entry digest and cold-master token.
 # They are wire identities, not signatures from an external evidence record.
 require(len(template)==592,'Returning GPU requires exactly 592 session bytes.')
 original.session_check(template)
 require(any(template[80:112]) and struct.unpack_from('<Q',template,192)[0],
         'GPU entry digest and cold-master identity must be nonzero.')
 require(record.get('ram_disposition','restore') in ('restore','discard'),'Unknown GPU RAM disposition.')
 reservation=record.get('reservation',1)
 require(type(reservation) is int and 0<reservation<2**64,'Invalid GPU reservation identity.')
 require(not record.get('payloads') and record.get('radio_ddr') is None,'Unexpected GPU payload or radio memory.')
 return {}

def prepare(image,manifest,admission,session_template,output,clang,lld):
 m={};parts,address,build,geometry=image_profile(image,m);template=session_template.read_bytes();record=json.loads(admission.read_bytes()) if admission else {};record={k:record[k] for k in ('ram_disposition','reservation') if k in record};record.setdefault('reservation',secrets.randbits(64) or 1);cache=validate_admission(record,sha(image.read_bytes()),template,build)
 output=new_directory(output)
 for name,source in [('image.elf',image),('session-template.bin',session_template)]:shutil.copy2(source,output/name)
 save(output/'manifest.json',{'execution_profile':PROFILE,'build_id':build.hex(),'final_elf':sha(image.read_bytes())})
 save(output/'admission.json',record)
 raw=bytearray(template);epoch,generation,lease=(secrets.randbits(64) or 1 for _ in range(3));struct.pack_into('<3Q',raw,24,epoch,generation,lease);raw[48:80]=build;(output/'session.bin').write_bytes(raw)
 original.build_launcher(output,clang,lld)
 for name,data in zip(('request.bin','contract.bin'),original.request_bytes(address,build,epoch,generation,lease,raw,record['reservation'],bytes(raw[80:112]))):(output/name).write_bytes(data)
 for name,size in [('result-zero.bin',16384),('report-zero.bin',128)]:(output/name).write_bytes(bytes(size))
 uploads=[]
 for i,part in enumerate(parts):
  if part['data']:
   name=f'segment-{i}.bin';(output/name).write_bytes(part['data']);uploads.append({'name':name,'address':part['address']})
 uploads.append({'name':'session.bin','address':0x41f00000});refs={}
 for i,(name,data) in enumerate(sorted(cache.items())):
  target=f'entry-{i}.bin';(output/target).write_bytes(data);refs[name]={'name':target,'sha256':sha(data)}
 runtime_names={'image.elf','launch.bin','session.bin','request.bin','contract.bin','result-zero.bin','report-zero.bin','admission.json'}|{part['name'] for part in uploads}
 files={name:sha((output/name).read_bytes()) for name in sorted(runtime_names)}
 save(output/'prepared.json',{'schema':SCHEMA,'execution_profile':PROFILE,'physical_access':False,'image_sha256':sha(image.read_bytes()),'launcher_sha256':sha((output/'launch.bin').read_bytes()),'build_id':build.hex(),'epoch':epoch,'generation':generation,'lease':lease,'reservation':record['reservation'],'geometry':geometry,'segments':uploads,'entry_evidence':refs,'output':str(output),'files':files})
 return validate_bundle(output)

def validate_bundle(bundle):
 p=json.loads((bundle/'prepared.json').read_bytes());require(p.get('schema')==SCHEMA and p.get('execution_profile')==PROFILE,'Wrong GPU returning bundle.');frozen={}
 parts,address,build,geometry=image_profile(bundle/'image.elf',{})
 names={'image.elf','launch.bin','session.bin','request.bin','contract.bin','result-zero.bin','report-zero.bin','admission.json'}|{f'segment-{i}.bin' for i,part in enumerate(parts) if part['data']}
 for name in sorted(names):
  frozen[name]=(bundle/name).read_bytes();require(sha(frozen[name])==p['files'][name],'Bundle input changed.')
 p['files']={name:p['files'][name] for name in sorted(names)}
 require(p['geometry']==geometry and p['build_id']==build.hex() and p['image_sha256']==sha(frozen['image.elf']),'Returning metadata differs.')
 record=json.loads(frozen['admission.json']);raw=frozen['session.bin'];validate_admission(record,p['image_sha256'],raw,build)
 require(raw[48:80]==build and struct.unpack_from('<3Q',raw,24)==(p['epoch'],p['generation'],p['lease']) and p['reservation']==record['reservation'],'Native session identity differs.')
 expected=[]
 for i,part in enumerate(parts):
  if part['data']:
   name=f'segment-{i}.bin';require(frozen[name]==part['data'],'Upload differs from ELF.');expected.append({'name':name,'address':part['address']})
 expected.append({'name':'session.bin','address':0x41f00000});require(p['segments']==expected,'Upload addresses differ.')
 for name,value in zip(('request.bin','contract.bin'),original.request_bytes(address,build,p['epoch'],p['generation'],p['lease'],raw,p['reservation'],bytes(raw[80:112]))):require(frozen[name]==value,'Launch packet differs.')
 for name,size in [('result-zero.bin',16384),('report-zero.bin',128)]:require(frozen[name]==bytes(size),'Initial report differs.')
 require(0<len(frozen['launch.bin'])<=0xc00 and sha(frozen['launch.bin'])==p['launcher_sha256'],'Invalid launcher bytes.')
 p['_frozen']=frozen;p['_sizes']={n:len(v) for n,v in frozen.items()};return p
if __name__=='__main__':
 parser=argparse.ArgumentParser(description=__doc__)
 for n in ('image','session-template','output','clang','lld'):parser.add_argument('--'+n,type=Path,required=True)
 parser.add_argument('--manifest',type=Path,help='Optional build metadata.')
 parser.add_argument('--admission',type=Path,help='Optional RAM disposition and reservation configuration.')
 a=parser.parse_args();print(prepare(a.image,a.manifest,a.admission,a.session_template,a.output,a.clang,a.lld)['output'])
