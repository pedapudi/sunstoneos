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
ACTIVE_CANCEL='npu-active-cancel-return-v1'
# The NPU runtime client (build.py --runtime): one job of each linked network through npu_job_run.
RUNTIME='npu-runtime-return-v1'
SWITCH='npu-model-switch-return-v1'
SDK='npu-sdk-return-v1'
POWER='npu-power-cycle-return-v1'
LATENCY='npu-latency-return-v1'
# The startup client's receipt magic, monitor detail, inference jobs and interrupt completions.
STARTUP_CLIENT={STARTUP:(0x4e50555354415231,0x4e505553,0,1),INFERENCE:(0x4e5055494e463031,0x4e505549,1,2),
 ACTIVE_CANCEL:(0x4e505543414e3031,0x4e505543,1,3),
 RUNTIME:(0x4e50555254523031,0x4e505552,2,3),
 SWITCH:(0x4e50555357433031,0x4e505557,3,4),
 SDK:(0x4e505553444b3031,0x4e505544,2,3),
 LATENCY:(0x4e50554c41543031,0x4e50554c,1001,1002)}
MBUS_CHECKPOINT='npu-entry-mbus-checkpoint-v1'
IRQ_CHECKPOINT='npu-entry-irq-checkpoint-v1'
PROVIDER_CHECKPOINT='npu-entry-provider-checkpoint-v1'
PROFILE='npu-finite-return-v1';CHECKPOINT='npu-entry-memory-checkpoint-v1';THERMAL='npu-finite-thermal-return-v1';PROFILES=(PROFILE,CHECKPOINT,THERMAL,MBUS_CHECKPOINT,IRQ_CHECKPOINT,PROVIDER_CHECKPOINT,RESTART,STARTUP,INFERENCE,ACTIVE_CANCEL,RUNTIME,SWITCH,SDK,POWER,LATENCY);SCHEMA='a733-npu-finite-entry-v1';DRAM=(0x41000000,0x1200000)
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
 if prepared.get('execution_profile')==LATENCY and prepared.get('geometry',{}).get('latency',{}).get('idle'):
  return (STARTUP_CLIENT[LATENCY][0],STARTUP_CLIENT[LATENCY][1],IDLE_JOBS,IDLE_JOBS+1)
 if prepared.get('execution_profile')==LATENCY and prepared.get('geometry',{}).get('latency',{}).get('dependent_chain'):
  count=prepared['geometry']['latency'].get('chain_repeats',1)
  require(type(count) is int and 1<=count<=1000,'Dependent chain repetition geometry differs.')
  return (STARTUP_CLIENT[LATENCY][0],STARTUP_CLIENT[LATENCY][1],1001+3*count,1002+3*count)
 if prepared.get('execution_profile')==LATENCY and prepared.get('geometry',{}).get('latency',{}).get('sequence_repeats'):
  return (STARTUP_CLIENT[LATENCY][0],STARTUP_CLIENT[LATENCY][1],1002,1003)
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

IDLE_PROFILE=(0x4e505549444c3033,3);IDLE_GAPS=(9000,11000,100000);IDLE_CYCLES=(100,100,30)
IDLE_THRESHOLD_US=10000;IDLE_RESUME_US=100;IDLE_WINDOWS=5;IDLE_WINDOW_JOBS=12000
IDLE_JOBS=1+1000+IDLE_WINDOWS*IDLE_WINDOW_JOBS+sum(IDLE_CYCLES)
IDLE_POLICIES=('control','gate','baseline')
IDLE_HISTOGRAM=4096

def idle_extension(startup_marker,names,read):
 """The immutable idle benchmark marker and the symbols its policy needs."""
 if 'npu_idle_return_profile' not in names:return None
 words=struct.unpack('<13Q',read('npu_idle_return_profile'))
 owner=('npu_runtime_queue_power_start','npu_idle_gate','npu_idle_resume')
 require(startup_marker[2:]==(IDLE_JOBS,IDLE_JOBS+1) and words[:2]==IDLE_PROFILE and words[2] in (0,1,2) and
  words[3:]==(*IDLE_CYCLES,*IDLE_GAPS,IDLE_THRESHOLD_US,IDLE_RESUME_US,IDLE_WINDOWS,IDLE_WINDOW_JOBS) and
  'npu_dependent_return_profile' not in names and
  all(n in names for n in ('sunstone_deadline_wait_until','sunstone_deadline_irq','npu_finite_deadline_setup')) and
  (words[2]==2 or all(n in names for n in owner)),
  'Idle benchmark marker, power owner or shared timer composition differs.')
 return {'policy':IDLE_POLICIES[words[2]],'cycles':list(IDLE_CYCLES),'gaps_us':list(IDLE_GAPS),'threshold_us':IDLE_THRESHOLD_US,
  'resume_budget_us':IDLE_RESUME_US,'windows':IDLE_WINDOWS,'window_jobs':IDLE_WINDOW_JOBS}

def latency_extension(startup_marker,names,read):
 idle=idle_extension(startup_marker,names,read)
 latency=(startup_marker[:2]==(STARTUP_CLIENT[LATENCY][0],1) and startup_marker[3]==startup_marker[2]+1 and
  (startup_marker[2] in (1001,1002) or idle is not None and startup_marker[2]==IDLE_JOBS or
   idle is None and 1004<=startup_marker[2]<=4001 and (startup_marker[2]-1001)%3==0))
 require(('npu_latency_return_profile' in names)==latency,'Latency extension/startup marker differs.')
 if not latency:return None
 raw=read('npu_latency_return_profile')
 require(len(raw)==64,'Latency immutable extension length differs.')
 words=struct.unpack('<8Q',raw)
 repeats=words[5]
 dependent=startup_marker[2]>=1004 and idle is None
 chain_repeats=(startup_marker[2]-1001)//3 if dependent else 1
 require(('npu_dependent_return_profile' in names)==dependent,'Dependent chain extension/startup marker differs.')
 if dependent:require(read('npu_dependent_return_profile')==struct.pack('<8Q',0x4e50554445503031,1 if chain_repeats==1 else 2,0,0,1,0,2,chain_repeats),'Dependent chain immutable extension differs.')
 require(words[0]==0x4e50554c41545031 and words[1] in (1,2) and words[2] in (0,1) and words[3:5]==(1000,1) and
  words[6:]==(4,24000000) and (repeats==0 or words[2]==1 and 2<=repeats<=64) and
  (startup_marker[2]==1002)==bool(repeats) and (not dependent or words[1]==2 and words[2]==1 and not repeats) and
  (idle is None or words[1:3]==(2,1) and not repeats),
  'Latency immutable extension differs.')
 return {'mode':'queue' if words[2] else 'legacy','measured_jobs':1000,'warmup_jobs':1,'model':0,'sequence_repeats':repeats,**({'doorbell_timing':True} if words[1]==2 else {}),**({'idle':idle} if idle else {}),**({'dependent_chain':True,**({'chain_repeats':chain_repeats} if chain_repeats!=1 else {})} if dependent else {})}


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
  if value[:2]==(STARTUP_CLIENT[LATENCY][0],1) and value[3]==value[2]+1 and (value[2] in (1002,IDLE_JOBS) or 1004<=value[2]<=4001 and (value[2]-1001)%3==0):execution=LATENCY
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
  if latency:startup=startup_client({'execution_profile':execution,'geometry':{'latency':latency}})
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
  require(read('npu_latency_task_stats_profile')==struct.pack('<4Q',0x4e50554350553031,1,64,24000000) and
   'ulTaskGetRunTimeCounter' in symbols,'Missing native latency task runtime counters.')
  require('npu_finite_reference_2' not in names,'Latency plan requires exactly two linked networks.')
  if latency.get('dependent_chain'):
   require(all(n in names for n in ('a733_npu_runtime_resident_slots','a733_npu_runtime_resident_prepare','npu_runtime_link_tensor','npu_runtime_preload','npu_runtime_bind_resident','run_dependent')),'Missing native dependent tensor loader or client.')
  if latency['mode']=='queue':
   require(all(name in symbols for name in ('npu_runtime_queue_claim','npu_runtime_queue_wait','npu_runtime_queue_return','npu_queue_submit','npu_queue_release')),
    'Missing native resident queue implementation.')
 if 'npu_failure_return_profile' in names:
  require(runtime and startup and read('npu_failure_return_profile')==FAILURE_MARKER,'Failure return marker or profile differs.')
  geometry['failure_return']=True
 if manifest['execution_profile']==SWITCH:geometry['model_switch']=True
 if manifest['execution_profile']==SDK:geometry['sdk']=True
 if manifest['execution_profile']==INFERENCE:geometry['single_inference']=True
 if manifest['execution_profile']==ACTIVE_CANCEL:
  require(read('npu_active_cancel_profile')==struct.pack('<6Q',0x4e505543414e3031,32768,262160,524288,0x189000,0xe010a000) and
   all(n in symbols for n in ('npu_service_cancel_diagnostic_enable','npu_convolution_diagnostic_send','cancel_checkpoint','npu_application_irq_restart')) and
   all(n not in symbols for n in ('a733_npu_startup_power_off','a733_npu_startup_power_restart','npu_convolution_restart_poll')),
   'Active cancellation implementation or fixed command layout differs.')
  geometry['active_cancel']=True
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

def geometry_profile(g):return LATENCY if g.get('latency') else POWER if g.get('power_cycle') else SDK if g.get('sdk') else SWITCH if g.get('model_switch') else RUNTIME if g.get('runtime') else ACTIVE_CANCEL if g.get('active_cancel') else INFERENCE if g.get('single_inference') else STARTUP if g.get('startup_only') else PROVIDER_CHECKPOINT if g.get('checkpoint_provider') else IRQ_CHECKPOINT if g.get('checkpoint_irq') else MBUS_CHECKPOINT if g.get('checkpoint_mbus') else RESTART if g.get('restart') else CHECKPOINT if g.get('checkpoint') else THERMAL if g.get('thermal') else PROFILE

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


FAILURE_REASONS={1:'status above the allowed return status',2:'return state not ready',3:'status above one',4:'NPU not quiescent',
 5:'receipt completion word differs',6:'past the diagnostic deadline',7:'cache stride differs',9:'the client requested a failure return'}
FAILURE_MARKER=struct.pack('<3Q',0x4e5055464c523031,1,2)

def failure_report(text,deadline):
 """A failure return of a quiesced NPU: status 2, the receipt matched, and the platform log ends with the reason."""
 match=re.fullmatch(r'(?s)(.*\n)?NPU_FAILURE=0x([0-9a-f]{16})\n',text)
 require(match is not None and int(match[2],16) in FAILURE_REASONS,'Missing or unknown failure return record.')
 reason=int(match[2],16)
 return {'normal_return':True,'failure_return':True,'functional_success':False,'controlled_negative':False,'status':2,
  'diagnostic_deadline':deadline,'failure':{'reason':reason,'predicate':FAILURE_REASONS[reason]},'log':text}

def validate_reports(monitor,platform,receipt,p):
 thermal=p.get('execution_profile')==THERMAL;startup=startup_client(p)
 require(len(monitor)==128 and len(platform)==16384 and len(receipt)==(112 if thermal else 88),'Incomplete returned reports.')
 m=struct.unpack('<16Q',monitor);h=struct.unpack_from('<16Q',platform);r=struct.unpack('<14Q' if thermal else '<11Q',receipt);g=p['geometry']
 magic=0x415337334e505231
 require(m[0]==magic and (m[1],m[2])==(p['epoch'],p['generation']) and m[3]==5 and m[4] in ((0,1,2) if thermal or g.get('failure_return') else (0,1)) and m[5]==(0x4e50554e if p.get('execution_profile') in (CHECKPOINT,MBUS_CHECKPOINT,IRQ_CHECKPOINT,PROVIDER_CHECKPOINT) else startup[1] if startup else 0) and m[7]==1,'No matched normal monitor completion.')
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
 if status==2 and g.get('failure_return'):
  return failure_report(text,deadline)
 if g.get('latency'):
  result=latency_report(text,status,deadline,g)
  require(m[6]<=result['timing']['start_ticks']<result['timing']['end_ticks']<=h[9],
   'Latency timing escapes native report.')
  if result.get('repeated_sequence'):
   require(result['timing']['end_ticks']<=result['repeated_sequence']['start_ticks']<result['repeated_sequence']['end_ticks']<=h[9],
    'Repeated sequence timing escapes native report.')
  if result.get('dependent_chain'):require(result['dependent_chain'][-1]['end_ticks']<=h[9],'Dependent chain timing escapes native report.')
  return result
 if g.get('sustained'):
  result=sustained_report(text,status,deadline,g)
  ticks=result['timing']
  require(m[6]<=ticks['start_ticks']<ticks['end_ticks']<=h[9],'Sustained timing escapes native report.')
  return result
 if g.get('runtime'):return runtime_report(text,status,deadline,g)
 if g.get('active_cancel'):return active_cancel_report(text,status,deadline,g)
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

def active_cancel_report(text,status,deadline,g):
 """Validate the active observations, pulse, owned recovery and exact output."""
 head=('NPU_STAGE=WAIT_STARTUP\nNPU_STAGE=OPEN\nNPU_STAGE=BOOTSTRAP_READY\nNPU_STAGE=CANCELED_RECOVERED\n'
  'NPU_STAGE=SUBMIT job=0x0000000000000001\nNPU_STAGE=REAPED\nNPU_STAGE=CLOSE\nNPU_STAGE=WAIT_LOCAL_STOP\n'
  'NPU_IRQ_COMPLETIONS=0x0000000000000003\nNPU_IRQ_STOPPED=0x0000000000000001\nNPU_CANCEL_RECEIPT=\n')
 match=re.fullmatch(re.escape(head)+r'((?:0x[0-9a-f]{16}\n){51})NPU_STAGE=QUIESCENT\nNPU_OUTPUT=\n'
  r'((?:0x[0-9a-f]{16}\n)+)NPU_INFERENCE=MATCH; inference_jobs=1\n',text)
 require(status==0 and match is not None,'Active cancellation terminal stages or receipt differ.')
 words=[int(v,16) for v in match[1].splitlines()]
 ticket,trigger,asserted,released,done,recovery_ticket,receipts,recovery_receipts,cancel_status,idle=words[:10]
 require(0<ticket<recovery_ticket and trigger<=asserted<=trigger+5000 and released-asserted>100 and
  released<=done<asserted+20000 and receipts==3 and recovery_receipts==2 and
  cancel_status&24==16 and idle==0x7fffffff,'Active cancellation pulse, idle or IRQ receipts differ.')
 samples=[words[10+8*i:18+8*i] for i in range(3)]
 for index,sample in enumerate(samples):
  time,job,idle,frontend0,frontend1,address0,address1,state=sample
  require(trigger<=time<=trigger+2000 and job==ticket and not idle&1 and state==3 and
   all(0xe010a000<=address<0xe018a000 and not address&7 for address in (address0,address1)),
   'Active cancellation sample does not prove the matching command was active.')
  if index:require(time-samples[index-1][0]>=100,'Active cancellation samples are too close.')
 require(samples[-1][0]<=asserted,'Active observations follow cancellation assertion.')
 platform=[words[34+8*i:42+8*i] for i in range(2)]
 for sample in platform:
  policy,status,pll,module,bus,gate,authority,ticks=sample
  require(policy&15==8 and status&15==8 and pll&~(1<<24)==0xf8104600 and
   module&0x8700001f==0x80000000 and bus&0xf0001==0xf0001 and authority&4 and
   not authority&0x1700 and ticks>0,'Active cancellation power or clock state differs.')
 require(platform[0][:6]==platform[1][:6] and platform[0][7]<platform[1][7] and words[50]==2,
  'Active cancellation recovery changed a power or clock checkpoint.')
 output=[int(v,16) for v in match[2].splitlines()]
 require(all(v<256 for v in output) and bytes(output)==bytes.fromhex(g['reference']),
  'Active cancellation follow-up output differs from the complete reference.')
 return dict(normal_return=True,active_cancel=True,functional_success=True,controlled_negative=False,jobs=1,status=0,
  diagnostic_deadline=deadline,output=bytes(output).hex(),samples=samples,platform=platform,
  cancel=dict(ticket=ticket,trigger_us=trigger,asserted_us=asserted,released_us=released,done_us=done,
   recovery_ticket=recovery_ticket,status=cancel_status,idle=words[9],IRQ_receipts=receipts,recovery_IRQ_receipts=recovery_receipts),log=text)

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

def _idle_pattern():
 words=lambda name,n:r'(?P<'+name+r'>(?:0x[0-9a-f]{16}\n){'+str(n)+r'})'
 text=r'NPU_IDLE=\n'+words('idle_totals',17)
 for window in range(IDLE_WINDOWS):
  text+=(r'NPU_IDLE_WINDOW=\n'+words(f'idle_window_{window}',20)+
   r'NPU_IDLE_WINDOW_OUTPUT_SHA256=\n(?P<idle_window_%d_output>[0-9a-f]{64})\n'%window)
 for group in range(len(IDLE_GAPS)):
  text+=(r'NPU_IDLE_GROUP=\n'+words(f'idle_{group}',32)+
   r'NPU_IDLE_OUTPUT_SHA256=\n(?P<idle_%d_output>[0-9a-f]{64})\n'%group)
 return text
IDLE_PATTERN=_idle_pattern()

def idle_report(match,g,frequency,warm_end,deadline,idle):
 """Decode and cross-check the repeated windows and the cold-job intervals.

 Each window runs 12000 back-to-back jobs and reports exact quantiles from a
 one-tick histogram. Each group runs 100 cold jobs after an idle gap. A gap
 below the ten millisecond threshold must never gate. Only the gating policy
 may write registers, and its resumes must stay within the budget. The
 baseline creates no power owner, so every owner counter is zero.
 """
 policy=idle['policy'];enabled=policy=='gate';owned=policy!='baseline'
 totals=[int(v,16) for v in match['idle_totals'].split()]
 (code,warm_owner_ticks,warm_owner_elapsed,warm_wakes,gates,resumes,writes,gated,resume_max,over_budget,
  owner_elapsed,owner_wakes,irq_ticks,quiet_retries,margin_windows,margin_after_windows,margin_end)=totals
 require(0<margin_end<=margin_after_windows<=margin_windows<=deadline and code==IDLE_POLICIES.index(policy) and warm_wakes<=owner_wakes and over_budget==0 and
  (owned or not any((warm_owner_ticks,warm_owner_elapsed,warm_wakes,owner_elapsed,owner_wakes,quiet_retries))),
  'Idle policy or warm owner counters differ.')
 reference=bytes.fromhex(g['references'][0]);digest=hashlib.sha256(reference).digest()
 tolerance=frequency//10000
 windows=[];previous=warm_end;matched=True;sentinel=(1<<64)-1
 for index in range(IDLE_WINDOWS):
  (number,jobs,checked,bad,start,end,total,p50,p99,maximum,overflow,client,worker,owner,idle_task,irq,wakes,
   first,offset,value)=(int(v,16) for v in match[f'idle_window_{index}'].split())
  length=end-start
  require(number==index and jobs==checked==IDLE_WINDOW_JOBS and bad<=jobs and previous<=start<end<deadline and
   jobs<=total<=length and 0<p50<=p99<=maximum<=frequency and overflow<=jobs and
   (overflow==0)==(maximum<IDLE_HISTOGRAM) and
   (p99==IDLE_HISTOGRAM)==(jobs-overflow<(jobs*99+99)//100) and (p50==IDLE_HISTOGRAM)==(jobs-overflow<(jobs*50+99)//100) and
   client+worker+owner+idle_task<=length+tolerance and irq<=length and wakes<=length//(IDLE_THRESHOLD_US*frequency//1000000)+2 and
   (owned or not (owner or wakes)),'Idle window identity, order or timing differs.')
  ok=match[f'idle_window_{index}_output']==hashlib.sha256(digest*jobs).hexdigest()
  require(ok==(bad==0),'Idle window output status disagrees with its digest.')
  if bad:require(first<jobs and offset<len(reference) and value<256 and value!=reference[offset],'Idle window first mismatch detail differs.')
  else:require((first,offset,value)==(sentinel,)*3,'Unexpected idle window mismatch detail.')
  matched=matched and ok;previous=end
  windows.append({'index':index,'jobs':jobs,'start_ticks':start,'end_ticks':end,'duration_ticks':length,
   'jobs_per_second':jobs*frequency/length,'latency_ticks':{'p50':p50,'p99':p99,'max':maximum},
   'latency_us':{k:v*1000000/frequency for k,v in (('p50',p50),('p99',p99),('max',maximum))},
   'histogram_overflow_jobs':overflow,'job_duration_sum_ticks':total,
   'task_runtime_ticks':{'client':client,'worker':worker,'owner':owner,'idle_task':idle_task},
   'charged_cpu_ticks':client+worker+owner,'non_idle_ticks':length-idle_task,'irq_driver_elapsed_ticks':irq,
   'owner_wakes':wakes,'functional_success':ok,'output_digest':match[f'idle_window_{index}_output'],
   'first_mismatch':None if not bad else {'job':first,'offset':offset,'byte':value}})
 groups=[];sums=dict.fromkeys(('gates','resumes','writes','gated','owner_elapsed'),0)
 budget=IDLE_RESUME_US*frequency//1000000;threshold=IDLE_THRESHOLD_US*frequency//1000000
 for index,gap in enumerate(IDLE_GAPS):
  (got_gap,cycles,checked,bad,start,end,gate_count,resume_count,write_count,gated_ticks,requests,driver_max,owner_ticks,
   group_owner_elapsed,client_ticks,worker_ticks,idle_task,group_irq,idle_min,idle_max,idle_sum,first,offset,value,
   pending,cold50,cold99,cold_max,cold_sum,adm50,adm99,adm_max)=(int(v,16) for v in match[f'idle_{index}'].split())
  length=end-start
  require(got_gap==gap and cycles==checked==IDLE_CYCLES[index] and bad<=cycles and previous<=start<end<deadline,
   'Idle group identity or order differs.')
  require(0<cold50<=cold99<=cold_max<=frequency and cold_max<=cold_sum<=cycles*cold_max and
   adm50<=adm99<=adm_max<=65535 and gap*frequency//1000000<=idle_min<=idle_max and idle_min*cycles<=idle_sum<=idle_max*cycles and
   idle_sum<=length and client_ticks+worker_ticks+owner_ticks+idle_task<=length+tolerance and group_irq<=length and
   group_owner_elapsed<=length,'Idle gap, duration or task counters differ.')
  # A gap below the threshold that reached it (a late timer wake) is a flawed measurement, reported as invalid.
  gap_valid=gap*frequency//1000000>=threshold or idle_max<threshold
  if enabled and gap>IDLE_THRESHOLD_US:
   require(gate_count==resume_count==requests==pending==cycles and write_count==6*cycles and
    0<gated_ticks<=idle_sum-cycles*threshold and 0<driver_max<=IDLE_RESUME_US and adm_max<=IDLE_RESUME_US,
    'Idle gating or resume bound differs.')
  elif enabled and not gap_valid:
   # Some cycles idled past the delay: each of them gated, none of the others did.
   require(gate_count==resume_count==requests==pending<=cycles and write_count==6*gate_count and
    gated_ticks<=idle_sum and driver_max<=IDLE_RESUME_US and adm_max<=IDLE_RESUME_US,'Idle gating or resume bound differs.')
  else:
   require((gate_count,resume_count,write_count,gated_ticks,requests,driver_max,pending)==(0,)*7 and
    (owned or not (owner_ticks or group_owner_elapsed)),'Idle interval gated or wrote registers outside the enabled policy.')
  ok=match[f'idle_{index}_output']==hashlib.sha256(digest*cycles).hexdigest()
  require(ok==(bad==0),'Idle output status disagrees with its digest.')
  if bad:require(first<cycles and offset<len(reference) and value<256 and value!=reference[offset],'Idle first mismatch detail differs.')
  else:require((first,offset,value)==(sentinel,)*3,'Unexpected idle mismatch detail.')
  for name,v in (('gates',gate_count),('resumes',resume_count),('writes',write_count),('gated',gated_ticks),('owner_elapsed',group_owner_elapsed)):sums[name]+=v
  matched=matched and ok;previous=end
  groups.append({'gap_us':gap,'cycles':cycles,'gap_valid':gap_valid,'gates':gate_count,'resumes':resume_count,'register_writes':write_count,
   'gated_ticks':gated_ticks,'resume_requests':requests,'pending_admissions':pending,
   'idle_ticks':{'min':idle_min,'max':idle_max,'sum':idle_sum},'driver_resume_us_max':driver_max,
   'admission_us':{'p50':adm50,'p99':adm99,'max':adm_max},
   'cold_job_ticks':{'p50':cold50,'p99':cold99,'max':cold_max,'sum':cold_sum},
   'cold_job_us':{k:v*1000000/frequency for k,v in (('p50',cold50),('p99',cold99),('max',cold_max))},
   'task_runtime_ticks':{'client':client_ticks,'worker':worker_ticks,'owner':owner_ticks,'idle_task':idle_task},
   'owner_elapsed_ticks':group_owner_elapsed,'irq_driver_elapsed_ticks':group_irq,'start_ticks':start,'end_ticks':end,
   'functional_success':ok,'output_digest':match[f'idle_{index}_output'],
   'first_mismatch':None if not bad else {'cycle':first,'offset':offset,'byte':value}})
 require((gates,resumes,writes,gated)==(sums['gates'],sums['resumes'],sums['writes'],sums['gated']) and
  owner_elapsed>=warm_owner_elapsed+sums['owner_elapsed'] and resume_max<=budget and
  (resume_max>0)==(sums['resumes']>0),'Idle totals disagree with the groups.')
 return {'policy':policy,'threshold_us':IDLE_THRESHOLD_US,'resume_budget_us':IDLE_RESUME_US,
  'warm':{'owner_runtime_ticks':warm_owner_ticks,'owner_elapsed_ticks':warm_owner_elapsed,'owner_wakes':warm_wakes},
  'totals':{'gates':gates,'resumes':resumes,'register_writes':writes,'gated_ticks':gated,'resume_max_ticks':resume_max,
   'resume_over_budget':over_budget,'owner_elapsed_ticks':owner_elapsed,'owner_wakes':owner_wakes,
   'irq_driver_elapsed_ticks':irq_ticks,'quiet_retries':quiet_retries},
  'deadline_margin_ticks':{'before_windows':margin_windows,'after_windows':margin_after_windows,'end':margin_end},
  'windows':windows,'groups':groups,'functional_success':matched,
  'scope':'Each window is 12000 back-to-back jobs with exact one-tick quantiles. Charged CPU time is client, runtime worker and power-owner scheduler runtime; non-idle time is the window length minus the idle task runtime. Cold jobs follow 9, 11 and 100 ms gaps and cover input preparation through record release, including power admission and resume.'}

def latency_report(text,status,deadline,g):
 """Check every measured latency and the digest of every checked output.

 The legacy interval covers npu_job_run. The queue interval covers input copy,
 output poison, tensor cache work, submit, interrupt wait, acquire and release.
 Output comparison and hashing follow each interval and enter total time.
 """
 geometry=g.get('latency')
 repeats=geometry.get('sequence_repeats',0) if geometry else 0
 doorbell_timing=geometry.get('doorbell_timing',False) if geometry else False
 dependent=geometry.get('dependent_chain',False) if geometry else False
 chain_repeats=geometry.get('chain_repeats',1) if geometry else 1
 require(type(chain_repeats) is int and 1<=chain_repeats<=1000 and (dependent or chain_repeats==1),'Dependent chain repetition geometry differs.')
 idle=geometry.get('idle') if geometry else None
 require(idle is None or idle in ({'policy':policy,'cycles':list(IDLE_CYCLES),'gaps_us':list(IDLE_GAPS),'threshold_us':IDLE_THRESHOLD_US,'resume_budget_us':IDLE_RESUME_US,'windows':IDLE_WINDOWS,'window_jobs':IDLE_WINDOW_JOBS} for policy in IDLE_POLICIES),
  'Idle benchmark geometry differs.')
 extra_jobs=3*dependent*chain_repeats+bool(repeats)+(IDLE_JOBS-1001)*bool(idle)
 require(g.get('runtime') and len(g['references'])==2 and geometry and
  geometry=={'mode':geometry['mode'],'measured_jobs':1000,'warmup_jobs':1,'model':0,'sequence_repeats':repeats,**({'doorbell_timing':True} if doorbell_timing else {}),**({'idle':idle} if idle else {}),**({'dependent_chain':True,**({'chain_repeats':chain_repeats} if chain_repeats!=1 else {})} if dependent else {})} and
  geometry['mode'] in ('legacy','queue') and (not dependent or doorbell_timing and geometry['mode']=='queue' and not repeats) and (repeats==0 or geometry['mode']=='queue' and 2<=repeats<=64),'Latency network geometry differs.')
 head=('NPU_STAGE=WAIT_STARTUP\nNPU_STAGE=WARMUP\nNPU_STAGE=LATENCY\nNPU_STAGE=CLOSE\n'
  f'NPU_STAGE=WAIT_LOCAL_STOP\nNPU_IRQ_COMPLETIONS=0x{1002+extra_jobs:016x}\n'
  'NPU_IRQ_STOPPED=0x0000000000000001\nNPU_STAGE=QUIESCENT\nNPU_LATENCY=\n')
 pattern=(re.escape(head)+r'((?:0x[0-9a-f]{16}\n){23})(?:NPU_SUBMIT_TO_DOORBELL=\n(?P<doorbell_values>(?:0x[0-9a-f]{16}\n){5}))?NPU_DURATION_LE32=\n'
  r'(?P<duration_raw>(?:[0-9a-f]{128}\n){62}[0-9a-f]{64}\n)NPU_DURATION_SHA256=\n(?P<duration_digest>[0-9a-f]{64})\n'
  r'NPU_OUTPUT_SHA256=\n(?P<output_digest>[0-9a-f]{64})\n'
  +(IDLE_PATTERN if idle else '')
  +(r'(?:NPU_DEPENDENT_BOUNDARY=\n(?P<boundary>(?:0x[0-9a-f]{16}\n){6})'+
   ('NPU_DEPENDENT_CHAIN=\n' if chain_repeats==1 else 'NPU_DEPENDENT_REPEATED_CHAIN=\n')+
   r'(?P<dependent_0>(?:0x[0-9a-f]{16}\n){'+str(12 if chain_repeats==1 else 21)+r'})(?P<dependent_digest_0>[0-9a-f]{64})\n'+
   r'(?P<dependent_1>(?:0x[0-9a-f]{16}\n){'+str(12 if chain_repeats==1 else 21)+r'})(?P<dependent_digest_1>[0-9a-f]{64})\n)?')
  +r'(?:NPU_REPEATED_SEQUENCE=\n(?P<sequence_values>(?:0x[0-9a-f]{16}\n){11})NPU_SEQUENCE_OUTPUT_SHA256=\n(?P<sequence_digest>[0-9a-f]{64})\n)?'
  r'NPU_INFERENCE=(?P<outcome>MATCH|MISMATCH); inference_jobs='+str(1001+extra_jobs)+r'\n')
 match=re.fullmatch(pattern,text)
 require(match is not None,'Latency stage, sample, IRQ or output log differs.')
 (frequency,mode,jobs,warmups,total,checked,bad,start,end,p50,p99,maximum,total_ticks,
  submit_ticks,acquire_ticks,first_bad,offset,value,submitted,finished,irq_ticks,client_cpu,worker_cpu)=(int(v,16) for v in match[1].split())
 require(frequency==24000000 and mode==int(geometry['mode']=='queue') and
  jobs==checked==1000 and warmups==1 and total==1001+extra_jobs and bad<=jobs,
  'Latency mode, warmup or job/check counters differ.')
 require(submitted==finished==(1000 if mode else 0),'Latency direct fence counters differ.')
 raw=bytes.fromhex(match['duration_raw']);durations=list(struct.unpack('<1000I',raw));ordered=sorted(durations)
 require(hashlib.sha256(raw).hexdigest()==match['duration_digest'],'Latency sample checksum differs.')
 require(all(0<t<=frequency for t in durations) and total_ticks==sum(durations) and
  (p50,p99,maximum)==(ordered[499],ordered[989],ordered[999]),'Latency samples or quantiles differ.')
 require(deadline-480000000<=start<end<deadline and total_ticks<=end-start and
  all(t<=total_ticks for t in (submit_ticks,acquire_ticks,irq_ticks)) and (mode or submit_ticks==acquire_ticks==irq_ticks==0),
  'Latency timing intervals or caller phases differ.')
 require(client_cpu+worker_cpu<=end-start,'Latency scheduled task counters exceed the measured window.')
 doorbell=None
 require(bool(match['doorbell_values'])==doorbell_timing,'Submit-to-doorbell report/marker differs.')
 if doorbell_timing:
  count,db50,db99,dbmax,dbsum=(int(v,16) for v in match['doorbell_values'].split())
  require(count==jobs and 0<db50<=db99<=dbmax<maximum and
   500*db50+490*db99+10*dbmax>=dbsum and
   499+490*db50+10*db99+dbmax<=dbsum<=total_ticks,
   'Submit-to-doorbell counts, quantiles or sum differ.')
  doorbell={'samples':count,'latency_ticks':{'p50':db50,'p99':db99,'max':dbmax},
   'mean_ticks':dbsum/count,'sum_ticks':dbsum,
   'latency_us':{k:v*1000000/frequency for k,v in {'p50':db50,'p99':db99,'max':dbmax}.items()},
   'mean_us':dbsum*1000000/frequency/count,
   'scope':'API entry through completed doorbell write and device barrier; includes local lock and queue admission' if mode else
    'helper API entry through completed doorbell write; includes tensor copies, validation, mailbox and worker scheduling'}
 reference=bytes.fromhex(g['references'][0]);digest=match['output_digest']
 expected=hashlib.sha256(hashlib.sha256(reference).digest()*1000).hexdigest()
 matched=digest==expected
 require(matched==(bad==0),'Latency status disagrees with output checks/hash.')
 sequence=None
 require(bool(match['sequence_values'])==bool(repeats),'Repeated sequence report/marker differs.')
 if repeats:
  count,seq_start,seq_end,stream,seq_checked,seq_bad,seq_offset,seq_byte,seq_submitted,seq_finished,seq_irq=(int(v,16) for v in match['sequence_values'].split())
  seq_match=match['sequence_digest']==hashlib.sha256(reference).hexdigest()
  require(count==repeats and seq_checked==seq_submitted==seq_finished==1 and seq_bad in (0,1) and
   seq_match==(seq_bad==0) and end<=seq_start<seq_end<deadline and seq_end-seq_start<=frequency and
   0<stream<=524280 and not stream&7 and seq_irq<=seq_end-seq_start,'Repeated sequence counters, output or timing differ.')
  sentinel=(1<<64)-1
  if not seq_bad:require((seq_offset,seq_byte)==(sentinel,sentinel),'Unexpected repeated sequence mismatch detail.')
  else:require(seq_offset<len(reference) and seq_byte<256 and seq_byte!=reference[seq_offset],'Repeated sequence mismatch detail differs.')
  sequence={'semantics':'repeated same command with shared input and output; final output checked; no tensor dependency claim',
   'repetitions':count,'start_ticks':seq_start,'end_ticks':seq_end,'duration_ticks':seq_end-seq_start,
   'effective_commands_per_second':count*frequency/(seq_end-seq_start),'stream_bytes':stream,
   'final_output_checked':True,'functional_success':seq_match,'output_digest':match['sequence_digest'],
   'first_mismatch':None if not seq_bad else {'offset':seq_offset,'byte':seq_byte},'irq_driver_elapsed_ticks':seq_irq,
   'window':'one input preparation, submit, interrupt wait, output acquisition and release; stream construction and output checking excluded'}
  matched=matched and seq_match
 idle_result=None
 require(bool(match.groupdict().get('idle_totals'))==bool(idle),'Idle report/marker differs.')
 if idle:
  idle_result=idle_report(match,g,frequency,end,deadline,idle)
  matched=matched and idle_result['functional_success']
 chain=[]
 require(bool(match['dependent_0'])==dependent,'Dependent chain report/marker differs.')
 if dependent:
  src_device,dst_device,src_cpu,dst_cpu,logical,allocation=(int(v,16) for v in match['boundary'].split())
  require(src_device==dst_device and 0<src_device<=0xffffffff and not src_device&4095 and
   src_cpu==dst_cpu and src_cpu>0 and not src_cpu&63 and logical==len(bytes.fromhex(g['references'][0])) and
   allocation>=logical and not allocation&63,'Dependent chain DMA boundary differs.')
  boundary={'producer_device':src_device,'consumer_device':dst_device,'producer_cpu':src_cpu,
   'consumer_cpu':dst_cpu,'logical_bytes':logical,'allocation_bytes':allocation}
  combined=hashlib.sha256(b''.join(bytes.fromhex(r) for r in g['references'])*chain_repeats).hexdigest()
  previous=end
  for index in range(2):
   values=[int(v,16) for v in match[f'dependent_{index}'].split()]
   dbegin,dend,before_wait,issued,done,checks,fault,stage,at,byte,stream,irq=values[:12]
   count=1 if index else 2
   ok=match[f'dependent_digest_{index}']==combined
   require(previous<=dbegin<dend<deadline and (chain_repeats>1 or dend-dbegin<=frequency) and
    before_wait==count and issued==done==count*chain_repeats and checks==2*chain_repeats and fault in (0,1) and ok==(fault==0) and
    irq<=dend-dbegin and (0<stream<=524280 and not stream&7 if index else stream==0),
    'Dependent chain counters, output or timing differ.')
   sentinel=(1<<64)-1
   if fault:
    require(stage<2 and at<len(bytes.fromhex(g['references'][stage])) and byte<256 and byte!=bytes.fromhex(g['references'][stage])[at],'Dependent chain mismatch detail differs.')
   else:require((stage,at,byte)==(sentinel,sentinel,sentinel),'Unexpected dependent chain mismatch detail.')
   repeated={}
   if chain_repeats>1:
    planned,completed,pending_checks,duration_sum,minimum,maximum_chain,client_ticks,worker_ticks,first_repeat=values[12:]
    require(planned==completed==pending_checks==chain_repeats and
     0<minimum<=maximum_chain<=frequency and minimum*completed<=duration_sum<=maximum_chain*completed and
     maximum_chain<=duration_sum<=dend-dbegin and client_ticks+worker_ticks<=dend-dbegin and
     (first_repeat<completed if fault else first_repeat==(1<<64)-1),
     'Dependent chain repetition counts or aggregate timing differ.')
    repeated={'repetitions':completed,'pending_checks':pending_checks,'hardware_jobs':issued,'logical_stages':2*completed,
     'chains_per_second':completed*frequency/(dend-dbegin),'logical_stages_per_second':2*completed*frequency/(dend-dbegin),
     'duration_seconds':(dend-dbegin)/frequency,'chain_duration_ticks':{'sum':duration_sum,'min':minimum,'max':maximum_chain,'mean':duration_sum/completed},
     'chain_duration_window':'input preparation, submission, final fence wait, both output acquisitions and record release; comparison and digest excluded',
     'task_runtime':{'client_ticks':client_ticks,'worker_ticks':worker_ticks,'combined_ticks':client_ticks+worker_ticks},
     'first_mismatch_repetition':first_repeat if fault else None,
     'total_window':'resident iterations including input preparation, final fence wait, both output acquisitions and comparisons, digest, record release and progress checks; registration, stream construction and power claim excluded'}
   chain.append({'mode':'single_device_stream' if index else 'fifo_two_pending','boundary':boundary,
    'start_ticks':dbegin,'end_ticks':dend,'duration_ticks':dend-dbegin,'submitted_before_wait':before_wait,
    'queue_records':issued,'device_commands':2*chain_repeats,'interrupt_completions':done,'outputs_checked_after_final_completion':checks,
    'interstage_cpu_copy_bytes':0,'intermediate_device_completion_events':0 if index else chain_repeats,
    'completion_scope':'one final hardware event per chain; no interstage device completion notification' if index else 'one hardware event per stage; wait ends after the final stage fence completes',
    'functional_success':ok,'output_digest':match[f'dependent_digest_{index}'],'stream_bytes':stream,
    'irq_driver_elapsed_ticks':irq,'first_mismatch':None if not fault else {'stage':stage,'offset':at,'byte':byte},**repeated})
   previous=dend;matched=matched and ok
 require(matched==(status==0) and (match['outcome']=='MATCH')==matched,'Latency status disagrees with output checks/hash.')
 sentinel=(1<<64)-1
 if not bad:require((first_bad,offset,value)==(sentinel,sentinel,sentinel),'Unexpected latency mismatch detail.')
 else:require(first_bad<1000 and offset<len(reference) and value<256 and value!=reference[offset],
  'Latency first mismatch detail differs.')
 return {'normal_return':True,'runtime':True,'latency':geometry,'functional_success':matched,
  'controlled_negative':not matched,'jobs':total,'measured_jobs':jobs,'warmup_jobs':warmups,
  'outputs_checked':checked,'mismatches':bad,'status':status,'diagnostic_deadline':deadline,
  'first_mismatch':None if not bad else {'job':first_bad,'offset':offset,'byte':value},
  'output_digest':digest,'duration_digest':match['duration_digest'],**({'idle':idle_result} if idle_result else {}),'repeated_sequence':sequence,'dependent_chain':chain,
  'timing':{'frequency_hz':frequency,'start_ticks':start,'end_ticks':end,'duration_ticks':end-start,
   'duration_seconds':(end-start)/frequency,'jobs_per_second':jobs*frequency/(end-start),
   'latency_ticks':{'p50':p50,'p99':p99,'max':maximum},
   'latency_us':{k:v*1000000/frequency for k,v in {'p50':p50,'p99':p99,'max':maximum}.items()},
   'job_duration_ticks':durations,'warmup_excluded':True,'submit_to_doorbell':doorbell,
   'window':'legacy helper call' if not mode else 'input copy, output poison, tensor cache work, submit, interrupt wait, output acquire and record release',
   'total_window':'measured jobs including output comparison, digest and progress checks; warmup and close excluded',
   'task_runtime':{'client_ticks':client_cpu,'worker_ticks':worker_cpu,'combined_ticks':client_cpu+worker_cpu,
    'ticks_per_job':(client_cpu+worker_cpu)/jobs,'us_per_job':(client_cpu+worker_cpu)*1000000/frequency/jobs,
    'scope':'FreeRTOS scheduled runtime of client and runtime worker during the measured window, including output checks and digest; hardware waits while another task runs excluded; IRQ time charged to these tasks included; IRQs charged to idle and other task/kernel work excluded'},
   'caller_phases':None if not mode else {'submit_elapsed_ticks':submit_ticks,'acquire_elapsed_ticks':acquire_ticks,
    'irq_driver_elapsed_ticks':irq_ticks,
    'scope':'submit under local IRQ lock; output invalidate and release; direct IRQ driver including notification; phases can overlap when an IRQ follows unlock; wait, scheduler and preparation excluded; interrupts and preemption may contribute; CPU utilization is not measured'}},'log':text}


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
