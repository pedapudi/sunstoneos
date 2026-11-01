#!/usr/bin/env python3
"""Prepare or capture the exact NPU returning profile; no admission defaults."""
import argparse,hashlib,importlib.util,io,json,re,secrets,shutil,struct,sys,subprocess
from pathlib import Path
from elftools.elf.elffile import ELFFile
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
from host_evidence import new_directory, unique_directory
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
stopped=module('finite_npu_stopped',ROOT/'apps/npu-finite/stopped-capture.py')
original=module('finite_npu_original',ROOT/'apps/npu-convolution/prepare-supervised.py')
base=original.supervisor.base
require,sha=base.require,original.sha
RESTART='npu-interrupt-restart-return-v1'
STARTUP='npu-startup-return-v1'
INFERENCE='npu-single-inference-return-v1'
# The NPU runtime client (build.py --runtime): one job of each linked network through npu_job_run.
RUNTIME='npu-runtime-return-v1'
SWITCH='npu-model-switch-return-v1'
SDK='npu-sdk-return-v1'
POWER='npu-power-cycle-return-v1'
LATENCY='npu-latency-return-v1'
# The startup client's receipt magic, monitor detail, inference jobs and interrupt completions.
STARTUP_CLIENT={STARTUP:(0x4e50555354415231,0x4e505553,0,1),INFERENCE:(0x4e5055494e463031,0x4e505549,1,2),
 RUNTIME:(0x4e50555254523031,0x4e505552,2,3),
 SWITCH:(0x4e50555357433031,0x4e505557,3,4),
 SDK:(0x4e505553444b3031,0x4e505544,2,3),
 LATENCY:(0x4e50554c41543031,0x4e50554c,1001,1002)}
MBUS_CHECKPOINT='npu-entry-mbus-checkpoint-v1'
IRQ_CHECKPOINT='npu-entry-irq-checkpoint-v1'
PROVIDER_CHECKPOINT='npu-entry-provider-checkpoint-v1'
PROFILE='npu-finite-return-v1';CHECKPOINT='npu-entry-memory-checkpoint-v1';THERMAL='npu-finite-thermal-return-v1';PROFILES=(PROFILE,CHECKPOINT,THERMAL,MBUS_CHECKPOINT,IRQ_CHECKPOINT,PROVIDER_CHECKPOINT,RESTART,STARTUP,INFERENCE,RUNTIME,SWITCH,SDK,POWER,LATENCY);SCHEMA='a733-npu-finite-entry-v1';DRAM=(0x41000000,0x1200000)
HOST_SOURCES=('apps/npu-finite/host.py','apps/npu-finite/stopped-capture.py','tools/a733-returning-host.py','tools/a733-crumbs.py','include/sunstone_crumb.h','apps/npu-convolution/prepare-supervised.py','apps/npu-convolution/observe.py','tools/check-a733-smp.py','tools/check-a733-smp-supervised.py','tools/check-a733-smp-observe.py','tools/check-a733-kernel.py','tools/usb-access.py','tools/usb_guard.py','tools/npu-models.json','platform/a733-smp-supervised/launch.c','platform/a733-smp-observe/launch.S','platform/a733-smp-observe/launch.ld','platform/a733-smp-cached/boot.h')
def save(path,value):base.save_json(path,value)

def watchdog_marker(data):
 elf=ELFFile(io.BytesIO(data))
 markers=[x for x in elf.get_section_by_name('.symtab').iter_symbols() if x.name=='os_cpu_return_watchdog' and x['st_shndx']!='SHN_UNDEF']
 require(len(markers)==1 and markers[0]['st_size']==40,'Missing or malformed finite watchdog marker.')
 marker=markers[0];section=elf.get_section(marker['st_shndx']);offset=marker['st_value']-section['sh_addr']
 words=struct.unpack('<5Q',section.data()[offset:offset+40])
 require(words==(0x31544457534f5243,2,0xb1,0x07090000,20),'Finite watchdog marker differs.')
 return {'mode':words[2],'session_seconds':words[4]}

def build_launcher(output,clang,lld,objcopy,*,watchdog_mode=0xb1):
 """Build the explicitly selected finite NPU armed-mode launcher."""
 require(watchdog_mode==0xb1,'Finite NPU watchdog mode differs.')
 for name,path in [('launch.c',ROOT/'platform/a733-smp-supervised/launch.c'),('launch.S',ROOT/'platform/a733-smp-observe/launch.S'),('launch.ld',ROOT/'platform/a733-smp-observe/launch.ld'),('boot.h',ROOT/'platform/a733-smp-cached/boot.h')]:
  (output/name).write_bytes(path.read_bytes())
 flags=['--target=arm-none-eabi','-march=armv8-a','-marm','-mfloat-abi=soft','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-unwind-tables','-fno-asynchronous-unwind-tables','-Os','-g','-Wall','-Wextra','-Werror','-DSUNSTONE_NPU_FINITE_WATCHDOG=1',f'-DA733_SMP_CONTRACT_MAGIC={original.CONTRACT_MAGIC}ULL','-DA733_SUPERVISED_CONTRACT_VERSION=3','-DA733_SUPERVISED_SESSION_BYTES=0','-I'+str(output)]
 commands=[[clang,*flags,'-MD','-MF',output/(name+'.d'),'-c',output/name,'-o',output/(name+'.o')] for name in ('launch.c','launch.S')]
 commands += [[lld,'-T',output/'launch.ld',output/'launch.c.o',output/'launch.S.o','-o',output/'launch.elf'],[objcopy,'-O','binary',output/'launch.elf',output/'launch.bin']]
 for command in commands:
  command=list(map(str,command));result=subprocess.run(command,capture_output=True,text=True)
  with (output/'build.log').open('a') as stream:stream.write(result.stdout+result.stderr)
  require(result.returncode==0,'Finite NPU armed launcher build failed.')
 save(output/'commands.json',[list(map(str,c)) for c in commands])
 require(0<(output/'launch.bin').stat().st_size<=0xc00,'Launcher exceeds SRAM reservation.')


def startup_client(prepared):
 client=STARTUP_CLIENT.get(prepared.get('execution_profile'))
 if prepared.get('geometry',{}).get('sustained'):
  require(prepared.get('execution_profile')==RUNTIME,'Sustained geometry requires runtime profile.')
  return (STARTUP_CLIENT[RUNTIME][0],STARTUP_CLIENT[RUNTIME][1],200,201)
 return client

def sustained_extension(startup_marker,names,read):
 sustained=startup_marker==(STARTUP_CLIENT[RUNTIME][0],1,200,201)
 require(('npu_sustained_return_profile' in names)==sustained,'Sustained extension/startup marker differs.')
 if sustained:
  require(read('npu_sustained_return_profile')==struct.pack('<4Q',0x4e50555355533031,1,200,2),
   'Sustained immutable extension differs.')
  require('npu_finite_reference_2' not in names,'Sustained plan requires exactly two networks.')
 return sustained

def latency_extension(startup_marker,names,read):
 latency=startup_marker==(STARTUP_CLIENT[LATENCY][0],1,1001,1002)
 require(('npu_latency_return_profile' in names)==latency,'Latency extension/startup marker differs.')
 if not latency:return None
 raw=read('npu_latency_return_profile')
 require(len(raw)==64,'Latency immutable extension length differs.')
 words=struct.unpack('<8Q',raw)
 require(words[:2]==(0x4e50554c41545031,1) and words[2] in (0,1) and words[3:]==(1000,1,0,4,24000000),
  'Latency immutable extension differs.')
 return {'mode':'queue' if words[2] else 'legacy','measured_jobs':1000,'warmup_jobs':1,'model':0}


def image_profile(data,manifest):
 watchdog_marker(data)
 parts,address,build,layout=original.image_data(data)
 elf=ELFFile(io.BytesIO(data));symbols={s.name:s for s in elf.get_section_by_name('.symtab').iter_symbols()}
 # Select the parser from native immutable markers rather than a manifest.
 names=set(symbols);sustained=False
 if 'npu_startup_return_profile' in names:
  marker=symbols['npu_startup_return_profile'];start=int(marker['st_value'])
  part=next(p for p in parts if p['address']<=start and start+32<=p['address']+len(p['data']))
  value=struct.unpack_from('<4Q',part['data'],start-part['address'])
  execution=next((name for name,description in STARTUP_CLIENT.items() if value==(description[0],1,description[2],description[3])),None)
  sustained=value==(STARTUP_CLIENT[RUNTIME][0],1,200,201)
  if sustained:execution=RUNTIME
  require(execution is not None,'Unsupported native startup marker.')
 elif 'npu_restart_return_profile' in names:
  marker=symbols['npu_restart_return_profile'];start=int(marker['st_value'])
  part=next(p for p in parts if p['address']<=start and start+32<=p['address']+len(p['data']))
  execution=POWER if struct.unpack_from('<Q',part['data'],start-part['address'])[0]==0x4e50555057523031 else RESTART
 elif 'npu_entry_checkpoint_provider_profile' in names:execution=PROVIDER_CHECKPOINT
 elif 'npu_entry_checkpoint_irq_profile' in names:execution=IRQ_CHECKPOINT
 elif 'npu_entry_checkpoint_mbus_profile' in names:execution=MBUS_CHECKPOINT
 elif 'npu_entry_checkpoint_profile' in names:execution=CHECKPOINT
 elif 'npu_finite_thermal_profile' in names:execution=THERMAL
 else:execution=PROFILE
 manifest={'execution_profile':execution}
 # Finite profiles use ROM transport after return; none starts a
 # native USB provider. STT_FILE names remain after linker collection.
 functions=[s.name for s in elf.get_section_by_name('.symtab').iter_symbols()
  if s['st_info']['type']=='STT_FUNC' and s['st_shndx']!='SHN_UNDEF']
 require(not any('usb' in n.lower() or n.startswith(('tud_','tuh_','dcd_','hcd_')) or
  n=='a733_observe_service_start' for n in functions),'Finite image contains native USB functions.')
 startup=startup_client({'execution_profile':execution,'geometry':{'sustained':sustained}})
 checkpoint=manifest['execution_profile'] in (CHECKPOINT,MBUS_CHECKPOINT,IRQ_CHECKPOINT,PROVIDER_CHECKPOINT);thermal=manifest['execution_profile']==THERMAL
 for name in ('npu_finite_receipt','npu_finite_smc','npu_finite_rom_return', 'npu_entry_checkpoint_ready' if checkpoint else 'a733_npu_application_quiescent'):
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
  address_of('__npu_retained_start')<=receipt<receipt+(112 if thermal else 88)<=address_of('__npu_retained_end') and
  lo<=log<log+16384<=hi,'Returning layout/build identity differs.')
 if checkpoint:
  require(read('npu_entry_checkpoint_profile')==struct.pack('<4Q',0x4e505543484b3031,1,0x4e50554e,0),'Checkpoint immutable marker differs.')
  if manifest['execution_profile']==PROVIDER_CHECKPOINT:
   require(read('npu_entry_checkpoint_provider_profile')==struct.pack('<4Q',0x4e505550524f5631,1,4,0) and
    all(n in symbols for n in ('a733_npu_application_init','npu_application_irq_bind')) and
    'vTaskStartScheduler' not in symbols,'Provider checkpoint implementation differs.')
  else:
   require('npu_entry_checkpoint_provider_profile' not in symbols and all(n not in symbols for n in ('a733_npu_application_init','npu_convolution_startup_poll','npu_service_activate','vTaskStartScheduler')),'Checkpoint contains a provider or scheduler constructor.')
 else:require('npu_entry_checkpoint_profile' not in symbols,'Checkpoint image cannot use numerical profile.')
 latency=None
 if startup:
  require(sustained_extension(value,names,read)==sustained,'Sustained startup selection differs.')
  latency=latency_extension(value,names,read)
 else:require('npu_sustained_return_profile' not in names and 'npu_latency_return_profile' not in names,'Unexpected workload extension.')
 runtime=manifest['execution_profile'] in (RUNTIME,SWITCH,SDK,LATENCY)
 if manifest['execution_profile']==SDK:
  require(all(n in symbols for n in ('ss_npu_run','ss_npu_close','ss_npu_service_step','sdk_service')),'Missing linked SDK owner or public API.')
 if startup:
  engine=('npu_latency_client' if latency else 'npu_runtime_client','npu_job_run','npu_runtime_stop_poll') if runtime else ('npu_startup_client','npu_convolution_stop_poll')
  require(read('npu_startup_return_profile')==struct.pack('<4Q',startup[0],1,startup[2],startup[3]) and
   all(n in symbols for n in ('npu_startup_return_ready','npu_service_activate','vTaskStartScheduler')+engine) and
   'client' not in symbols and (not runtime or 'npu_convolution_startup_poll' not in symbols),'Startup-only implementation differs.')
 else:require('npu_startup_return_profile' not in symbols,'Startup-only image requires its distinct profile.')
 restart=manifest['execution_profile'] in (RESTART,POWER)
 if restart:
  require(read('npu_restart_return_profile')==struct.pack('<4Q',0x4e50555057523031 if manifest['execution_profile']==POWER else 0x4e50555253543031,1,2,4) and
   all(n in symbols and symbols[n]['st_value'] for n in ('npu_convolution_restart_poll','npu_service_restart_prepare','npu_service_restart_resume','npu_application_irq_restart')),
   'Missing exact returning restart implementation.')
 else:require('npu_restart_return_profile' not in symbols,'Restart image requires its distinct profile.')
 # The runtime image compares each network's copied output with its linked
 # reference; network 0 is the image's selected immutable model.
 references=[read(f'npu_finite_reference_{n}').hex() for n in range(2 if sustained or latency else startup[2])] if runtime else []
 if not runtime:require('npu_finite_reference_0' not in symbols,'Only the runtime image links network references.')
 geometry={'clean_start':lo,'clean_end':hi,'receipt':receipt,'log':log,'reference':read('a733_npu_application_reference').hex()}
 require(not runtime or references[0]==geometry['reference'],'Runtime network 0 is not the selected model.')
 if startup:geometry['startup_only']=True
 if runtime:geometry.update(runtime=True,references=references)
 if sustained:geometry['sustained']=True
 if latency:
  geometry['latency']=latency
  require('npu_finite_reference_2' not in names,'Latency plan requires exactly two linked networks.')
  if latency['mode']=='queue':
   require(all(name in symbols for name in ('npu_runtime_queue_claim','npu_runtime_queue_wait','npu_runtime_queue_return','npu_queue_submit','npu_queue_release')),
    'Missing native resident queue implementation.')
 if manifest['execution_profile']==SWITCH:geometry['model_switch']=True
 if manifest['execution_profile']==SDK:geometry['sdk']=True
 if manifest['execution_profile']==INFERENCE:geometry['single_inference']=True
 if restart:geometry['restart']=True
 if manifest['execution_profile']==POWER:
  require(all(n in symbols for n in ('a733_npu_startup_power_off','a733_npu_startup_power_restart')),'Missing domain OFF/ON sequencer.')
  geometry['power_cycle']=True
 if thermal:
  require(read('npu_finite_thermal_profile')==struct.pack('<4Q',0x4e50555448533031,1,160,2) and
   layout[:3]==(616,224,392),'Thermal marker/session layout differs.')
  geometry['thermal']=True
 else:require('npu_finite_thermal_profile' not in symbols and layout[:3]==(456,224,232),'Ordinary profile cannot consume thermal suffix.')
 if checkpoint:geometry['checkpoint']=True
 if manifest['execution_profile'] in (MBUS_CHECKPOINT,IRQ_CHECKPOINT,PROVIDER_CHECKPOINT):
  require(read('npu_entry_checkpoint_mbus_profile')==struct.pack('<4Q',0x4e50554d42555331,1,3,0),
   'MBUS checkpoint immutable marker differs.')
  geometry['checkpoint_mbus']=True
 else:require('npu_entry_checkpoint_mbus_profile' not in symbols,'MBUS checkpoint requires its distinct profile.')
 if manifest['execution_profile'] in (IRQ_CHECKPOINT,PROVIDER_CHECKPOINT):
  require(read('npu_entry_checkpoint_irq_profile')==struct.pack('<4Q',0x4e50554952514331,1,97,0) and
   all(n in symbols and symbols[n]['st_value'] for n in ('npu_irq_route_capture','npu_irq_route_install','npu_irq_route_installed','npu_irq_route_restore')) and
   (manifest['execution_profile']==PROVIDER_CHECKPOINT or 'npu_application_irq' not in symbols),'IRQ checkpoint implementation differs.')
  geometry['checkpoint_irq']=True
  if manifest['execution_profile']==PROVIDER_CHECKPOINT:geometry['checkpoint_provider']=True
 else:require('npu_entry_checkpoint_irq_profile' not in symbols,'IRQ checkpoint requires its distinct profile.')
 capture=stopped.layout(sha(data),symbols,read,lo,hi) if manifest['execution_profile']==PROFILE else None
 if capture is not None:geometry['stopped_capture']=capture
 return parts,address,build,layout,geometry

def geometry_profile(g):return LATENCY if g.get('latency') else POWER if g.get('power_cycle') else SDK if g.get('sdk') else SWITCH if g.get('model_switch') else RUNTIME if g.get('runtime') else INFERENCE if g.get('single_inference') else STARTUP if g.get('startup_only') else PROVIDER_CHECKPOINT if g.get('checkpoint_provider') else IRQ_CHECKPOINT if g.get('checkpoint_irq') else MBUS_CHECKPOINT if g.get('checkpoint_mbus') else RESTART if g.get('restart') else CHECKPOINT if g.get('checkpoint') else THERMAL if g.get('thermal') else PROFILE

def validate_admission(admission_raw,raw,build,layout,image_hash,cache=None,*,execution_profile=PROFILE):
 """Check the actual native wire record without external process evidence."""
 require(execution_profile in PROFILES,'Unsupported execution profile.')
 profile=original.session_profile(raw,layout)
 require(struct.unpack_from('<Q',raw,8)[0]==original.SINGLE_CORE_VERSION and raw[48:80]==build,
         'Native session version or build differs.')
 require(all(any(raw[at:at+32]) for at in (80,112,144,176)),'Missing native session digest.')
 if execution_profile==THERMAL:
  require(layout[:3]==(616,224,392),'Thermal session layout differs.')
  at=layout[1]+232;owner,authority=struct.unpack_from('<2Q',raw,at)
  require(owner and authority and owner not in [profile[k] for k in ('npu_owner','pmic_owner','usb_owner','bridge_owner')],
          'Thermal owner differs.')
  require(all(any(raw[at+16+32*i:at+48+32*i]) for i in range(4)),'Missing thermal configuration digest.')
  apb,hz,configure=struct.unpack_from('<2IB',raw,at+144);rate={0:24000000,3:600000000}.get(apb>>24,0);div=(apb&31)+1
  require(not apb&~0x0300001f and rate and rate%div==0 and hz==rate//div and configure in (0,1) and raw[at+153:at+160]==bytes(7),
          'Unsupported polling thermal configuration.')
 return profile,{}

def construct_session(image,manifest,boot,admission,output):
 """Serialize typed boot bytes and fresh native session identities."""
 data=image.read_bytes();_,_,build,layout,geometry=image_profile(data,{})
 typed=boot.read_bytes();size,offset,boot_bytes=layout[:3]
 require(offset==224 and size==offset+boot_bytes and len(typed)==boot_bytes,'Typed boot length differs.')
 record=json.loads(admission.read_bytes()) if admission else {}
 identities=[record.get(k,secrets.randbits(64) or 1) for k in ('epoch','generation','lease','supervisor','whole_platform')]
 require(all(type(v) is int and 0<v<2**64 for v in identities),'Nonzero native session identities required.')
 raw=bytearray(size);struct.pack_into('<6Q',raw,0,original.SESSION_MAGIC,original.SINGLE_CORE_VERSION,size,*identities[:3])
 raw[48:80]=build
 for name,at in (('entry',80),('clock',112),('power',144),('supervision',176)):
  raw[at:at+32]=hashlib.sha256(name.encode()+typed+build).digest()
 struct.pack_into('<2Q',raw,208,*identities[3:]);raw[offset:]=typed
 validate_admission(b'',bytes(raw),build,layout,sha(data),execution_profile=geometry_profile(geometry))
 output=Path(output)
 with output.open('xb') as stream:stream.write(raw)
 return output

def prepare(image,manifest,session,admission,output,clang,lld,objcopy):
 data=image.read_bytes();parts,address,build,layout,geometry=image_profile(data,{});raw=session.read_bytes();entry=b'{}'
 execution=geometry_profile(geometry)
 profile,cache=validate_admission(entry,raw,build,layout,sha(data),execution_profile=execution);output=new_directory(output)
 for name,source in [('image.elf',image),('session.bin',session)]:shutil.copy2(source,output/name)
 save(output/'manifest.json',{'execution_profile':execution,'build_id':build.hex(),'final_elf':sha(data)})
 save(output/'admission.json',{})
 build_launcher(output,clang,lld,objcopy)
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
 runtime_names={'image.elf','launch.bin','session.bin','request.bin','contract.bin','result-zero.bin','report-zero.bin'}|{part['name'] for part in segments}
 files={name:sha((output/name).read_bytes()) for name in sorted(runtime_names)}
 result={'schema':'a733-npu-finite-bundle-v1','execution_profile':execution,'physical_access':False,'image_sha256':sha(data),'build_id':build.hex(),'session_layout':list(layout),'geometry':geometry,'segments':segments,'entry_evidence':refs,'output':str(output),'files':files,**profile}
 save(output/'prepared.json',result);return result

def validate_bundle(bundle):
 p=json.loads((bundle/'prepared.json').read_bytes());require(p.get('schema')=='a733-npu-finite-bundle-v1' and p.get('execution_profile') in PROFILES,'Wrong returning bundle.')
 frozen={'image.elf':(bundle/'image.elf').read_bytes()}
 parts,address,build,layout,geometry=image_profile(frozen['image.elf'],{})
 names={'image.elf','launch.bin','session.bin','request.bin','contract.bin','result-zero.bin','report-zero.bin'}|{f'segment-{i}.bin' for i,part in enumerate(parts) if part['data']}
 for name in sorted(names):
  if name not in frozen:frozen[name]=(bundle/name).read_bytes()
  require(sha(frozen[name])==p['files'][name],'Bundle file changed: '+name)
 p['files']={name:p['files'][name] for name in sorted(names)}
 profile,_=validate_admission(b'',frozen['session.bin'],build,layout,sha(frozen['image.elf']),execution_profile=geometry_profile(geometry))
 require(p['execution_profile']==geometry_profile(geometry) and all(p.get(k)==v for k,v in profile.items()) and (p['geometry']==geometry or p['geometry']=={k:v for k,v in geometry.items() if k!='stopped_capture'}) and p['build_id']==build.hex() and p['image_sha256']==sha(frozen['image.elf']),'Prepared metadata differs.')
 expected=[]
 for i,part in enumerate(parts):
  if part['data']:
   name=f'segment-{i}.bin';require(frozen[name]==part['data'],'Segment differs.');expected.append({'name':name,'address':part['address']})
 expected.append({'name':'session.bin','address':0x41f00000});require(p['segments']==expected,'Upload addresses differ.')
 request,contract=original.request_bytes(address,build,frozen['session.bin'],profile)
 for name,value in [('request.bin',request),('contract.bin',contract),('result-zero.bin',bytes(16384)),('report-zero.bin',bytes(128))]:require(frozen[name]==value,'Launch packet differs: '+name)
 require(0<len(frozen['launch.bin'])<=0xc00,'Invalid launcher length.')
 p['_frozen']=frozen;p['_sizes']={k:len(v) for k,v in frozen.items()};return p

# SUBMIT fills each job's device output with NPU_CONVOLUTION_OUTPUT_POISON
# (include/npu_convolution.h), little-endian and repeated from offset zero.
OUTPUT_POISON=0xc35a3ca5
def output_poison(size):return (OUTPUT_POISON.to_bytes(4,'little')*(size//4+1))[:size]
def output_pattern(first_digest,second):
 """Copied output bytes that still hold the SUBMIT pattern, as [start, end)
 ranges. A short range can match by chance; a range covering the output means
 the job wrote nothing. Only images whose SUBMIT fills the pattern give these
 fields meaning. The first job returns only its digest, which identifies an
 output that kept the pattern in every byte."""
 pattern=output_poison(len(second));ranges=[];start=None
 for i in range(len(second)+1):
  same=i<len(second) and second[i]==pattern[i]
  if same and start is None:start=i
  elif not same and start is not None:ranges.append([start,i]);start=None
 return {'second_pattern_bytes':sum(b-a for a,b in ranges),'second_pattern_ranges':ranges,
  'second_unwritten':bool(second) and ranges==[[0,len(second)]],
  'first_unwritten':bytes(first_digest)==hashlib.sha256(pattern).digest()}

def timing(text):
 """Startup entry to READY, in the startup's counter clock, and each job's
 publication to completion observation, in the NPU adapter's clock, from the
 image's NPU_TIME_US block; None for an image that prints none. A job whose
 completion reads 0 or precedes its publication has no time; this never
 refuses an otherwise valid report."""
 found=re.findall(r'NPU_TIME_US=\n((?:0x[0-9a-f]{16}\n){6})',text)
 if len(found)!=1:return None
 entry,ready,*jobs=[int(v,16) for v in found[0].split()]
 return {'startup_us':ready-entry if ready>=entry else None,
  'job_us':[jobs[i+1]-jobs[i] if jobs[i] and jobs[i+1]>=jobs[i] else None for i in (0,2)],
  'publication_gap_us':jobs[2]-jobs[0] if jobs[2]>=jobs[0]>0 else None,
  'entry_us':entry,'ready_us':ready,'jobs_raw_us':jobs,'counters':job_counters(text),
  'resolution':'Completion is observed at the service worker\'s next poll, once per 1 ms tick, or on INTID97.'}

def job_counters(text):
 """Each job's system-counter and NPU cycle-counter record from the image's
 NPU_JOB_COUNTERS block, or None for an image that prints none. Values that
 are 0 or out of order give no time; this never refuses a report."""
 found=re.findall(r'NPU_JOB_COUNTERS=\n((?:0x[0-9a-f]{16}\n){11})',text)
 if len(found)!=1:return None
 frequency,*values=[int(v,16) for v in found[0].split()]
 span=lambda a,b:(b-a)*1000000/frequency if frequency and a and b>=a else None
 jobs=[]
 for start,irq,done,total,idle in (values[0:5],values[5:10]):
  jobs.append({'start_ticks':start,'irq_ticks':irq,'done_ticks':done,
   'start_to_irq_us':span(start,irq),'start_to_done_us':span(start,done),
   'total_cycles':total,'idle_cycles':idle,'active_cycles':total-idle if total>=idle else None})
 return {'frequency':frequency,'jobs':jobs}


def validate_reports(monitor,platform,receipt,p):
 thermal=p.get('execution_profile')==THERMAL;startup=startup_client(p)
 require(len(monitor)==128 and len(platform)==16384 and len(receipt)==(112 if thermal else 88),'Incomplete returned reports.')
 m=struct.unpack('<16Q',monitor);h=struct.unpack_from('<16Q',platform);r=struct.unpack('<14Q' if thermal else '<11Q',receipt);g=p['geometry']
 magic=0x415337334e505231
 require(m[0]==magic and (m[1],m[2])==(p['epoch'],p['generation']) and m[3]==5 and m[4] in ((0,1,2) if thermal else (0,1)) and m[5]==(0x4e50554e if p.get('execution_profile') in (CHECKPOINT,MBUS_CHECKPOINT,IRQ_CHECKPOINT,PROVIDER_CHECKPOINT) else startup[1] if startup else 0) and m[7]==1,'No matched normal monitor completion.')
 status=m[4];deadline=r[9];armed=deadline-480000000
 checkpoint=p.get('execution_profile') in (CHECKPOINT,MBUS_CHECKPOINT,IRQ_CHECKPOINT,PROVIDER_CHECKPOINT)
 require(p.get('execution_profile',PROFILE) in PROFILES and bool(g.get('checkpoint'))==checkpoint and bool(g.get('thermal'))==thermal,'Report profile differs.')
 require(bool(g.get('latency'))==(p.get('execution_profile')==LATENCY),'Latency report profile differs.')
 require(r[:6]==(0x4e505543484b3031 if checkpoint else 0x4e50555448533031 if thermal else startup[0] if startup else 0x4e50555245543031,p['epoch'],p['generation'],p['lease'],status,0x4e50554e if checkpoint else status+1) and
  r[6:9]==(g['clean_start'],g['clean_end'],(g['clean_end']-g['clean_start'])//64) and
  0<=armed<=m[6]<armed+24000000 and m[6]<=m[8]<deadline,'Receipt identity/cleanup/time differs.')
 require(h[0]==0x534b4552 and h[1]==5 and h[2]==status and h[9]==m[8] and h[3]==r[10] and h[3]<=16127 and m[6]<=h[8]<=h[9] and h[4:8]==(0,0,0,0),'Platform completion/log differs.')
 text=platform[256:256+h[3]].decode('ascii');require(platform[256+h[3]]==0,'Unterminated returned log.')
 if checkpoint:
  prefix=('NPU_CHECKPOINT=PROVIDERS_CONSTRUCTED; scheduler never started; jobs=0\n' if p.get('execution_profile')==PROVIDER_CHECKPOINT else 'NPU_CHECKPOINT=ENTRY_MEMORY_READY; providers never constructed; jobs=0\n')
  mbus={}
  if p.get('execution_profile') in (MBUS_CHECKPOINT,IRQ_CHECKPOINT,PROVIDER_CHECKPOINT):
   match=re.fullmatch(re.escape(prefix)+r'NPU_MBUS=\n(0x[0-9a-f]{16})\n(0x[0-9a-f]{16})\n(0x[0-9a-f]{16})\n',text)
   require(match is not None,'Missing or ambiguous MBUS checkpoint log.')
   before,result,after=(int(v,16) for v in match.groups())
   require(before<=0xffffffff and after<=0xffffffff and result in (0,8,9,10),'Invalid MBUS checkpoint result.')
   mbus={'mbus':{'before':before,'restore_result':result,'after':after}}
  else:require(text==prefix,'Checkpoint log or status differs.')
  require(status==0,'Checkpoint log or status differs.')
  return {'normal_return':True,'checkpoint':('PROVIDERS_CONSTRUCTED' if p.get('execution_profile')==PROVIDER_CHECKPOINT else 'ENTRY_MEMORY_READY'),'functional_success':False,'controlled_negative':False,'jobs':0,'status':0,'diagnostic_deadline':deadline,'log':text,**mbus}
 if g.get('latency'):
  result=latency_report(text,status,deadline,g)
  require(m[6]<=result['timing']['start_ticks']<result['timing']['end_ticks']<=h[9],
   'Latency timing escapes native report.')
  return result
 if g.get('sustained'):
  result=sustained_report(text,status,deadline,g)
  ticks=result['timing']
  require(m[6]<=ticks['start_ticks']<ticks['end_ticks']<=h[9],'Sustained timing escapes native report.')
  return result
 if g.get('runtime'):return runtime_report(text,status,deadline,g)
 if startup and startup[2]:return inference_report(text,status,deadline,g)
 if startup:
  expected=('NPU_STAGE=WAIT_STARTUP\nNPU_STAGE=OPEN\nNPU_STAGE=BOOTSTRAP_READY\nNPU_STAGE=CLOSE\nNPU_STAGE=WAIT_LOCAL_STOP\n'
   'NPU_IRQ_COMPLETIONS=0x0000000000000001\nNPU_IRQ_STOPPED=0x0000000000000001\n'
   'NPU_STAGE=QUIESCENT\nNPU_STARTUP=READY_CLOSED_STOPPED; inference_jobs=0\n')
  require(status==0 and text==expected,'Startup-only stage, job or IRQ result differs.')
  return {'normal_return':True,'startup_bootstrap':True,'functional_success':False,'controlled_negative':False,'jobs':0,'status':0,'diagnostic_deadline':deadline,'log':text}
 power=p.get('execution_profile')==POWER
 passed='PASS NPU two matched jobs across domain OFF/restart and parked providers' if power else 'PASS NPU two matched jobs across normal local restart and parked providers' if p.get('execution_profile')==RESTART else 'PASS NPU two matched jobs, copied output, REAP, CLOSE, local stop and parked providers'
 if power:
  samples=[]
  for name,expected in (('OFF',0),('ON',8)):
   fields=re.findall(r'NPU_POWER_CYCLE_'+name+r'=\n(0x[0-9a-f]{16})\n(0x[0-9a-f]{16})\n(0x[0-9a-f]{16})\n',text)
   require(len(fields)==1 and tuple(int(v,16) for v in fields[0][:2])==(expected,expected) and 0<int(fields[0][2],16)<20000000,'Missing or invalid physical power transition.')
   samples.append({'policy_nibble':expected,'status_nibble':expected,'request_to_observation_us':int(fields[0][2],16)})
 pattern=r'NPU_STAGE=QUIESCENT\nNPU_FIRST_DIGEST=\n((?:0x[0-9a-f]{16}\n){32})NPU_SECOND_OUTPUT=\n((?:0x[0-9a-f]{16}\n)+)('+re.escape(passed)+r'|FAIL NPU numerical comparison|REFUSED NPU thermal admission; configured sensor disabled and providers parked)\n'
 matches=list(re.finditer(pattern,text));require(len(matches)==1 and matches[0].end()==len(text),'Missing or ambiguous finite output/terminal marker.')
 values=lambda raw:[int(x,16) for x in raw.splitlines()]
 first,second=values(matches[0][1]),values(matches[0][2]);require(all(v<256 for v in first+second),'Invalid copied output byte.')
 reference=bytes.fromhex(g['reference']);require(len(second)==len(reference),'Copied output extent differs.')
 matched=bytes(first)==hashlib.sha256(reference).digest() and bytes(second)==reference
 if thermal:
  reason,jobs,thermal_deadline=r[11:14]
  require(m[8]<thermal_deadline<=deadline,'Thermal cleanup deadline differs.')
  require(reason<=3 and jobs<=2 and (status!=0 or jobs==2) and
   (status!=2 or reason>0 and jobs<2) and (status!=1 or jobs>0),'Invalid thermal completion fields.')
  thermal_markers=list(re.finditer(r'NPU_THERMAL_REASON=(0x[0-9a-f]{16})\nNPU_JOBS=(0x[0-9a-f]{16})\n',text))
  require(len(thermal_markers)==1 and thermal_markers[0].end()==matches[0].start() and
   tuple(int(v,16) for v in thermal_markers[0].groups())==(reason,jobs),'Thermal log/receipt differs.')
  if jobs==0:require(bytes(first)==bytes(32) and bytes(second)==bytes(len(reference)),'Never-started outputs differ.');completed_match=True
  elif jobs==1:
   require(bytes(first)==hashlib.sha256(bytes(second)).digest(),'Single completed output digest differs.')
   completed_match=bytes(second)==reference
  else:completed_match=matched
  require((status==1)==(not completed_match) and
   matches[0][3].startswith('PASS' if status==0 else 'FAIL' if status==1 else 'REFUSED'),'Thermal status disagrees with completed numerical work.')
  return {'normal_return':True,'functional_success':status==0,'controlled_negative':status==1,
   'thermal_refusal':status==2,'thermal_reason':reason,'jobs':jobs,'status':status,'diagnostic_deadline':deadline,
   'first_digest':bytes(first).hex(),'last_output':bytes(second).hex(),'output_pattern':output_pattern(first,second),
   'timing':timing(text),'log':text}
 require(matched==(status==0) and matches[0][3].startswith('PASS')==(status==0),'Numerical status disagrees with copied outputs.')
 return {'normal_return':True,'functional_success':matched,'controlled_negative':not matched,'status':status,'diagnostic_deadline':deadline,'first_digest':bytes(first).hex(),'second_output':bytes(second).hex(),'output_pattern':output_pattern(first,second),'timing':timing(text),'log':text,**({'power_cycle':samples} if power else {})}

def inference_report(text,status,deadline,g):
 """The single-inference log: startup, one job, CLOSE, then the copied output.
 Status 0 requires the output to equal the image's reference; status 1 is a
 normal return whose output differs and is reported, not refused."""
 head=('NPU_STAGE=WAIT_STARTUP\nNPU_STAGE=OPEN\nNPU_STAGE=BOOTSTRAP_READY\nNPU_STAGE=SUBMIT job=0x0000000000000001\n'
  'NPU_STAGE=REAPED\nNPU_STAGE=CLOSE\nNPU_STAGE=WAIT_LOCAL_STOP\n'
  'NPU_IRQ_COMPLETIONS=0x0000000000000002\nNPU_IRQ_STOPPED=0x0000000000000001\nNPU_STAGE=QUIESCENT\nNPU_OUTPUT=\n')
 match=re.fullmatch(re.escape(head)+r'((?:0x[0-9a-f]{16}\n)+)NPU_INFERENCE=(MATCH|MISMATCH); inference_jobs=1\n',text)
 require(match is not None,'Single-inference stage, job, IRQ or output log differs.')
 output=[int(x,16) for x in match[1].splitlines()];require(all(v<256 for v in output),'Invalid copied output byte.')
 reference=bytes.fromhex(g['reference']);require(len(output)==len(reference),'Copied output extent differs.')
 matched=bytes(output)==reference
 require(matched==(status==0) and (match[2]=='MATCH')==matched,'Inference status disagrees with copied output.')
 return {'normal_return':True,'single_inference':True,'functional_success':matched,'controlled_negative':not matched,
  'jobs':1,'status':status,'diagnostic_deadline':deadline,'output':bytes(output).hex(),
  'output_pattern':{k:v for k,v in output_pattern(b'',bytes(output)).items() if k!='first_unwritten'},'log':text}

def latency_report(text,status,deadline,g):
 """Check every measured latency and the digest of every checked output.

 The legacy interval covers npu_job_run. The queue interval covers input copy,
 output poison, tensor cache work, submit, interrupt wait, acquire and release.
 Output comparison and hashing follow each interval and enter total time.
 """
 geometry=g.get('latency')
 require(g.get('runtime') and len(g['references'])==2 and geometry and
  geometry=={'mode':geometry['mode'],'measured_jobs':1000,'warmup_jobs':1,'model':0} and
  geometry['mode'] in ('legacy','queue'),'Latency network geometry differs.')
 head=('NPU_STAGE=WAIT_STARTUP\nNPU_STAGE=WARMUP\nNPU_STAGE=LATENCY\nNPU_STAGE=CLOSE\n'
  'NPU_STAGE=WAIT_LOCAL_STOP\nNPU_IRQ_COMPLETIONS=0x00000000000003ea\n'
  'NPU_IRQ_STOPPED=0x0000000000000001\nNPU_STAGE=QUIESCENT\nNPU_LATENCY=\n')
 pattern=(re.escape(head)+r'((?:0x[0-9a-f]{16}\n){21})NPU_DURATION_LE32=\n'
  r'((?:[0-9a-f]{128}\n){62}[0-9a-f]{64}\n)NPU_DURATION_SHA256=\n([0-9a-f]{64})\n'
  r'NPU_OUTPUT_SHA256=\n([0-9a-f]{64})\nNPU_INFERENCE=(MATCH|MISMATCH); inference_jobs=1001\n')
 match=re.fullmatch(pattern,text)
 require(match is not None,'Latency stage, sample, IRQ or output log differs.')
 (frequency,mode,jobs,warmups,total,checked,bad,start,end,p50,p99,maximum,total_ticks,
  submit_ticks,acquire_ticks,first_bad,offset,value,submitted,finished,irq_ticks)=(int(v,16) for v in match[1].split())
 require(frequency==24000000 and mode==int(geometry['mode']=='queue') and
  jobs==checked==1000 and warmups==1 and total==1001 and bad<=jobs,
  'Latency mode, warmup or job/check counters differ.')
 require(submitted==finished==(1000 if mode else 0),'Latency direct fence counters differ.')
 raw=bytes.fromhex(match[2]);durations=list(struct.unpack('<1000I',raw));ordered=sorted(durations)
 require(hashlib.sha256(raw).hexdigest()==match[3],'Latency sample checksum differs.')
 require(all(0<t<=frequency for t in durations) and total_ticks==sum(durations) and
  (p50,p99,maximum)==(ordered[499],ordered[989],ordered[999]),'Latency samples or quantiles differ.')
 require(deadline-480000000<=start<end<deadline and total_ticks<=end-start and
  submit_ticks+acquire_ticks+irq_ticks<=total_ticks and (mode or submit_ticks==acquire_ticks==irq_ticks==0),
  'Latency timing intervals or caller phases differ.')
 reference=bytes.fromhex(g['references'][0]);digest=match[4]
 expected=hashlib.sha256(hashlib.sha256(reference).digest()*1000).hexdigest()
 matched=digest==expected
 require(matched==(bad==0)==(status==0) and (match[5]=='MATCH')==matched,
  'Latency status disagrees with output checks/hash.')
 sentinel=(1<<64)-1
 if not bad:require((first_bad,offset,value)==(sentinel,sentinel,sentinel),'Unexpected latency mismatch detail.')
 else:require(first_bad<1000 and offset<len(reference) and value<256 and value!=reference[offset],
  'Latency first mismatch detail differs.')
 return {'normal_return':True,'runtime':True,'latency':geometry,'functional_success':matched,
  'controlled_negative':not matched,'jobs':total,'measured_jobs':jobs,'warmup_jobs':warmups,
  'outputs_checked':checked,'mismatches':bad,'status':status,'diagnostic_deadline':deadline,
  'first_mismatch':None if not bad else {'job':first_bad,'offset':offset,'byte':value},
  'output_digest':digest,'duration_digest':match[3],
  'timing':{'frequency_hz':frequency,'start_ticks':start,'end_ticks':end,'duration_ticks':end-start,
   'duration_seconds':(end-start)/frequency,'jobs_per_second':jobs*frequency/(end-start),
   'latency_ticks':{'p50':p50,'p99':p99,'max':maximum},
   'latency_us':{k:v*1000000/frequency for k,v in {'p50':p50,'p99':p99,'max':maximum}.items()},
   'job_duration_ticks':durations,'warmup_excluded':True,
   'window':'legacy helper call' if not mode else 'input copy, output poison, tensor cache work, submit, interrupt wait, output acquire and record release',
   'total_window':'measured jobs including output comparison, digest and progress checks; warmup and close excluded',
   'caller_phases':None if not mode else {'submit_elapsed_ticks':submit_ticks,'acquire_elapsed_ticks':acquire_ticks,
    'irq_driver_elapsed_ticks':irq_ticks,
    'scope':'submit under local IRQ lock; output invalidate and release; direct IRQ driver including notification; wait, scheduler and preparation excluded; interrupts and preemption may contribute; CPU utilization is not measured'}},'log':text}


def sustained_report(text,status,deadline,g):
 """Validate all 200 copied-output hashes and API latency windows in ticks."""
 require(g.get('runtime') and len(g['references'])==2,'Sustained network geometry differs.')
 head=('NPU_STAGE=WAIT_STARTUP\nNPU_STAGE=SUSTAINED\nNPU_STAGE=CLOSE\n'
  'NPU_STAGE=WAIT_LOCAL_STOP\nNPU_IRQ_COMPLETIONS=0x00000000000000c9\n'
  'NPU_IRQ_STOPPED=0x0000000000000001\nNPU_STAGE=QUIESCENT\nNPU_SUSTAINED=\n')
 word=r'0x[0-9a-f]{16}'
 pattern=re.escape(head)+r'((?:'+word+r'\n){12})NPU_JOB_TICKS=\n((?:'+word+' '+word+r'\n){200})NPU_OUTPUT_DIGEST=\n((?:'+word+r'\n){32})NPU_INFERENCE=(MATCH|MISMATCH); inference_jobs=200\n'
 match=re.fullmatch(pattern,text)
 require(match is not None,'Sustained stage, job, IRQ or output log differs.')
 frequency,start,end,maximum,jobs,checked,bad,first_count,second_count,first_bad,offset,value=(int(v,16) for v in match[1].split())
 require(frequency==24000000 and jobs==checked==200 and first_count==second_count==100 and 0<=bad<=200,
  'Sustained job/check/model counters differ.')
 windows=[tuple(int(v,16) for v in line.split()) for line in match[2].splitlines()]
 require(start==windows[0][0] and deadline-480000000<=start<end<deadline and
  all(start<=a<b<=end and b-a<=frequency for a,b in windows) and
  all(windows[n-1][1]<=windows[n][0] for n in range(1,200)) and
  maximum==max(b-a for a,b in windows),'Sustained timing windows or maximum differ.')
 raw=[int(v,16) for v in match[3].split()]
 require(all(v<256 for v in raw),'Sustained digest byte differs.')
 digest=bytes(raw).hex();references=[bytes.fromhex(v) for v in g['references']]
 expected=hashlib.sha256(b''.join(hashlib.sha256(references[n%2]).digest() for n in range(200))).hexdigest()
 matched=digest==expected
 require(matched==(bad==0)==(status==0) and (match[4]=='MATCH')==matched,
  'Sustained status disagrees with copied output checks/hash.')
 sentinel=(1<<64)-1
 if not bad:require((first_bad,offset,value)==(sentinel,sentinel,sentinel),'Unexpected sustained mismatch detail.')
 else:require(first_bad<200 and offset<len(references[first_bad%2]) and value<256 and
  value!=references[first_bad%2][offset],'Sustained first mismatch detail differs.')
 return {'normal_return':True,'runtime':True,'sustained':True,'functional_success':matched,
  'controlled_negative':not matched,'jobs':jobs,'outputs_checked':checked,'mismatches':bad,
  'model_jobs':[first_count,second_count],'first_mismatch':None if not bad else {'job':first_bad,'offset':offset,'byte':value},
  'status':status,'diagnostic_deadline':deadline,'output_digest':digest,
  'timing':{'frequency_hz':frequency,'start_ticks':start,'end_ticks':end,'duration_ticks':end-start,
   'duration_seconds':(end-start)/frequency,'jobs_per_second':jobs*frequency/(end-start),
   'maximum_api_ticks':maximum,'maximum_api_us':maximum*1000000/frequency,
   'job_windows_ticks':windows,'window':'npu_job_run call; total includes every copied-output comparison and digest'},'log':text}


def runtime_report(text,status,deadline,g):
 """The runtime client's log: startup, one job of each network through
 npu_job_run, CLOSE, then each copied output in network order. Status 0
 requires every output to equal its network's reference; status 1 is a normal
 return with at least one differing output, reported and not refused."""
 references=[bytes.fromhex(x) for x in g['references']];jobs=len(references)
 head=('NPU_STAGE=WAIT_STARTUP\n'+''.join(f'NPU_STAGE=JOB network=0x{n:016x}\n' for n in range(jobs))+
  'NPU_STAGE=CLOSE\nNPU_STAGE=WAIT_LOCAL_STOP\n'
  f'NPU_IRQ_COMPLETIONS=0x{1+jobs:016x}\nNPU_IRQ_STOPPED=0x0000000000000001\nNPU_STAGE=QUIESCENT\n')
 compact=g.get('model_switch',False)
 label='NPU_OUTPUT_HEX' if compact else 'NPU_OUTPUT'
 encoded=r'((?:(?:[0-9a-f]{2}){1,64}\n)+)' if compact else r'((?:0x[0-9a-f]{16}\n)+)'
 body=''.join(re.escape(f'{label} network=0x{n:016x}\n')+encoded for n in range(jobs))
 match=re.fullmatch(re.escape(head)+body+r'NPU_INFERENCE=(MATCH|MISMATCH); inference_jobs='+str(jobs)+r'\n',text)
 require(match is not None,'Runtime stage, job, IRQ or output log differs.')
 outputs=[bytes.fromhex(match[n+1]) if compact else bytes(int(x,16) for x in match[n+1].splitlines()) if all(int(x,16)<256 for x in match[n+1].splitlines()) else None for n in range(jobs)]
 require(all(o is not None for o in outputs),'Invalid copied output byte.')
 require([len(o) for o in outputs]==[len(r) for r in references],'Copied output extent differs.')
 matches=[o==r for o,r in zip(outputs,references)];matched=all(matches)
 require(matched==(status==0) and (match[jobs+1]=='MATCH')==matched,'Inference status disagrees with copied outputs.')
 return {'normal_return':True,'runtime':True,'functional_success':matched,'controlled_negative':not matched,
  'jobs':jobs,'status':status,'diagnostic_deadline':deadline,'outputs':[o.hex() for o in outputs],'network_matches':matches,'log':text}

def transfer(transaction,address,data,destination):
 with base.sealed_source(data) as (source,fd):transaction('write',hex(address),source,'read',hex(address),len(data),destination,pass_fds=(fd,))
 require(destination.read_bytes()==data,'Restoration readback differs.');base.sync_file(destination)

def require_entry_state(transaction):
 values=[int(v,16) for v in transaction(*[a for address in ('0x02050014','0x070501c0','0x070501d4','0x03000008','0x02002b04','0x02002b00') for a in ('readl',address)]).split()]
 require(len(values)==6 and not values[0]&1 and values[1:3]==[0,0], 'Watchdog or reset redirect enabled before execution.')
 require(values[3]&2 and not values[4]&0x70001 and not values[5]&0x80000000,'NPU SRAM/clock/reset entry state differs.')

def run_once(transaction,launch,ready,bundle,p,evidence,record,save):
 """Root owns the lock/claim; every transport failure is terminal to this call."""
 require(not record.get('launch_attempted'),'One attempt only.');ready()
 require_entry_state(transaction)
 base.verify_dram(transaction,evidence,record,save)
 backup=evidence/'before-DRAM.bin';transaction('read',hex(DRAM[0]),DRAM[1],backup)
 require(backup.stat().st_size==DRAM[1],'Incomplete full DRAM backup.');base.sync_file(backup);backup_hash=base.digest(backup)
 record.update(dram_backup_sha256=backup_hash,dram_restored=False);save()
 record['dirty']=True;save();base.preload(transaction,bundle,p,evidence)
 # The linked return tail sets RVBAR=0x20, bypassing the normal boot input.
 # readl uses SRAM scratch, so preserve it through preload before this batch.
 words=[int(v,16) for v in transaction(*[a for address in ('0x20','0x24','0x28','0x3c','0x40') for a in ('readl',address)]).split()]
 require(words==[0xeb00008f,0xe3a00911,0xe3a0193d,0xe3a0605c,0xea00004d],'Forced-FEL ROM entry differs from the reviewed instructions.')
 # The shared returning host arms WDT0 inside launch(), after these checks.
 # Require a fresh CPU-route/held-reset baseline after any whole-system reset;
 # DDR evidence is separately validated before this DRAM preload is allowed.
 ready()
 require_entry_state(transaction)
 record.update(backup_sha256=p['_backup_hashes'].copy());save();launch()
 require('soc=00001903(A733)' in transaction('version'),'Returned service is not A733 ROM.')
 reports=[]
 for name,address,size in [('monitor',0x42000000,128),('platform',p['geometry']['log'],16384),('receipt',p['geometry']['receipt'],112 if p['execution_profile']==THERMAL else 88)]:
  path=evidence/(name+'.bin');transaction('read',hex(address),size,path);require(path.stat().st_size==size,'Short '+name+' report.');base.sync_file(path);reports.append(path.read_bytes())
 result=validate_reports(*reports,p);record['npu_result']=result;record['result']={'magic':0x534b4552,'stage':5,'status':result['status']};save()
 stopped.capture(transaction,base,p,evidence,record,save,result)
 # No exception/report failure can reach restoration. The new ROM owns SRAM.
 restoration=backup.read_bytes();require(sha(restoration)==backup_hash,'Saved DRAM changed.');transfer(transaction,DRAM[0],restoration,evidence/'DRAM-restored.bin')
 if result['status']:
  # Publish continuity only after the complete saved DRAM has been restored.
  # The nonzero result and failed functional result remain unchanged.
  kernel=module('finite_npu_controlled',ROOT/'tools/check-a733-kernel.py')
  terminal=f"NPU_INFERENCE=MISMATCH; inference_jobs={result['jobs']}" if result.get('runtime') else 'NPU_INFERENCE=MISMATCH; inference_jobs=1' if result.get('single_inference') else 'FAIL NPU numerical comparison' if result['status']==1 else 'REFUSED NPU thermal admission; configured sensor disabled and providers parked'
  markers=['NPU_STAGE=QUIESCENT',terminal]
  controlled={'outcome':{'name':'npu-numerical-mismatch-after-quiescence' if result['status']==1 else 'npu-thermal-refusal-after-quiescence',
   'status':result['status'],'interrupt':None,'fail_lines':0,'fault':None,'log_contains':markers},
   'faults':kernel.fault_lines(result['log']),'log_found':[text for text in markers if text in result['log']]}
  require(kernel.controlled_return({**record,'controlled':controlled}),'Controlled return differs from validated reports.')
  record['controlled']=controlled
 record.update(complete=True,dirty=False,dram_restored=True,rom_usable=True,sram_restored=False,ownership_release_permitted=True);save();return result

returning=module('npu_returning_transport',ROOT/'tools/a733-returning-host.py')
def retain_failure(record,save,quarantine,error,hold=base.hold_ownership):
 return returning.retain_failure(base,record,save,quarantine,error,hold=hold)

def run(args):
 kernel=module('finite_npu_kernel',ROOT/'tools/check-a733-kernel.py')
 def admit(args,p,node,identity,port):
  validate_bundle(args.bundle.resolve())
 def execute(transaction,launch,ready,p,record,journal):
  return run_once(transaction,launch,ready,args.bundle.resolve(),p,args.evidence,record,journal)
 execution=json.loads((args.bundle/'prepared.json').read_bytes()).get('execution_profile')
 require(execution in PROFILES,'Unsupported execution profile.')
 result=returning.run(args,base=base,profile=execution,validate_bundle=validate_bundle,admit=admit,execute=execute,description='NPU finite inference and validated RAM return',watchdog=watchdog_marker((args.bundle/'image.elf').read_bytes()))
 return 0 if result['functional_success'] or result.get('checkpoint') in ('ENTRY_MEMORY_READY','PROVIDERS_CONSTRUCTED') or result.get('startup_bootstrap') else 2

def main(argv=None):
 p=argparse.ArgumentParser(description=__doc__);sub=p.add_subparsers(dest='operation',required=True)
 q=sub.add_parser('session')
 for n in ('image','boot','output'):q.add_argument('--'+n,type=Path,required=True)
 for n in ('manifest','admission'):q.add_argument('--'+n,type=Path)
 q=sub.add_parser('prepare')
 for n in ('image','session','output','clang','lld','objcopy'):q.add_argument('--'+n,type=Path,required=True)
 for n in ('manifest','admission'):q.add_argument('--'+n,type=Path)
 q=sub.add_parser('validate');q.add_argument('--bundle',type=Path,required=True)
 q=sub.add_parser('run')
 for n in ('bundle','fel','evidence'):q.add_argument('--'+n,type=Path,required=True)
 for n in ('ddr-evidence','platform-survey'):q.add_argument('--'+n,type=Path)
 q.add_argument('--warm-return',type=Path,action='append',default=[]);q.add_argument('--root-owned-claim')
 a=p.parse_args(argv)
 if a.operation=='session':print(construct_session(a.image,a.manifest,a.boot,a.admission,a.output));return 0
 if a.operation=='prepare':print(prepare(a.image,a.manifest,a.session,a.admission,a.output,a.clang,a.lld,a.objcopy)['output']);return 0
 if a.operation=='validate':validate_bundle(a.bundle);print('Validated exact NPU returning bundle; no hardware access');return 0
 return run(a)
if __name__=='__main__':
 try:sys.exit(main())
 except (OSError,ValueError,RuntimeError,KeyError,struct.error) as error:sys.exit('Stopped: '+str(error))
