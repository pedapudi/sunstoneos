#!/usr/bin/env python3
"""Prepare or capture the exact NPU returning profile; no admission defaults."""
import argparse,hashlib,importlib.util,io,json,os,re,shutil,struct,subprocess,sys,time,types
from pathlib import Path
from elftools.elf.elffile import ELFFile
ROOT=Path(__file__).resolve().parents[2]
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
original=module('finite_npu_original',ROOT/'apps/npu-convolution/prepare-supervised.py')
base=original.supervisor.base
require,sha=base.require,original.sha
PROFILE='npu-finite-return-v1';SCHEMA='a733-npu-finite-entry-v1';DRAM=(0x41000000,0x1200000)
HOST_SOURCES=('apps/npu-finite/host.py','apps/npu-convolution/prepare-supervised.py','apps/npu-convolution/observe.py','tools/check-a733-smp.py','tools/check-a733-smp-supervised.py','tools/check-a733-smp-observe.py','tools/check-a733-kernel.py','tools/usb-access.py','tools/npu-models.json','platform/a733-smp-supervised/launch.c','platform/a733-smp-observe/launch.S','platform/a733-smp-observe/launch.ld','platform/a733-smp-cached/boot.h')
def save(path,value):base.save_json(path,value)
def evidence_ref(item,cache=None):
 require(isinstance(item,dict) and set(item)=={'path','sha256'},'Invalid evidence reference.')
 path=Path(item['path']);require(path.is_absolute(),'Evidence path must be absolute.')
 data=path.read_bytes() if cache is None else cache[str(path)]
 require(sha(data)==item['sha256'],'Evidence changed: '+str(path));return data

def image_profile(data,manifest):
 require(manifest.get('execution_profile')==PROFILE and manifest.get('final_elf')==sha(data),'Exact returning image manifest required.')
 parts,address,build,layout=original.image_data(data)
 elf=ELFFile(io.BytesIO(data));symbols={s.name:s for s in elf.get_section_by_name('.symtab').iter_symbols()}
 for name in ('npu_finite_receipt','npu_finite_smc','npu_finite_rom_return','a733_npu_application_quiescent'):
  require(name in symbols and symbols[name]['st_value'],'Missing returning producer: '+name)
 def address_of(name):return int(symbols[name]['st_value'])
 def read(name):
  s=symbols[name];at,n=int(s['st_value']),int(s['st_size'])
  for p in parts:
   if p['address']<=at and at+n<=p['address']+len(p['data']):return p['data'][at-p['address']:at-p['address']+n]
  raise ValueError('Unbacked immutable symbol: '+name)
 lo,hi=address_of('__npu_clean_start'),address_of('__device_pool_end')
 receipt=address_of('npu_finite_receipt');log=address_of('__npu_platform_log')
 require(lo==0x41200000 and lo<hi<=0x41f00000 and not (lo|hi)&4095 and
  address_of('__npu_retained_start')<=receipt<receipt+88<=address_of('__npu_retained_end') and
  lo<=log<log+16384<=hi and manifest.get('build_id')==build.hex(),'Returning layout/build identity differs.')
 return parts,address,build,layout,{'clean_start':lo,'clean_end':hi,'receipt':receipt,'log':log,'reference':read('a733_npu_application_reference').hex()}

def validate_admission(admission_raw,raw,build,layout,image_hash,cache=None):
 """Preserve original NPU facts; require a distinct reviewed returning lifetime.

 This profile-specific predicate follows prepare-supervised.validate_admission.
 Its supervision schema is explicit; old retained-only flags are never translated.
 """
 record=json.loads(admission_raw);collected={}
 def evidence(item):
  data=evidence_ref(item,cache);collected[item['path']]=data;return data
 profile=original.session_profile(raw,layout);boot,boot_bytes,pll,pll_bytes=layout[1:5]
 require(record.get('schema')==SCHEMA and record.get('execution_profile')==PROFILE and
  record.get('image_sha256')==image_hash and raw[48:80]==build and raw[80:112]==hashlib.sha256(admission_raw).digest(),'Session does not bind returning entry/image.')
 require(struct.unpack_from('<Q',raw,8)[0]==2 and all(record.get(k)==v for k,v in profile.items()),'Returning admission/profile identities differ.')
 require(record.get('boot_sha256')==sha(raw[boot:boot+boot_bytes]) and record.get('pll_sha256')==sha(raw[pll:pll+pll_bytes]),'Typed boot/PLL bytes differ.')
 identity,port=record.get('identity'),record.get('port');require(identity and port,'Fresh target identity required.')
 ddr=json.loads(evidence(record['ddr']))
 require(ddr.get('schema')=='a733-ddr-reservation-v1' and ddr.get('identity')==identity and ddr.get('port')==port and
  ddr.get('image_sha256')==image_hash and ddr.get('ranges')==[[0x41000000,0x42200000]] and
  all(ddr.get(k) is True for k in ('exclusive','published_cache_disabled','remote_writers_excluded')) and ddr.get('evidence'),'Fresh full DDR ownership/initialization required.')
 for ref in ddr['evidence']:evidence(ref)
 # The existing platform predicate retains all secondary/cache/MMU/ROM-PIO facts.
 platform=json.loads(evidence(record['platform']));fn=original.protocol.validate_platform
 validate=types.FunctionType(fn.__code__,{**fn.__globals__,'artifact':evidence})
 validate(platform,{**profile,'image_sha256':image_hash,'execution_profile':'npu-single-core-supervised-v1'},identity,port)
 required={'clock':{'independent-dram-phy-sources','exclusive-pll-npu-consumers-and-writers','no-autonomous-parent-selector-writers','enabled-retune-snapshot'},'power':{'dcdc2-stable-800mv','authenticated-pmic-pin-inheritance'},'supervision':{'manual-whole-board-power-removal','stop-after-first-failure','successful-close-stop-quiescence-ROM-retirement','failure-retain-until-whole-power-removal','DRAM-restore-only-after-validated-normal-return','no-old-SRAM-restore'}}
 if struct.unpack_from('<I',raw,pll+24)[0]==3:
  required['clock'].remove('enabled-retune-snapshot');required['clock'].update({'live-disabled-pll-quiescent-on-snapshot','native-dram-continuations-and-shared-sram-abandonment','supervised-cross-pll-disturbance-risk'})
 for name,offset in [('clock',112),('power',144),('supervision',176)]:
  data=evidence(record[name]);require(hashlib.sha256(data).digest()==raw[offset:offset+32],name+' digest differs.');facts=json.loads(data)
  require(facts.get('image_sha256')==image_hash and facts.get('identity')==identity and facts.get('port')==port and facts.get('whole_platform')==profile['whole_platform'] and set(facts.get('conditions',{}))==required[name],name+' facts do not cover returning entry.')
  for ref in facts['conditions'].values():evidence(ref)
  if name=='clock':require(facts.get('pll_sha256')==record['pll_sha256'],'Clock snapshot differs.')
  if name=='power':require(facts.get('supply_uv')==800000 and facts.get('boot_sha256')==record['boot_sha256'],'Power configuration differs.')
  if name=='supervision':require(facts.get('supervisor')==profile['supervisor'] and facts.get('one_attempt') is True,'One-attempt supervisor differs.')
 review=json.loads(evidence(record['root_review']))
 require(review.get('schema')=='a733-npu-finite-return-review-v1' and review.get('reviewed') is True and
  review.get('execution_profile')==PROFILE and review.get('image_sha256')==image_hash and review.get('build_id')==build.hex() and
  review.get('identity')==identity and review.get('port')==port and review.get('supervisor')==profile['supervisor'] and
  set(review.get('conditions',{}))=={'architecture-reset-reviewed','software-handoff-reviewed','normal-return-retirement-reviewed','failure-retention-reviewed'},'Root review does not cover this exact returning lifetime.')
 for ref in review['conditions'].values():evidence(ref)
 require(review.get('host_sources')=={n:sha((ROOT/n).read_bytes()) for n in HOST_SOURCES},'Root review covers different host sources.')
 return profile,collected

def prepare(image,manifest,session,admission,output,clang,lld,objcopy):
 data=image.read_bytes();parts,address,build,layout,geometry=image_profile(data,json.loads(manifest.read_bytes()));raw=session.read_bytes();entry=admission.read_bytes()
 profile,cache=validate_admission(entry,raw,build,layout,sha(data));output.mkdir(parents=True,exist_ok=False)
 for name,source in [('image.elf',image),('manifest.json',manifest),('session.bin',session),('admission.json',admission)]:shutil.copy2(source,output/name)
 original.build_launcher(output,clang,lld,objcopy)
 root_review=json.loads(cache[json.loads(entry)['root_review']['path']])
 require(root_review.get('launcher_sha256')==sha((output/'launch.bin').read_bytes()),'Launcher differs from reviewed bytes.')
 request,contract=original.request_bytes(address,build,raw,profile)
 for name,value in [('request.bin',request),('contract.bin',contract),('result-zero.bin',bytes(16384)),('report-zero.bin',bytes(128))]:(output/name).write_bytes(value)
 segments=[]
 for i,part in enumerate(parts):
  if part['data']:
   name=f'segment-{i}.bin';(output/name).write_bytes(part['data']);segments.append({'name':name,'address':part['address']})
 segments.append({'name':'session.bin','address':0x41f00000})
 refs={}
 for i,(path,value) in enumerate(sorted(cache.items())):
  name=f'entry-{i}.bin';(output/name).write_bytes(value);refs[path]={'name':name,'sha256':sha(value)}
 sources={}
 for name in HOST_SOURCES:
  target=output/'host'/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(ROOT/name,target);sources[name]=sha(target.read_bytes())
 files={p.name:sha(p.read_bytes()) for p in output.iterdir() if p.is_file()}
 result={'schema':'a733-npu-finite-bundle-v1','execution_profile':PROFILE,'physical_access':False,'image_sha256':sha(data),'build_id':build.hex(),'session_layout':list(layout),'geometry':geometry,'segments':segments,'entry_evidence':refs,'host_sources':sources,'files':files,**profile}
 save(output/'prepared.json',result);return result

def validate_bundle(bundle):
 p=json.loads((bundle/'prepared.json').read_bytes());require(p.get('schema')=='a733-npu-finite-bundle-v1' and p.get('execution_profile')==PROFILE and p.get('physical_access') is False,'Wrong returning bundle.')
 for name,identity in p['host_sources'].items():
  require(name in HOST_SOURCES and sha((ROOT/name).read_bytes())==identity==sha((bundle/'host'/name).read_bytes()),'Host source changed: '+name)
 require(set(p['host_sources'])==set(HOST_SOURCES),'Incomplete host source closure.')
 frozen={}
 for name,identity in p['files'].items():
  require(Path(name).name==name,'Invalid bundle filename.');frozen[name]=(bundle/name).read_bytes();require(sha(frozen[name])==identity,'Bundle file changed: '+name)
 parts,address,build,layout,geometry=image_profile(frozen['image.elf'],json.loads(frozen['manifest.json']))
 cache={path:frozen[ref['name']] for path,ref in p['entry_evidence'].items()}
 profile,_=validate_admission(frozen['admission.json'],frozen['session.bin'],build,layout,sha(frozen['image.elf']),cache)
 require(all(p.get(k)==v for k,v in profile.items()) and p['geometry']==geometry and p['build_id']==build.hex() and p['image_sha256']==sha(frozen['image.elf']),'Prepared metadata differs.')
 expected=[]
 for i,part in enumerate(parts):
  if part['data']:
   name=f'segment-{i}.bin';require(frozen[name]==part['data'],'Segment differs.');expected.append({'name':name,'address':part['address']})
 expected.append({'name':'session.bin','address':0x41f00000});require(p['segments']==expected,'Upload addresses differ.')
 request,contract=original.request_bytes(address,build,frozen['session.bin'],profile)
 for name,value in [('request.bin',request),('contract.bin',contract),('result-zero.bin',bytes(16384)),('report-zero.bin',bytes(128))]:require(frozen[name]==value,'Launch packet differs: '+name)
 root_review=json.loads(cache[json.loads(frozen['admission.json'])['root_review']['path']])
 require(0<len(frozen['launch.bin'])<=0xc00 and sha(frozen['launch.bin'])==root_review.get('launcher_sha256'),'Launcher differs from root review.')
 p['_frozen']=frozen;p['_sizes']={k:len(v) for k,v in frozen.items()};return p

def validate_reports(monitor,platform,receipt,p):
 require(len(monitor)==128 and len(platform)==16384 and len(receipt)==88,'Incomplete returned reports.')
 m=struct.unpack('<16Q',monitor);h=struct.unpack_from('<16Q',platform);r=struct.unpack('<11Q',receipt);g=p['geometry']
 magic=0x4153373343525331
 require(m[0]==magic and (m[1],m[2])==(p['epoch'],p['generation']) and m[3]==5 and m[4] in (0,1) and m[5]==0 and m[7]==1,'No matched normal monitor completion.')
 status=m[4];deadline=r[9];armed=deadline-480000000
 require(r[:6]==(0x4e50555245543031,p['epoch'],p['generation'],p['lease'],status,2 if status else 1) and
  r[6:9]==(g['clean_start'],g['clean_end'],(g['clean_end']-g['clean_start'])//64) and
  0<=armed<=m[6]<armed+24000000 and m[6]<=m[8]<deadline,'Receipt identity/cleanup/time differs.')
 require(h[0]==0x534b4552 and h[1]==5 and h[2]==status and h[9]==m[8] and h[3]==r[10] and h[3]<=16127 and m[6]<=h[8]<=h[9] and h[4:8]==(0,0,0,0),'Platform completion/log differs.')
 text=platform[256:256+h[3]].decode('ascii');require(platform[256+h[3]]==0,'Unterminated returned log.')
 pattern=r'NPU_STAGE=QUIESCENT\nNPU_FIRST_DIGEST=\n((?:0x[0-9a-f]{16}\n){32})NPU_SECOND_OUTPUT=\n((?:0x[0-9a-f]{16}\n)+)(PASS NPU two matched jobs, copied output, REAP, CLOSE, local stop and parked providers|FAIL NPU numerical comparison)\n'
 matches=list(re.finditer(pattern,text));require(len(matches)==1 and matches[0].end()==len(text),'Missing or ambiguous finite output/terminal marker.')
 values=lambda raw:[int(x,16) for x in raw.splitlines()]
 first,second=values(matches[0][1]),values(matches[0][2]);require(all(v<256 for v in first+second),'Invalid copied output byte.')
 reference=bytes.fromhex(g['reference']);require(len(second)==len(reference),'Copied output extent differs.')
 matched=bytes(first)==hashlib.sha256(reference).digest() and bytes(second)==reference
 require(matched==(status==0) and matches[0][3].startswith('PASS')==(status==0),'Numerical status disagrees with copied outputs.')
 return {'normal_return':True,'functional_success':matched,'controlled_negative':not matched,'status':status,'diagnostic_deadline':deadline,'first_digest':bytes(first).hex(),'second_output':bytes(second).hex(),'log':text}

def transfer(transaction,address,data,destination):
 with base.sealed_source(data) as (source,fd):transaction('write',hex(address),source,'read',hex(address),len(data),destination,pass_fds=(fd,))
 require(destination.read_bytes()==data,'Restoration readback differs.');base.sync_file(destination)

def run_once(transaction,wait_return,ready,bundle,p,evidence,record,save):
 """Root owns the lock/claim; every transport failure is terminal to this call."""
 require(not record.get('launch_attempted'),'One attempt only.');ready()
 backup=evidence/'before-DRAM.bin';transaction('read',hex(DRAM[0]),DRAM[1],backup)
 require(backup.stat().st_size==DRAM[1],'Incomplete full DRAM backup.');base.sync_file(backup);backup_hash=base.digest(backup)
 record.update(dram_backup_sha256=backup_hash,dram_restored=False);save()
 record['dirty']=True;save();base.preload(transaction,bundle,p,evidence)
 words=[int(v,16) for v in transaction(*[a for _ in range(4) for a in ('readl','0x03000024')]).split()]
 require(len(words)==4 and all(0<=v<=0xffffffff and not v&0x100 for v in words),'Protected FEL input is not consistently low.')
 ready();require(int(transaction('readl','0x02050014').strip(),16)&1==0,'Watchdog enabled before execution.')
 record.update(launch_attempted=True,backup_sha256=p['_backup_hashes'].copy());save();transaction('exe','0x52000');wait_return()
 require('soc=00001903(A733)' in transaction('version'),'Returned service is not A733 ROM.')
 reports=[]
 for name,address,size in [('monitor',0x42000000,128),('platform',p['geometry']['log'],16384),('receipt',p['geometry']['receipt'],88)]:
  path=evidence/(name+'.bin');transaction('read',hex(address),size,path);require(path.stat().st_size==size,'Short '+name+' report.');base.sync_file(path);reports.append(path.read_bytes())
 result=validate_reports(*reports,p);record['npu_result']=result;record['result']={'magic':0x534b4552,'stage':5,'status':result['status']};save()
 # No exception/report failure can reach restoration. The new ROM owns SRAM.
 restoration=backup.read_bytes();require(sha(restoration)==backup_hash,'Saved DRAM changed.');transfer(transaction,DRAM[0],restoration,evidence/'DRAM-restored.bin')
 record.update(complete=True,dirty=False,dram_restored=True,rom_usable=True,sram_restored=False,ownership_release_permitted=True);save();return result

def retain_failure(record,save,quarantine,error,hold=base.hold_ownership):
 if record.get('dirty') or record.get('transport_failed') or record.get('launch_attempted'):
  try:base.stop_after_failure(record,save,quarantine,error,hold=hold)
  finally:
   if not record.get('launch_attempted'):hold()


def run(args):
 bundle=args.bundle.resolve();p=validate_bundle(bundle)
 require(base.digest(args.fel)==base.FEL_HASH,'FEL executable differs from reviewed bytes.')
 admission=json.loads(p['_frozen']['admission.json']);cache={path:p['_frozen'][ref['name']] for path,ref in p['entry_evidence'].items()}
 access=module('finite_npu_access',ROOT/'tools/usb-access.py');kernel=module('finite_npu_kernel',ROOT/'tools/check-a733-kernel.py')
 sys.path.insert(0,str(Path.home()/'git/mimus/tools'));import usb_guard
 usb_guard.check_health();args.evidence.mkdir(parents=True,exist_ok=False)
 record={'mode':'kernel','execution_profile':PROFILE,'complete':False,'launch_attempted':False,'dirty':False,'transport_failed':False,'root_owned_claim':args.root_owned_claim,'transactions':[],'tool_sha256':base.FEL_HASH,'image_sha256':p['image_sha256']}
 def journal():save(args.evidence/'session.json',record)
 with access.board_lock() as node:
  access.require_session(node);access.require_rom(node);identity=access.identity(node);port=access.target(node);config={'port':port.name,'sysfs':str(port.resolve())};failed=False
  record.update(initial_node=str(node),initial_identity=identity,port=port.name);journal()
  def ready():
   require(not failed,'No admission after transport failure.')
   validate_bundle(bundle)
   require(admission['identity']==identity and admission['port']==port.name,'Admission covers another live instance.')
   if not record['launch_attempted']:
    kernel.validate_platform_admission((args.ddr_evidence.read_bytes(),args.platform_survey.read_bytes()),node,identity,port.name,base.FEL_HASH,base.FEL_HASH,tuple(x.read_bytes() for x in args.warm_return))
    covered={(v['path'],v['sha256']) for v in json.loads(cache[admission['ddr']['path']])['evidence']}
    require(all((str(x.resolve()),sha(x.read_bytes())) in covered for x in [args.ddr_evidence,args.platform_survey,*args.warm_return]),'Live DDR/survey continuity is not covered by returning admission.')
  def transaction(*arguments,pass_fds=()):
   nonlocal failed
   require(not failed,'A transport failure forbids all further requests.')
   entry={'arguments':[str(x) for x in arguments]};record['transactions'].append(entry);journal()
   try:
    require(access.identity(node)==identity,'USB instance changed before FEL request.');access.require_rom(node)
    result=subprocess.run([str(args.fel),'-d',f'{int(node.parent.name)}:{int(node.name)}',*map(str,arguments)],capture_output=True,text=True,timeout=120 if 'read' in arguments or 'write' in arguments else 15,pass_fds=pass_fds)
    entry.update(exit_code=result.returncode,stdout=result.stdout,stderr=result.stderr);require(result.returncode==0,'FEL request failed.')
   except (OSError,ValueError,subprocess.TimeoutExpired) as error:
    failed=True;entry['failure']=str(error);record['transport_failed']=True;journal();raise
   journal();return result.stdout
  def wait_return():
   nonlocal node,identity
   old=identity;deadline=time.monotonic()+30
   while True:
    candidate=access.session_node(config)
    if candidate is not None and access.identity(candidate)!=old and os.access(candidate,os.R_OK|os.W_OK):break
    require(time.monotonic()<deadline,'No new ROM instance within return window; no retry.');time.sleep(.05)
   node=candidate;identity=access.identity(node);access.require_session(node);access.require_rom(node)
   record.update(returned_node=str(node),returned_identity=identity,automatic_access=True);journal()
  try:
   ready();require('soc=00001903(A733)' in transaction('version'),'Target is not A733.')
   with usb_guard.reenumeration('sunstoneos-a733','NPU finite inference and validated RAM return',2):
    result=run_once(transaction,wait_return,ready,bundle,p,args.evidence,record,journal)
  except BaseException as error:
   record['transport_failed']=failed
   retain_failure(record,journal,lambda:access.quarantine_instance(node,'NPU returning attempt stopped; no requests or retry; root recovery required.',identity),error)
   raise
 return 0 if result['functional_success'] else 2

def main(argv=None):
 p=argparse.ArgumentParser(description=__doc__);sub=p.add_subparsers(dest='operation',required=True)
 q=sub.add_parser('prepare')
 for n in ('image','manifest','session','admission','output','clang','lld','objcopy'):q.add_argument('--'+n,type=Path,required=True)
 q=sub.add_parser('validate');q.add_argument('--bundle',type=Path,required=True)
 q=sub.add_parser('run')
 for n in ('bundle','fel','evidence','ddr-evidence','platform-survey'):q.add_argument('--'+n,type=Path,required=True)
 q.add_argument('--warm-return',type=Path,action='append',default=[]);q.add_argument('--root-owned-claim',required=True,help='Existing root claim; this program does not create or close queue records.')
 a=p.parse_args(argv)
 if a.operation=='prepare':prepare(a.image,a.manifest,a.session,a.admission,a.output,a.clang,a.lld,a.objcopy);return 0
 if a.operation=='validate':validate_bundle(a.bundle);print('Validated exact NPU returning bundle; no hardware access');return 0
 return run(a)
if __name__=='__main__':
 try:sys.exit(main())
 except (OSError,ValueError,RuntimeError,KeyError,struct.error) as error:sys.exit('Stopped: '+str(error))
