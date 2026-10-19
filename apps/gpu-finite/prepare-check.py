#!/usr/bin/env python3
"""Exercise returning GPU bundle production with explicit software-only facts."""
import argparse,copy,hashlib,importlib.util,json,shutil,struct,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def module(name,path):
 spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
prep=module('gpu_finite_prepare_check',ROOT/'apps/gpu-finite/prepare.py')
def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--evidence',type=Path,required=True);parser.add_argument('--shared-helper',type=Path,required=True);parser.add_argument('--image-evidence',type=Path,default=Path.home()/'.cache/sunstoneos/evidence/gpu-returning-application-20261019/ns-publication');a=parser.parse_args();e=a.evidence.resolve();e.mkdir(parents=True,exist_ok=True)
 def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
 def dump(name,value):path=e/name;path.write_text(json.dumps(value,indent=2)+'\n');return {'path':str(path),'sha256':sha(path)}
 assert (ROOT/'tools/a733-returning-host.py').read_bytes()==a.shared_helper.read_bytes()
 image=a.image_evidence/'build/return.elf';manifest=a.image_evidence/'manifest.json';parts,address,build,geometry=prep.image_profile(image,json.loads(manifest.read_bytes()));h=sha(image)
 model=e/'MODEL-ONLY.txt';model.write_text('Synthetic test evidence. No physical admission or qualified hardware fact.\n');ref={'path':str(model),'sha256':sha(model)}
 def fact(schema,**fields):return {'schema':schema,'identity':'software-only-model','port':'no-physical-port','image_sha256':h,**fields}
 ddr=dump('ddr.json',fact('a733-ddr-reservation-v1',exclusive=True,published_cache_disabled=True,remote_writers_excluded=True,ranges=[[0x41000000,0x42200000]],evidence=[ref]))
 gpu_ddr=dump('gpu-ddr.json',fact('a733-ddr-reservation-v1',exclusive=True,published_cache_disabled=True,remote_writers_excluded=True,ranges=[[0x48000000,0x79000000]],evidence=[ref]))
 names={'primary-cache-transition-and-secondary-off','cold-cache-and-dirty-alias-exclusion','dual-pas-dram','exclusive-cpus-memory-dma','remote-writers-excluded','physical-code-publication','usb-clock-power-reset-ownership','usb-device-mapping','physical-counter-continuity','rom-usb0-pio-takeover','usb0-irq188-189-190-disabled-inactive-clear','usb-supplies-stable-and-writers-excluded'}
 platform=dump('platform.json',fact('a733-board-single-core-platform-v1',conditions={n:ref for n in names},entry_conditions_established=True,physical_coherence_result='under-test'))
 gpu=dump('gpu.json',fact('a733-gpu-initial-entry-v1',reservation=100,cold_master=200,conditions={n:ref for n in prep.original.GPU_CONDITIONS},entry_conditions_established=True,no_gpu_activity_since_initial_isolation=True))
 template=bytearray((Path.home()/'.cache/sunstoneos/evidence/gpu-single-core-20261018/provider-valid-fixture/fixtures/session-template.bin').read_bytes());template[80:112]=bytes.fromhex(gpu['sha256']);struct.pack_into('<Q',template,192,200);template_path=e/'session-template.bin';template_path.write_bytes(template)
 clang=Path('/usr/lib/llvm-21/bin/clang');lld=Path.home()/'.cache/sunstoneos/host/usr/bin/ld.lld-21';launch=e/'launcher';launch.mkdir(exist_ok=True);prep.original.build_launcher(launch,clang,lld)
 review=dump('root-review-MODELED.json',fact('a733-gpu-finite-return-review-v1',reviewed=True,execution_profile=prep.PROFILE,build_id=build.hex(),supervisor='modeled-supervisor',conditions={n:ref for n in ['architecture-reset-reviewed','software-handoff-reviewed','normal-return-retirement-reviewed','failure-retention-reviewed']},host_sources={n:sha(ROOT/n) for n in prep.HOST_SOURCES},launcher_sha256=sha(launch/'launch.bin')))
 admission=fact('a733-gpu-finite-entry-v1',execution_profile=prep.PROFILE,supervisor='modeled-supervisor',session_template_sha256=sha(template_path),reservation=100,ddr=ddr,gpu_ddr=gpu_ddr,platform=platform,gpu=gpu,root_review=review,one_attempt=True,supervision={n:ref for n in ['successful-close-stop-quiescence-ROM-retirement','failure-retain-until-whole-power-removal','DRAM-restore-only-after-validated-normal-return','no-old-SRAM-restore','manual-whole-board-power-removal','stop-after-first-failure']})
 admission_path=e/'admission-MODELED.json';admission_path.write_text(json.dumps(admission));bundle=e/'bundle';p=prep.prepare(image,manifest,admission_path,template_path,bundle,clang,lld);assert p['_sizes']['session.bin']==592 and p['_sizes']['contract.bin']==4096
 cases={'actual-bundle-producer-and-validator':True}
 for key,value in [('schema','a733-gpu-supervised-entry-v2'),('execution_profile','gpu-single-core-supervised-v2'),('image_sha256','0'*64),('session_template_sha256','0'*64),('one_attempt',False),('reservation',0),('supervision',{}),('root_review',ref),('gpu_ddr',ddr)]:
  bad=copy.deepcopy(admission);bad[key]=value
  try:prep.validate_admission(bad,h,bytes(template),build)
  except (ValueError,KeyError,json.JSONDecodeError):cases['admission-reject-'+key]=True
  else:raise AssertionError(key)
 for key,value in [('reviewed',False),('schema','retained-profile'),('execution_profile','gpu-single-core-supervised-v2'),('build_id','0'*64),('identity','another-instance'),('port','another-port'),('supervisor','another-supervisor'),('conditions',{}),('host_sources',{})]:
  changed=json.loads((e/'root-review-MODELED.json').read_bytes());changed[key]=value
  bad=copy.deepcopy(admission);bad['root_review']=dump('bad-review-'+key+'.json',changed)
  try:prep.validate_admission(bad,h,bytes(template),build)
  except ValueError:cases['review-reject-'+key]=True
  else:raise AssertionError(key)
 for key in ['epoch','generation','lease','reservation','build_id','geometry','segments']:
  original=(bundle/'prepared.json').read_bytes();bad=json.loads(original)
  if key in ['epoch','generation','lease','reservation']:bad[key]+=1
  elif key=='build_id':bad[key]='0'*64
  else:bad[key]={} if key=='geometry' else []
  (bundle/'prepared.json').write_text(json.dumps(bad))
  try:prep.validate_bundle(bundle)
  except (ValueError,KeyError):cases['bundle-reject-'+key]=True
  else:raise AssertionError(key)
  finally:(bundle/'prepared.json').write_bytes(original)
 for name in ['session.bin','contract.bin','request.bin','launch.bin','result-zero.bin','report-zero.bin']:
  path=bundle/name;original=path.read_bytes();bad=bytearray(original);bad[-1]^=1;path.write_bytes(bad)
  try:prep.validate_bundle(bundle)
  except ValueError:cases['bundle-reject-'+name]=True
  else:raise AssertionError(name)
  finally:path.write_bytes(original)
 result={'passed':True,'physical_access':False,'physical_admission_created':False,'synthetic_fixtures_only':True,'image_sha256':h,'shared_helper':{'path':str(a.shared_helper.resolve()),'sha256':sha(a.shared_helper)},'cases':cases,'source_sha256':{str(p.relative_to(ROOT)):sha(p) for p in [ROOT/'apps/gpu-finite/prepare.py',Path(__file__)]}}
 (e/'result.json').write_text(json.dumps(result,indent=2)+'\n');print('PASS GPU returning preparation:',len(cases),'modeled cases')
if __name__=='__main__':main()
