#!/usr/bin/env python3
"""Build the NPU-local returning monitor around the accepted finite client.

With --interrupt, build the optional interrupt-driven finite NPU application
instead; apps/npu-interrupt/build.py selects it. With --shufflenet (interrupt
only), the image runs the published 125-operation ShuffleNetV2 graph with its
published input and recorded SDK output instead of the trusted convolution.
With --runtime, the image runs the NPU runtime (apps/npu-convolution/native-runtime.c)
instead of the fixed-model convolution object: its client runs one job of each network
of apps/npu-finite/runtime-models.json through npu_job_run, and the image links an
admitted table generated from that list.
"""
import argparse,hashlib,json,shlex,shutil,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--evidence',type=Path,required=True)
p.add_argument('--baseline',type=Path,default=Path.home()/'.cache/sunstoneos/evidence/npu-finite-quiescence-20261018')
p.add_argument('--entry-provider-checkpoint',action='store_true',help='construct real native providers and tasks, then return before scheduling')
p.add_argument('--entry-checkpoint',action='store_true',help='return before any provider construction')
p.add_argument('--entry-irq-checkpoint',action='store_true',help='provider-free checkpoint after private IRQ97 capture and installation')
p.add_argument('--thermal',action='store_true',help='poll THS before and during finite execution')
p.add_argument('--interrupt',action='store_true',help='build the interrupt-driven variant')
p.add_argument('--startup-only',action='store_true',help='interrupt bootstrap OPEN/READY/CLOSE with no inference')
p.add_argument('--single-inference',action='store_true',help='interrupt bootstrap OPEN/READY, one job with copied output and REAP, then CLOSE')
p.add_argument('--active-cancel',action='store_true',help='finite diagnostic WAIT cancellation, controller-only recovery and one verified inference at 852 MHz')
p.add_argument('--build-jobs',type=int,default=1,help='maximum compiler processes (default: 1)')
p.add_argument('--host-elf-only',action='store_true',help='produce a linked host fixture ELF without a board binary')
p.add_argument('--restart',action='store_true',help='IRQ returning diagnostic: local stop/restart between two baseline jobs')
p.add_argument('--power-cycle',action='store_true',help='clean domain OFF/ON between two interrupt-driven baseline jobs')
p.add_argument('--rate-852',action='store_true',help='PLL_NPU at 852 MHz, admitted only for a speed bin that lists 800 mV for it')
p.add_argument('--rate-492',action='store_true',help='the optional 492 MHz mode; the default is the vendor operating point for the speed bin')
p.add_argument('--shufflenet',action='store_true',help='the published ShuffleNetV2 graph in the large NN-pool profile (needs --interrupt)')
p.add_argument('--runtime',action='store_true',help='the NPU runtime client: one job of each runtime-models.json network through npu_job_run')
p.add_argument('--model-switch',action='store_true',help='three runtime jobs: first model, second model, then first model again')
p.add_argument('--sustained',action='store_true',help='200 back-to-back runtime jobs alternating the two models; one final CLOSE')
p.add_argument('--latency',choices=('legacy','queue'),help='one checked warmup, then 1000 checked model-0 jobs through the helper or resident queue')
p.add_argument('--idle',choices=('off','gate'),help='queue latency only: matched power-owner control or 10 ms clock gating, plus 100 jobs after each 9/11/100 ms idle gap')
p.add_argument('--dependent-chain',action='store_true',help='queue latency only: append two queued models and their fused stream with an aliased output/input tensor')
p.add_argument('--chain-repeats',type=int,default=1,help='dependent chain: repeat each resident path 1..1000 times, checking both outputs after each final fence')
p.add_argument('--sequence',type=int,default=0,help='queue latency only: append one resident sequence repeating the same command 2..64 times')
p.add_argument('--sdk',action='store_true',help='two models through the public application NPU mailbox API')
p.add_argument('--artifacts',type=Path,default=Path.home()/'.cache/sunstoneos/build/npu-board/session-artifacts.json',
 help='--runtime: catalogue artifact paths by model name; the baseline fixture supplies its own model')
p.add_argument('--sdk-reference',type=Path,default=Path.home()/'.cache/sunstoneos/evidence/npu-complete-graph-proposal-20261019/sdk-reference',
 help='directory with network_binary.nb, published-input.dat and output0_2_1.dat')
a=p.parse_args()
if a.build_jobs<1:p.error('Build jobs must be positive')
if a.active_cancel:
 if a.latency or a.runtime or a.model_switch or a.sdk or a.sustained or a.restart or a.power_cycle or a.shufflenet or a.thermal or a.rate_492 or a.entry_checkpoint or a.entry_irq_checkpoint or a.entry_provider_checkpoint:
  p.error('Active cancellation requires the baseline single-core interrupt profile')
 a.startup_only=True;a.interrupt=True;a.rate_852=True
if a.idle and (a.latency!='queue' or a.sequence or a.dependent_chain):p.error('Idle benchmark requires --latency queue and excludes sequences and dependent chains')
if a.dependent_chain and (a.latency!='queue' or a.sequence):p.error('Dependent chain requires --latency queue and excludes --sequence')
if not 1<=a.chain_repeats<=1000 or a.chain_repeats!=1 and not a.dependent_chain:p.error('Chain repetitions require --dependent-chain and 1..1000 repetitions')
if a.sequence and (a.latency!='queue' or not 2<=a.sequence<=64):p.error('Sequence requires --latency queue and 2..64 repetitions')
if a.latency:
 if a.model_switch or a.sdk or a.sustained:p.error('Latency, sustained, SDK and model-switch are separate profiles')
 a.runtime=True
if a.power_cycle:a.restart=True;a.interrupt=True
if a.sustained:
 if a.model_switch or a.sdk:p.error("Sustained, SDK and model-switch are separate profiles")
 a.runtime=True
if a.model_switch:a.runtime=True
if a.sdk:
 if a.model_switch:p.error('SDK and model-switch are separate profiles')
 a.runtime=True
if a.runtime:
 if a.single_inference or a.startup_only or a.restart or a.shufflenet or a.thermal or a.rate_852 or a.rate_492 or a.entry_checkpoint or a.entry_irq_checkpoint or a.entry_provider_checkpoint:
  p.error('The runtime client is the baseline interrupt profile only')
 a.startup_only=True
if a.single_inference:a.startup_only=True
if a.startup_only:
 if a.restart or a.shufflenet or a.thermal or a.entry_checkpoint or a.entry_irq_checkpoint or a.entry_provider_checkpoint:p.error("Startup-only and single inference require the baseline interrupt profile")
 a.interrupt=True
if a.entry_provider_checkpoint:a.entry_irq_checkpoint=True
if a.entry_irq_checkpoint:a.entry_checkpoint=True
if a.entry_checkpoint and a.interrupt:p.error('Entry checkpoint constructs no interrupt provider')
if a.restart and (not a.interrupt or a.shufflenet or a.thermal or a.entry_checkpoint):p.error('Restart requires the baseline interrupt profile')
if a.shufflenet and not a.interrupt:p.error('The large graph profile requires --interrupt')
if sum((a.interrupt,a.entry_checkpoint,a.thermal))>1:p.error('Checkpoint, interrupt and thermal profiles are separate')
if (a.rate_852 or a.rate_492) and a.entry_checkpoint:p.error('The entry checkpoint starts no NPU; it has no rate')
if a.rate_852 and a.rate_492:p.error('Choose one rate')
root=Path(__file__).resolve().parents[2];e=a.evidence.resolve();base=a.baseline.resolve();e.mkdir(parents=True,exist_ok=True)
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def save(name,value):(e/name).write_text(json.dumps(value,indent=2)+'\n')
def module_from(path):
 import importlib.util
 s=importlib.util.spec_from_file_location(path.stem.replace('-','_'),path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
def run(name,c,cwd=None):
 with (e/(name+'.log')).open('w') as log:subprocess.run(c,cwd=cwd,stdout=log,stderr=subprocess.STDOUT,check=True)
# Reproduce the real single-core producer, preserving its configured toolchain,
# model, provider and service inputs. No fixture admission is generated here.
old=json.loads((base/'configure-command.json').read_text());old_root=next(v[:-len('/apps/npu-convolution-single-core')] for v in old if v.endswith('/apps/npu-convolution-single-core'))
config=[v.replace(old_root,str(root)).replace(str(base/'native'),str(e/'native')) for v in old]
config=[('-DSUNSTONE_LLD='+str(Path.home()/'.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld'))
 if v.startswith('-DSUNSTONE_LLD=') else v for v in config]
config+=['-DCMAKE_C_COMPILER=/usr/bin/clang-21','-DCMAKE_ASM_COMPILER=/usr/bin/clang-21']
# Every finite producer opts into RTC progress and armed-WDT behavior. The
# original retained producer and other single-core consumers remain unchanged.
config=[v+' -DSUNSTONE_CRUMBS=1 -DSUNSTONE_NPU_FINITE_WATCHDOG=1 -DINCLUDE_xTaskDelayUntil=1' if v.startswith('-DCMAKE_C_FLAGS=') else v for v in config]
if a.active_cancel:
 config=[v+' -DSUNSTONE_NPU_ACTIVE_CANCEL=1' if v.startswith('-DCMAKE_C_FLAGS=') else v for v in config]
if a.latency:
 assert sum(v.startswith('-DCMAKE_C_FLAGS=') for v in config)==1
 config=[v+' -include '+str(root/'apps/npu-finite/latency-stats.h') if v.startswith('-DCMAKE_C_FLAGS=') else v for v in config]
if a.idle:
 config=[v+' -DSUNSTONE_NPU_IDLE_BENCH=1 -DSUNSTONE_DEADLINE_SINGLE_CORE=1 -DINCLUDE_uxTaskPriorityGet=1' if v.startswith('-DCMAKE_C_FLAGS=') else v for v in config]
if a.restart:
 assert sum(v.startswith('-DCMAKE_C_FLAGS=') for v in config)==1
 config=[v+' -DSUNSTONE_NPU_RESTART=1'+(' -DSUNSTONE_NPU_POWER_CYCLE=1' if a.power_cycle else '') if v.startswith('-DCMAKE_C_FLAGS=') else v for v in config]
if a.interrupt or a.entry_provider_checkpoint:config+=['-DA733_NPU_INTERRUPT=ON']
if a.shufflenet:
 # artifacts.py admits these only by their catalogue hashes (tools/npu-models.json).
 sdk=a.sdk_reference.resolve();files={'MODEL':sdk/'network_binary.nb','INPUT':sdk/'published-input.dat','REFERENCE':sdk/'output0_2_1.dat'}
 config=[next((f'-DA733_NPU_{k}={v}' for k,v in files.items() if x.startswith(f'-DA733_NPU_{k}=')),x) for x in config]
 assert all(f'-DA733_NPU_{k}={v}' in config for k,v in files.items())
 config+=['-DA733_NPU_LARGE_GRAPH=ON','-DA733_NPU_NN_POOL_GRAPH=ON']
if a.thermal:config+=['-DA733_NPU_FINITE_THERMAL=ON']
save('configure-command.json',config);run('configure',config)
run('base-build',['cmake','--build',str(e/'native'),'--target','a733-npu-convolution-single-core','-j'+str(a.build_jobs)])
entries=[x for x in json.loads((e/'native/compile_commands.json').read_text()) if 'a733-npu-convolution-single-core.dir/' in x['command']]
link=shlex.split((e/'native/CMakeFiles/a733-npu-convolution-single-core.dir/link.txt').read_text());commands=[]
obj=e/'objects';obj.mkdir(exist_ok=True)
def compile_like(suffix,source,name):
 entry=next(x for x in entries if x['file'].endswith(suffix));c=shlex.split(entry['command']);dest=obj/(name+'.obj')
 c[c.index('-o')+1]=str(dest);c[c.index('-c')+1]=str(source)
 c += ['-MD','-MF',str(dest)+'.d']
 if source==root/'apps/npu-finite/monitor.c':c += ['-DSUNSTONE_NPU_BASE_MONITOR="'+str(e/'monitor-base.c')+'"']
 if a.entry_checkpoint:c += ['-DSUNSTONE_NPU_ENTRY_CHECKPOINT=1']
 if a.startup_only:c += ['-DSUNSTONE_NPU_STARTUP_ONLY=1']
 if a.single_inference:c += ['-DSUNSTONE_NPU_SINGLE_INFERENCE=1']
 if a.runtime:c += ['-DSUNSTONE_NPU_RUNTIME_RETURN=1']
 if a.model_switch:c += ['-DSUNSTONE_NPU_MODEL_SWITCH=1']
 if a.sdk:c += ['-DSUNSTONE_NPU_SDK_RETURN=1']
 if a.sustained:c += ['-DSUNSTONE_NPU_SUSTAINED=1']
 if a.latency:c += ['-DSUNSTONE_NPU_LATENCY=1','-DSUNSTONE_NPU_QUEUE_TIMING=1',f'-DSUNSTONE_NPU_LATENCY_QUEUE={int(a.latency=="queue")}',f'-DSUNSTONE_NPU_LATENCY_SEQUENCE={a.sequence}',f'-DSUNSTONE_NPU_DEPENDENT_CHAIN={int(a.dependent_chain)}',f'-DSUNSTONE_NPU_CHAIN_REPEATS={a.chain_repeats}']
 if a.idle:c += [f'-DSUNSTONE_NPU_IDLE_GATE={int(a.idle=="gate")}']
 if a.entry_provider_checkpoint:c += ['-DSUNSTONE_NPU_ENTRY_PROVIDER_CHECKPOINT=1','-DSUNSTONE_NPU_INTERRUPT=1']
 if a.entry_irq_checkpoint:
  c += ['-DSUNSTONE_NPU_ENTRY_IRQ_CHECKPOINT=1']
  if source==root/'apps/npu-finite/monitor.c':c += ['-DSUNSTONE_NPU_INTERRUPT=1']
 commands.append(c);run(name,c,cwd=entry['directory']);return dest
# Adapt the shared monitor privately so no storage/GPU admission changes.
monitor_base=(root/'apps/storage-read-single-core/monitor.c').read_text()
monitor_old='if((mmio_read(0x02050014)&1)||(mmio_read(0x02050010)&0x100)||'
assert monitor_base.count(monitor_old)==1
monitor_base=monitor_base.replace(monitor_old,'if(!npu_finite_watchdog_admitted()||')
monitor_old='    volatile const struct a733_smp_contract *source=(void *)A733_SMP_CONTRACT_ADDRESS;'
assert monitor_base.count(monitor_old)==1
monitor_base=monitor_base.replace(monitor_old,monitor_old+'\n    sunstone_crumb_el3_start(source->epoch);\n    sunstone_crumb_el3(SUNSTONE_CRUMB_EL3_ENTRY,0);')
monitor_old='    REPORT->magic=A733_SMP_RESULT_MAGIC;REPORT->epoch=admission.epoch;'
assert monitor_base.count(monitor_old)==1
monitor_base=monitor_base.replace(monitor_old,'    sunstone_crumb_el3(SUNSTONE_CRUMB_EL3_ADMITTED,0);\n'+monitor_old)
monitor_old='    REPORT->stage=5;__asm__ volatile("dsb sy":::"memory");'
assert monitor_base.count(monitor_old)==1
monitor_base=monitor_base.replace(monitor_old,monitor_old+'\n    sunstone_crumb_el3(SUNSTONE_CRUMB_EL3_STOP,((status>15?15:status)<<12)|(detail&0xfff));\n    npu_finite_watchdog_stop();')
monitor_base=monitor_base.replace('#include "../npu-interrupt/irq-route.h"','#include "'+str(root/'apps/npu-interrupt/irq-route.h')+'"')
(e/'monitor-base.c').write_text(monitor_base)
monitor=compile_like('/apps/storage-read-single-core/monitor.c',root/'apps/npu-finite/monitor.c','monitor')
platform=e/'platform.c';text=(e/'native/npu-single-core/platform.c').read_text();assert text.count('_Noreturn void platform_exit(')==1
if a.idle:
 anchor='    mmio_write(GICR_SGI + 0x100, 1U << TIMER_IRQ);'
 assert text.count(anchor)==1
 text='#include "'+str(root/'apps/npu-finite/deadline.h')+'"\n'+text.replace(anchor,anchor+'\n    npu_finite_deadline_setup();')
 anchor='void vApplicationFPUSafeIRQHandler(uint64_t interrupt)\n{'
 assert text.count(anchor)==1
 text=text.replace(anchor,anchor+'\n    if(interrupt==27){sunstone_deadline_irq();return;}')
platform.write_text(text.replace('_Noreturn void platform_exit(','_Noreturn void npu_finite_legacy_exit('))
platform_obj=compile_like('/npu-single-core/platform.c',platform,'platform')
ret=compile_like('/apps/npu-convolution/native.c',root/'apps/npu-finite/return.c','return')
# An explicit rate recompiles only the application's native unit with its selection.
native=None
if a.rate_852 or a.rate_492:
 rate='852' if a.rate_852 else '492'
 entry=next(x for x in entries if x['file'].endswith('/apps/npu-convolution/native.c'));c=shlex.split(entry['command'])
 native=obj/f'native-{rate}.obj';c[c.index('-o')+1]=str(native);c+=[f'-DSUNSTONE_NPU_RATE_{rate}=1','-MD','-MF',str(native)+'.d']
 commands.append(c);run(f'native-{rate}',c,cwd=entry['directory'])
assembly=compile_like('/arch/aarch64/cache.S',root/'apps/npu-finite/return.S','return-assembly')
watchdog=compile_like('/apps/npu-convolution/native.c',root/'apps/npu-finite/watchdog.c','watchdog')
vectors=compile_like('/arch/aarch64/vectors.S',root/'apps/npu-finite/vectors.S','vectors')
boot_start=compile_like('/apps/storage-read-single-core/start.S',root/'apps/npu-finite/start.S','boot-start')
# The ordinary finite client also starts the same supervisor after admission.
finite_main=None
if not a.startup_only and not a.entry_checkpoint:
 text=(root/'apps/npu-convolution/main.c').read_text()
 old='    configASSERT(a733_npu_application_observer_admit());'
 assert text.count(old)==1
 text='#include "'+str(root/'apps/npu-finite/watchdog.h')+'"\n'+text.replace(old,old+'\n    configASSERT(npu_finite_watchdog_start());')
 (e/'finite-main.c').write_text(text)
 finite_main=compile_like('/apps/npu-convolution/main.c',e/'finite-main.c','finite-main')
startup_main=compile_like('/apps/npu-convolution/main.c',root/'apps/npu-finite/startup.c','startup-client') if a.startup_only and not a.runtime else None
runtime_objects=[];runtime_identity={}
if a.runtime:
 # The networks in job order, each checked against its reviewed catalogue
 # entry; the baseline fixture (A733_NPU_MODEL) supplies the model it names.
 catalogue_tool=module_from(root/'tools/npu-session-catalogue.py')
 runtime_list=root/('apps/npu-finite/chain-models.json' if a.dependent_chain else 'apps/npu-finite/runtime-models.json')
 job_plan=root/'apps/npu-finite/switch-jobs.json'
 listed=catalogue_tool.application_models([runtime_list])
 artifacts=json.loads(a.artifacts.read_text())
 fixture={k:next(Path(v.split('=',1)[1]) for v in config if v.startswith(f'-DA733_NPU_{k}=')) for k in ('MODEL','INPUT','REFERENCE')}
 networks=[]
 for m in listed:
  if sha(fixture['MODEL'])==m['model']['sha256']:paths={'model':fixture['MODEL'],'inputs':[fixture['INPUT']],'references':[fixture['REFERENCE']]}
  else:
   item=catalogue_tool.artifact_files(artifacts,m['name']);paths={'model':Path(item['model']),'inputs':[Path(x) for x in item['inputs']],'references':[Path(x) for x in item['references']]}
  model=paths['model'].read_bytes()
  assert len(model)==m['model']['bytes'] and sha(paths['model'])==m['model']['sha256'],m['name']
  assert len(paths['inputs'])==len(m['inputs']) and len(paths['references'])==len(m['outputs']),m['name']
  for path,t in zip(paths['inputs']+paths['references'],m['inputs']+m['outputs']):
   assert path.stat().st_size==t['logical_bytes']==t['reference']['bytes'] and sha(path)==t['reference']['sha256'],(m['name'],path)
  networks.append({'name':m['name'],'inputs':len(m['inputs']),'outputs':len(m['outputs']),'fixture':paths['model']==fixture['MODEL'],'model':model,'input':b''.join(x.read_bytes() for x in paths['inputs']),
   'reference':b''.join(x.read_bytes() for x in paths['references']),'paths':{k:[str(x) for x in v] if isinstance(v,list) else str(v) for k,v in paths.items()}})
 assert len(networks)==2 and sha(fixture['MODEL'])==listed[0]['model']['sha256'],'network 0 is the baseline fixture; the client runs two networks'
 if a.model_switch:
  order=json.loads(job_plan.read_text())['job_order']
  assert order==[0,1,0] and networks[0]['model']!=networks[1]['model']
  networks=[networks[i] for i in order]
  assert 512+sum(2*len(n['reference'])+(len(n['reference'])+63)//64 for n in networks)<=16127,'compact output exceeds report capacity'
 assert all(len(n['reference'])<=4096 for n in networks)
 generated=e/'generated';generated.mkdir(exist_ok=True)
 def array(name,data):return f'static const unsigned char {name}[{len(data)}] __attribute__((aligned(64)))={{'+','.join(map(str,data))+'};\n'
 text='/* Generated by apps/npu-finite/build.py --runtime from apps/npu-finite/runtime-models.json. */\n#include "'+str(root/'apps/npu-finite/networks.h')+'"\n'
 # The baseline fixture's network loads the image's selected immutable model
 # and input (artifacts.c), which the host's image admission names.
 text+='#include "a733_npu_application.h"\n#define model_0 a733_npu_application_model\n#define input_0 a733_npu_application_input\n'
 for i,n in enumerate(networks):
  if n['fixture'] and i:text+=f'#define model_{i} a733_npu_application_model\n#define input_{i} a733_npu_application_input\n'
  text+=f'/* {n["name"]} */\n'+('' if n['fixture'] else array(f'model_{i}',n['model'])+array(f'input_{i}',n['input']))
  text+=f'const unsigned char npu_finite_reference_{i}[{len(n["reference"])}] __attribute__((used,retain))={{'+','.join(map(str,n['reference']))+'};\n'
 text+='const struct npu_finite_network npu_finite_networks[NPU_FINITE_NETWORKS]={\n'+''.join(
  f' {{model_{i},input_{i},npu_finite_reference_{i},{len(n["model"])}u,{len(n["input"])}u,{len(n["reference"])}u}},\n' for i,n in enumerate(networks))+'};\n'
 text+='const struct ss_npu_model npu_finite_models[NPU_FINITE_NETWORKS]={\n'+''.join(
  '{'+json.dumps(n['name'])+f',{n["inputs"]}u,{n["outputs"]}u,{len(n["input"])}u,{len(n["reference"])}u'+ '},\n' for n in networks)+'};\n'
 (generated/'networks.c').write_text(text)
 (generated/'npu_admitted_models.c').write_text(catalogue_tool.table(listed))
 runtime_objects=[compile_like('/apps/npu-convolution/native.c',root/'apps/npu-convolution/native-runtime.c','native-runtime'),
  compile_like('/apps/npu-convolution/main.c',root/('apps/npu-finite/queue-client.c' if a.latency else 'apps/npu-sdk/finite-client.c' if a.sdk else 'apps/npu-finite/runtime-client.c'),'runtime-client'),
  compile_like('/platform/a733/npu_irq.c',root/'apps/npu-finite/runtime-irq.c','runtime-irq'),
  compile_like('/apps/npu-convolution/native.c',generated/'networks.c','networks')]
 for n in ('a733_npu_runtime','a733_npu_runtime_interim','npu_job'):
  runtime_objects.append(compile_like('/drivers/npu/npu_service.c',root/'drivers/npu'/(n+'.c'),n))
 if a.latency:
  for n in ('npu_queue','npu_runtime_queue','npu_idle'):
   runtime_objects.append(compile_like('/drivers/npu/npu_service.c',root/'drivers/npu'/(n+'.c'),n))
  if a.sequence or a.dependent_chain:runtime_objects.append(compile_like('/drivers/npu/npu_service.c',root/'drivers/npu/a733_npu_sequence.c','npu-sequence'))
 if a.idle:
  for source in ('runtime/deadline.c','runtime/deadline-a733.c','apps/npu-finite/deadline.c'):
   runtime_objects.append(compile_like('/drivers/npu/npu_service.c',root/source,source.replace('/','-').removesuffix('.c')+'-idle'))
 if a.sdk:runtime_objects.append(compile_like('/drivers/npu/npu_service.c',root/'runtime/npu_app.c','npu-app'))
 runtime_objects.append(compile_like('/drivers/npu/npu_service.c',generated/'npu_admitted_models.c','npu_admitted_models'))
 runtime_identity={'runtime_sources':{str(x.relative_to(root)):sha(x) for x in sorted([root/'apps/npu-convolution/native-runtime.c',root/'apps/npu-convolution/native-runtime.h',
  root/'apps/npu-finite/runtime-client.c',root/'apps/npu-finite/runtime-irq.c',root/'apps/npu-finite/networks.h',root/'apps/npu-finite/runtime-models.json',
  root/'apps/npu-finite/startup.h',root/'apps/npu-finite/crumb.h',root/'platform/a733/npu_irq.c',root/'include/a733_npu_irq.h',
  root/'drivers/npu/a733_npu_runtime.c',root/'drivers/npu/a733_npu_runtime_interim.c',root/'drivers/npu/npu_job.c',root/'include/npu_job.h',root/'include/npu_crumb.h',
  root/'include/npu_admitted_models.h',root/'include/npu_runtime.h',root/'tools/npu-session-catalogue.py',root/'tools/npu-session-models.json',runtime_list]+([job_plan] if a.model_switch else [])+([root/'apps/npu-sdk/finite-client.c',root/'runtime/npu_app.c',root/'include/sunstone_app.h',root/'include/sunstone_npu_service.h'] if a.sdk else []))},
  'networks':[{'name':n['name'],'model_sha256':hashlib.sha256(n['model']).hexdigest(),'input_sha256':hashlib.sha256(n['input']).hexdigest(),
   'reference_sha256':hashlib.sha256(n['reference']).hexdigest()} for n in networks],
  'generated':{'networks.c':sha(generated/'networks.c'),'npu_admitted_models.c':sha(generated/'npu_admitted_models.c')}}
 save('networks.json',[{k:v for k,v in n.items() if k in ('name','paths')} for n in networks])
 if a.latency:
  runtime_identity['runtime_sources'].update({str(path.relative_to(root)):sha(path) for path in
   [root/'apps/npu-finite/queue-client.c',root/'apps/npu-finite/latency-stats.h',root/'include/npu_queue.h',root/'include/npu_runtime_queue.h',
    root/'drivers/npu/npu_queue.c',root/'drivers/npu/npu_runtime_queue.c']})
  if a.idle:runtime_identity['runtime_sources'].update({str(path.relative_to(root)):sha(path) for path in
   [root/'apps/npu-finite/idle-client.h',root/'apps/npu-finite/deadline.c',root/'apps/npu-finite/deadline.h',
    root/'runtime/deadline.c',root/'runtime/deadline-a733.c',root/'include/sunstone_deadline.h']})
  runtime_identity['runtime_sources'].update({str(path.relative_to(root)):sha(path) for path in
   [root/'include/npu_idle.h',root/'drivers/npu/npu_idle.c']})
  if a.dependent_chain:runtime_identity['runtime_sources'].update({str(path.relative_to(root)):sha(path) for path in
   [root/'apps/npu-finite/dependent-client.h',root/'apps/npu-finite/chain-models.json']})
  if a.sequence or a.dependent_chain:runtime_identity['runtime_sources'].update({str(path.relative_to(root)):sha(path) for path in
   [root/'include/a733_npu_sequence.h',root/'drivers/npu/a733_npu_sequence.c']})
checkpoint=compile_like('/apps/npu-convolution/native.c',root/'apps/npu-finite/checkpoint.c','checkpoint') if a.entry_checkpoint else None
checkpoint_irq=compile_like('/apps/npu-convolution/native.c',root/'apps/npu-interrupt/irq-route.c','checkpoint-irq-route') if a.entry_irq_checkpoint else None
# Bind the new monitor/cleanup/return sources into the existing authenticated
# image identity; the original producer identity already covers unchanged inputs.
identity={'base_build_id':(e/'native/npu-single-core/build-id.txt').read_text().strip(),'sources':{str(x.relative_to(root)):sha(x) for x in sorted(root/'apps/npu-finite'/n for n in ('monitor.c','return.c','return.S','return.h','link.ld','build.py'))},'platform_sha256':sha(platform),'profile':'npu-finite-return-v1'}
script=Path(__file__).resolve()
if a.interrupt:
 identity['irq_sources']={str(x.relative_to(root)):sha(x) for x in sorted(root/n for n in ('platform/a733/npu_irq.c','include/a733_npu_irq.h','apps/npu-interrupt/irq-route.c','apps/npu-interrupt/irq-route.h','apps/npu-interrupt/build.py'))}
 identity['profile']='npu-interrupt-finite-return-v1';script=root/'apps/npu-interrupt/build.py'
if a.restart:identity['profile']='npu-interrupt-restart-return-v1'
if a.startup_only:
 identity['profile']='npu-startup-return-v1'
 identity['startup_sources']={str(x.relative_to(root)):sha(x) for x in (root/'apps/npu-finite/startup.c',root/'apps/npu-finite/startup.h',root/'apps/npu-finite/crumb.h')}
if a.single_inference:identity['profile']='npu-single-inference-return-v1'
if a.active_cancel:identity['profile']='npu-active-cancel-return-v1'
if a.runtime:identity.update(profile='npu-runtime-return-v1',**runtime_identity)
if a.model_switch:identity.update(profile='npu-model-switch-return-v1')
if a.sdk:identity.update(profile='npu-sdk-return-v1')
if a.sustained:identity.update(sustained={'jobs':200,'models':2,'order':'job-index-modulo-model-count'})
if a.latency:identity.update(profile='npu-latency-return-v1',latency={'mode':a.latency,'measured_jobs':1000,'warmup_jobs':1,'model':0,'sequence_repeats':a.sequence,'doorbell_timing':True,**({'idle':{'enabled':a.idle=='gate','cycles':100,'gaps_us':[9000,11000,100000],'threshold_us':10000,'resume_budget_us':100}} if a.idle else {}),**({'dependent_chain':True,**({'chain_repeats':a.chain_repeats} if a.chain_repeats!=1 else {})} if a.dependent_chain else {})})
if a.power_cycle:identity.update(profile='npu-power-cycle-return-v1')
if a.thermal:
 identity['profile']='npu-finite-thermal-return-v1'
 identity['thermal_sources']={str(x.relative_to(root)):sha(x) for x in (root/'apps/npu-finite/thermal.c',root/'apps/npu-finite/thermal.h')}
if a.rate_852:
 identity['npu_rate_mhz']=852
elif a.rate_492:
 identity['npu_rate_mhz']=492
else:
 identity['npu_rate']='vendor-point-for-speed-bin'
if a.shufflenet:
 identity['model']='shufflenet-v2-published';identity['large_graph']=True
if a.entry_checkpoint:
 identity['profile']='npu-entry-mbus-checkpoint-v1'
 identity['checkpoint_sources']={str(x.relative_to(root)):sha(x) for x in (root/'apps/npu-finite/checkpoint.c',root/'apps/npu-finite/checkpoint.h')}
if a.entry_irq_checkpoint:
 identity['profile']='npu-entry-irq-checkpoint-v1'
 identity['checkpoint_irq_sources']={str(x.relative_to(root)):sha(x) for x in (root/'apps/npu-interrupt/irq-route.c',root/'apps/npu-interrupt/irq-route.h')}
if a.entry_provider_checkpoint:
 identity['profile']='npu-entry-provider-checkpoint-v1'
 identity['provider_checkpoint_sources']={'apps/npu-finite/provider-checkpoint.h':sha(root/'apps/npu-finite/provider-checkpoint.h')}
identity['watchdog_sources']={str(x.relative_to(root)):sha(x) for x in (root/'apps/npu-finite/watchdog.c',root/'apps/npu-finite/watchdog.h',root/'apps/npu-finite/vectors.S',root/'apps/npu-finite/start.S',root/'include/sunstone_crumb.h',root/'apps/storage-read-single-core/monitor.c')}
identity['watchdog']={'mode':0xb1,'session_seconds':20,'reset_route':'whole-system-no-redirect','active_npu_reset_measured':False}
build_id=hashlib.sha256(json.dumps(identity,sort_keys=True).encode()).hexdigest();source=e/'build-id.c';source.write_text('const unsigned char a733_smp_build_id[32]={'+','.join('0x'+build_id[i:i+2] for i in range(0,64,2))+'};\n')
build_obj=compile_like('/npu-single-core/build-id.c',source,'build-id')
replacements={'/arch/aarch64/vectors.S.obj':vectors,'/apps/storage-read-single-core/start.S.obj':boot_start,'/apps/storage-read-single-core/monitor.c.obj':monitor,'/npu-single-core/platform.c.obj':platform_obj,'/npu-single-core/build-id.c.obj':build_obj}
# The runtime image replaces the convolution object, its reservation and the
# single-core client and IRQ binding, and reserves the runtime's 6 MiB parent:
# read-only, read-window and write-window groups of 2 MiB for one slot.
dropped=('/apps/npu-convolution/native.c.obj','/apps/npu-convolution/main.c.obj','/platform/a733/npu_irq.c.obj',
 '/drivers/npu/npu_convolution.c.obj','/platform/a733/npu_reserved.c.obj') if a.runtime else ()
newlink=[]
for value in link:
 if any(value.endswith(x) for x in dropped):continue
 if a.runtime and value=='-Wl,--defsym=__device_pool_size=2097152':value='-Wl,--defsym=__npu_runtime_parent_bytes=6291456'
 if startup_main and value.endswith('/apps/npu-convolution/main.c.obj'):value=str(startup_main)
 elif finite_main and value.endswith('/apps/npu-convolution/main.c.obj'):value=str(finite_main)
 if a.entry_checkpoint and value.endswith('/apps/npu-convolution/main.c.obj'):continue
 if a.entry_checkpoint and value.endswith('/apps/npu-convolution/native.c.obj'):value=str(checkpoint)
 elif native and value.endswith('/apps/npu-convolution/native.c.obj'):value=str(native)
 if value.startswith('-Wl,-T,'):value='-Wl,-T,'+str(root/'apps/npu-finite/link.ld')
 elif value.startswith('-Wl,-Map,'):value='-Wl,-Map,'+str(e/'image.map')
 else:
  for suffix,replacement in replacements.items():
   if value.endswith(suffix):value=str(replacement);break
 newlink.append(value)
newlink[-1]=str(e/'image.elf');newlink[-2:-2]=[str(ret),str(assembly),str(watchdog),'-Wl,--undefined=os_cpu_return_watchdog','-Wl,--undefined=a733_npu_application_artifact_hashes']
if a.thermal:newlink[-2:-2]=['-Wl,--undefined=npu_finite_thermal_profile']
if a.runtime:newlink[-2:-2]=[str(x) for x in runtime_objects]+['-Wl,--undefined='+n for n in ('npu_startup_return_profile','a733_npu_application_reference')]
if checkpoint_irq and not a.entry_provider_checkpoint:newlink[-2:-2]=[str(checkpoint_irq)]
if a.entry_checkpoint:newlink[-2:-2]=['-Wl,--undefined='+n for n in ('a733_npu_application_model','a733_npu_application_input','a733_npu_application_reference')]
commands.append(newlink);run('link',newlink,cwd=e/'native')
if not a.host_elf_only:
 copy=['/usr/lib/llvm-21/bin/llvm-objcopy','-O','binary',str(e/'image.elf'),str(e/'image.bin')];commands.append(copy);run('binary',copy)
save('commands.json',commands)
# Preserve the effective compiler closure, including generated SRE/platform units.
inputs={root/'apps/npu-finite/link.ld',Path(__file__).resolve(),script,platform,source,e/'monitor-base.c'}
if finite_main:inputs.add(e/'finite-main.c')
if a.runtime:inputs|={e/'generated/networks.c',e/'generated/npu_admitted_models.c',root/'apps/npu-finite/runtime-models.json',a.artifacts.resolve()}
for dep in list((e/'native').rglob('*.obj.d'))+list(obj.glob('*.obj.d')):
 text=dep.read_text().replace('\\\n',' ');inputs.update(Path(x).resolve() for x in shlex.split(text.split(':',1)[1]))
closure={}
for i,path in enumerate(sorted(inputs)):
 dest=e/'compiled-inputs'/str(i)/path.name;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(path,dest);closure[str(path)]={'sha256':sha(path),'snapshot':str(dest.relative_to(e))}
save('input-identities.json',{'compiled_inputs':closure,'identity_inputs':identity,'build_id':build_id,'elf_sha256':sha(e/'image.elf'),'binary_sha256':None if a.host_elf_only else sha(e/'image.bin'),'physical_access':False,'physical_admission':False,'host_fixture_only':a.host_elf_only,'note':'Native NPU return image; host build evidence does not establish physical operation.'})
save('manifest.json',{'execution_profile':identity['profile'],'build_id':build_id,'final_elf':sha(e/'image.elf'),'binary_sha256':None if a.host_elf_only else sha(e/'image.bin'),'host_fixture_only':a.host_elf_only,'physical_admission':False})
print('Built NPU runtime finite returning image:' if a.runtime else 'Built NPU interrupt finite returning image:' if a.interrupt else 'Built NPU finite returning image:',sha(e/'image.elf'))
