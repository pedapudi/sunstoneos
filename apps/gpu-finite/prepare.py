#!/usr/bin/env python3
"""Freeze the distinct returning GPU admission without contacting a board."""
import argparse,hashlib,importlib.util,io,json,secrets,shutil,struct,types
from pathlib import Path
from elftools.elf.elffile import ELFFile
ROOT=Path(__file__).resolve().parents[2]
def module(name,path):
 spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
original=module('gpu_finite_original',ROOT/'apps/gpu-render/prepare.py');base=original.base
require=original.require
PROFILE='gpu-single-core-returning-v1';SCHEMA='a733-gpu-finite-bundle-v1'
RANGES=((0x41000000,0x42200000),(0x48000000,0x79000000))
HOST_SOURCES=('apps/gpu-finite/prepare.py','apps/gpu-finite/report.py','apps/gpu-finite/host.py','tools/a733-returning-host.py','apps/gpu-render/prepare.py','apps/gpu-render/observe.py','apps/gpu-render/launch.c','apps/board/prepare.py','tools/check-a733-smp.py','tools/check-a733-smp-supervised.py','tools/check-a733-smp-observe.py','tools/check-a733-kernel.py','tools/usb-access.py','platform/a733-smp-supervised/launch.c','platform/a733-smp-observe/launch.S','platform/a733-smp-observe/launch.ld','platform/a733-smp-cached/boot.h','include/a733_board_application.h')
def sha(data):return hashlib.sha256(data).hexdigest()
def save(path,value):path.write_text(json.dumps(value,indent=2)+'\n')
def image_profile(path,manifest):
 data=path.read_bytes();parts,address,build=original.image_parts(path)
 require(manifest.get('target')=='sunstone-a733-gpu-finite-returning' and manifest.get('execution_profile')==PROFILE and manifest.get('workload')=='fixed-arithmetic-repeat-return' and manifest.get('final_elf')==sha(data) and manifest.get('build_id')==build.hex(),'Distinct returning GPU image required.')
 elf=ELFFile(io.BytesIO(data));symbols={s.name:(int(s['st_value']),int(s['st_size'])) for s in elf.get_section_by_name('.symtab').iter_symbols()}
 def at(n):require(n in symbols,'Missing GPU return symbol: '+n);return symbols[n][0]
 for n in ('gpu_return_finish','gpu_return_reset','gpu_finite_smc','gpu_finite_return_ready','gpu_return_allocations','gpu_return_device_quiet'):require(at(n),'Missing returning implementation.')
 require(at('sd_single_core_entry')==0x41000000 and not any(n in symbols for n in ('a733_cpu_start_init','a733_cpu_primary_continue','a733_smp_pre_scheduler','FreeRTOS_SGI_Handler','gpu_transport_only_marker')),'Wrong GPU startup/workload.')
 lo,hi=at('__gpu_private_start'),(at('__gpu_private_end')+63)&~63;receipt=at('gpu_return_receipt');log=at('__gpu_platform_log')
 require(lo==0x41200000 and lo<hi<=0x41f00000 and at('__gpu_retained_start')<=receipt<receipt+104<=at('__gpu_retained_end') and symbols['gpu_return_receipt'][1]==104 and lo<=log<log+16384<=hi,'GPU return layout differs.')
 return parts,address,build,{'clean_start':lo,'clean_end':hi,'receipt':receipt,'log':log,'wb_start':0x48000000,'wb_end':0x48086000,'packet':0x42000100}

def validate_admission(record,image_hash,template,build,cache=None,frozen=False):
 cache={} if cache is None else cache
 def evidence(ref):
  require(isinstance(ref,dict) and set(ref)=={'path','sha256'} and Path(ref['path']).is_absolute(),'Invalid returning evidence reference.')
  name=ref['path']
  if name not in cache:require(not frozen,'Missing frozen evidence.');cache[name]=Path(name).read_bytes()
  require(sha(cache[name])==ref['sha256'],'Changed returning evidence.');return cache[name]
 original.session_check(template)
 require(record.get('schema')=='a733-gpu-finite-entry-v1' and record.get('execution_profile')==PROFILE and record.get('image_sha256')==image_hash and record.get('session_template_sha256')==sha(template) and type(record.get('reservation')) is int and 0<record['reservation']<2**64,'Distinct returning admission required.')
 identity,port,supervisor=(record.get(k) for k in ('identity','port','supervisor'));require(identity and port and supervisor,'Target and supervisor required.')
 for key,ranges in [('ddr',[[0x41000000,0x42200000]]),('gpu_ddr',[[0x48000000,0x79000000]])]:
  ddr=json.loads(evidence(record[key]));require(ddr.get('schema')=='a733-ddr-reservation-v1' and ddr.get('identity')==identity and ddr.get('port')==port and ddr.get('image_sha256')==image_hash and ddr.get('ranges')==ranges and all(ddr.get(k) is True for k in ('exclusive','published_cache_disabled','remote_writers_excluded')) and ddr.get('evidence'),'Full returning DDR ownership required.')
  for ref in ddr['evidence']:evidence(ref)
 platform=json.loads(evidence(record['platform']));fn=original.board.single_core_platform
 types.FunctionType(fn.__code__,{**fn.__globals__,'artifact':evidence})(platform,{'image_sha256':image_hash},identity,port)
 gpu=json.loads(evidence(record['gpu']));require(gpu.get('schema')=='a733-gpu-initial-entry-v1' and gpu.get('identity')==identity and gpu.get('port')==port and gpu.get('image_sha256')==image_hash and gpu.get('reservation')==record['reservation'] and gpu.get('entry_conditions_established') is True and gpu.get('no_gpu_activity_since_initial_isolation') is True and set(gpu.get('conditions',{}))==original.GPU_CONDITIONS and gpu.get('cold_master'),'Actual initial GPU state required.')
 for ref in gpu['conditions'].values():evidence(ref)
 require(template[80:112]==bytes.fromhex(record['gpu']['sha256']) and struct.unpack_from('<Q',template,192)[0]==gpu['cold_master'] and not record.get('payloads') and record.get('radio_ddr') is None,'GPU session/entry binding differs.')
 review=json.loads(evidence(record['root_review']));conditions={'architecture-reset-reviewed','software-handoff-reviewed','normal-return-retirement-reviewed','failure-retention-reviewed'}
 require(review.get('schema')=='a733-gpu-finite-return-review-v1' and review.get('reviewed') is True and review.get('execution_profile')==PROFILE and review.get('image_sha256')==image_hash and review.get('build_id')==build.hex() and review.get('identity')==identity and review.get('port')==port and review.get('supervisor')==supervisor and set(review.get('conditions',{}))==conditions,'Exact root returning-lifetime review required.')
 for ref in review['conditions'].values():evidence(ref)
 require(review.get('host_sources')=={n:sha((ROOT/n).read_bytes()) for n in HOST_SOURCES},'Root review covers different host sources.')
 required={'successful-close-stop-quiescence-ROM-retirement','failure-retain-until-whole-power-removal','DRAM-restore-only-after-validated-normal-return','no-old-SRAM-restore','manual-whole-board-power-removal','stop-after-first-failure'}
 require(record.get('one_attempt') is True and set(record.get('supervision',{}))==required,'Explicit returning supervision required.')
 for ref in record['supervision'].values():evidence(ref)
 return cache

def prepare(image,manifest,admission,session_template,output,clang,lld):
 m=json.loads(manifest.read_bytes());parts,address,build,geometry=image_profile(image,m);template=session_template.read_bytes();record=json.loads(admission.read_bytes());cache=validate_admission(record,sha(image.read_bytes()),template,build)
 output.mkdir(parents=True,exist_ok=False)
 for name,source in [('image.elf',image),('manifest.json',manifest),('admission.json',admission),('session-template.bin',session_template)]:shutil.copy2(source,output/name)
 raw=bytearray(template);epoch,generation,lease=(secrets.randbits(64) or 1 for _ in range(3));struct.pack_into('<3Q',raw,24,epoch,generation,lease);raw[48:80]=build;(output/'session.bin').write_bytes(raw)
 original.build_launcher(output,clang,lld);review=json.loads(cache[record['root_review']['path']]);require(review.get('launcher_sha256')==sha((output/'launch.bin').read_bytes()),'Launcher differs from reviewed bytes.')
 for name,data in zip(('request.bin','contract.bin'),original.request_bytes(address,build,epoch,generation,lease,raw,record['reservation'],bytes.fromhex(record['gpu']['sha256']))):(output/name).write_bytes(data)
 for name,size in [('result-zero.bin',16384),('report-zero.bin',128)]:(output/name).write_bytes(bytes(size))
 uploads=[]
 for i,part in enumerate(parts):
  if part['data']:
   name=f'segment-{i}.bin';(output/name).write_bytes(part['data']);uploads.append({'name':name,'address':part['address']})
 uploads.append({'name':'session.bin','address':0x41f00000});refs={}
 for i,(name,data) in enumerate(sorted(cache.items())):
  target=f'entry-{i}.bin';(output/target).write_bytes(data);refs[name]={'name':target,'sha256':sha(data)}
 sources={}
 for name in HOST_SOURCES:
  target=output/'host'/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(ROOT/name,target);sources[name]=sha(target.read_bytes())
 files={p.name:sha(p.read_bytes()) for p in output.iterdir() if p.is_file()}
 save(output/'prepared.json',{'schema':SCHEMA,'execution_profile':PROFILE,'physical_access':False,'image_sha256':sha(image.read_bytes()),'launcher_sha256':sha((output/'launch.bin').read_bytes()),'build_id':build.hex(),'epoch':epoch,'generation':generation,'lease':lease,'reservation':record['reservation'],'geometry':geometry,'segments':uploads,'entry_evidence':refs,'host_sources':sources,'files':files})
 return validate_bundle(output)

def validate_bundle(bundle):
 p=json.loads((bundle/'prepared.json').read_bytes());require(p.get('schema')==SCHEMA and p.get('execution_profile')==PROFILE and p.get('physical_access') is False,'Wrong GPU returning bundle.');frozen={}
 require(set(p['host_sources'])==set(HOST_SOURCES),'Incomplete host source closure.')
 for name,h in p['host_sources'].items():require(sha((ROOT/name).read_bytes())==h==sha((bundle/'host'/name).read_bytes()),'Host source changed.')
 for name,h in p['files'].items():require(Path(name).name==name,'Invalid bundle filename.');frozen[name]=(bundle/name).read_bytes();require(sha(frozen[name])==h,'Bundle input changed.')
 parts,address,build,geometry=image_profile(bundle/'image.elf',json.loads(frozen['manifest.json']));require(p['geometry']==geometry and p['build_id']==build.hex() and p['image_sha256']==sha(frozen['image.elf']),'Returning metadata differs.')
 record=json.loads(frozen['admission.json']);cache={n:frozen[v['name']] for n,v in p['entry_evidence'].items()};validate_admission(record,p['image_sha256'],frozen['session-template.bin'],build,cache,True)
 raw=bytearray(frozen['session-template.bin']);struct.pack_into('<3Q',raw,24,p['epoch'],p['generation'],p['lease']);raw[48:80]=build;require(raw==frozen['session.bin'] and p['reservation']==record['reservation'],'Session derivation differs.')
 expected=[]
 for i,part in enumerate(parts):
  if part['data']:
   name=f'segment-{i}.bin';require(frozen[name]==part['data'],'Upload differs from ELF.');expected.append({'name':name,'address':part['address']})
 expected.append({'name':'session.bin','address':0x41f00000});require(p['segments']==expected,'Upload addresses differ.')
 for name,value in zip(('request.bin','contract.bin'),original.request_bytes(address,build,p['epoch'],p['generation'],p['lease'],raw,p['reservation'],bytes.fromhex(record['gpu']['sha256']))):require(frozen[name]==value,'Launch packet differs.')
 for name,size in [('result-zero.bin',16384),('report-zero.bin',128)]:require(frozen[name]==bytes(size),'Initial report differs.')
 review=json.loads(cache[record['root_review']['path']]);require(0<len(frozen['launch.bin'])<=0xc00 and sha(frozen['launch.bin'])==review['launcher_sha256']==p['launcher_sha256'],'Launcher differs from reviewed bytes.')
 p['_frozen']=frozen;p['_sizes']={n:len(v) for n,v in frozen.items()};return p
if __name__=='__main__':
 parser=argparse.ArgumentParser(description=__doc__)
 for n in ('image','manifest','admission','session-template','output','clang','lld'):parser.add_argument('--'+n,type=Path,required=True)
 a=parser.parse_args();prepare(a.image,a.manifest,a.admission,a.session_template,a.output,a.clang,a.lld)
