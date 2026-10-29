#!/usr/bin/env python3
"""Build the NPU-local returning monitor around the accepted finite client.

With --interrupt, build the optional interrupt-driven finite NPU application
instead; apps/npu-interrupt/build.py selects it. With --shufflenet (interrupt
only), the image runs the published 125-operation ShuffleNetV2 graph with its
published input and recorded SDK output instead of the trusted convolution.
"""
import argparse,hashlib,json,shlex,shutil,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--evidence',type=Path,required=True)
p.add_argument('--baseline',type=Path,default=Path.home()/'.cache/sunstoneos/evidence/npu-finite-quiescence-20261018')
p.add_argument('--entry-checkpoint',action='store_true',help='return before any provider construction')
p.add_argument('--thermal',action='store_true',help='poll THS before and during finite execution')
p.add_argument('--interrupt',action='store_true',help='build the interrupt-driven variant')
p.add_argument('--restart',action='store_true',help='IRQ returning diagnostic: local stop/restart between two baseline jobs')
p.add_argument('--rate-852',action='store_true',help='PLL_NPU at 852 MHz, admitted only for a speed bin that lists 800 mV for it')
p.add_argument('--rate-492',action='store_true',help='the optional 492 MHz mode; the default is the vendor operating point for the speed bin')
p.add_argument('--shufflenet',action='store_true',help='the published ShuffleNetV2 graph in the large NN-pool profile (needs --interrupt)')
p.add_argument('--sdk-reference',type=Path,default=Path.home()/'.cache/sunstoneos/evidence/npu-complete-graph-proposal-20261019/sdk-reference',
 help='directory with network_binary.nb, published-input.dat and output0_2_1.dat')
a=p.parse_args()
if a.restart and (not a.interrupt or a.shufflenet or a.thermal or a.entry_checkpoint):p.error('Restart requires the baseline interrupt profile')
if a.shufflenet and not a.interrupt:p.error('The large graph profile requires --interrupt')
if sum((a.interrupt,a.entry_checkpoint,a.thermal))>1:p.error('Checkpoint, interrupt and thermal profiles are separate')
if (a.rate_852 or a.rate_492) and a.entry_checkpoint:p.error('The entry checkpoint starts no NPU; it has no rate')
if a.rate_852 and a.rate_492:p.error('Choose one rate')
root=Path(__file__).resolve().parents[2];e=a.evidence.resolve();base=a.baseline.resolve();e.mkdir(parents=True,exist_ok=True)
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def save(name,value):(e/name).write_text(json.dumps(value,indent=2)+'\n')
def run(name,c,cwd=None):
 with (e/(name+'.log')).open('w') as log:subprocess.run(c,cwd=cwd,stdout=log,stderr=subprocess.STDOUT,check=True)
# Reproduce the real single-core producer, preserving its configured toolchain,
# model, provider and service inputs. No fixture admission is generated here.
old=json.loads((base/'configure-command.json').read_text());old_root=next(v[:-len('/apps/npu-convolution-single-core')] for v in old if v.endswith('/apps/npu-convolution-single-core'))
config=[v.replace(old_root,str(root)).replace(str(base/'native'),str(e/'native')) for v in old]
if a.restart:
 assert sum(v.startswith('-DCMAKE_C_FLAGS=') for v in config)==1
 config=[v+' -DSUNSTONE_NPU_RESTART=1' if v.startswith('-DCMAKE_C_FLAGS=') else v for v in config]
if a.interrupt:config+=['-DA733_NPU_INTERRUPT=ON']
if a.shufflenet:
 # artifacts.py admits these only by their catalogue hashes (tools/npu-models.json).
 sdk=a.sdk_reference.resolve();files={'MODEL':sdk/'network_binary.nb','INPUT':sdk/'published-input.dat','REFERENCE':sdk/'output0_2_1.dat'}
 config=[next((f'-DA733_NPU_{k}={v}' for k,v in files.items() if x.startswith(f'-DA733_NPU_{k}=')),x) for x in config]
 assert all(f'-DA733_NPU_{k}={v}' in config for k,v in files.items())
 config+=['-DA733_NPU_LARGE_GRAPH=ON','-DA733_NPU_NN_POOL_GRAPH=ON']
if a.thermal:config+=['-DA733_NPU_FINITE_THERMAL=ON']
save('configure-command.json',config);run('configure',config)
run('base-build',['cmake','--build',str(e/'native'),'--target','a733-npu-convolution-single-core','-j6'])
entries=[x for x in json.loads((e/'native/compile_commands.json').read_text()) if 'a733-npu-convolution-single-core.dir/' in x['command']]
link=shlex.split((e/'native/CMakeFiles/a733-npu-convolution-single-core.dir/link.txt').read_text());commands=[]
obj=e/'objects';obj.mkdir(exist_ok=True)
def compile_like(suffix,source,name):
 entry=next(x for x in entries if x['file'].endswith(suffix));c=shlex.split(entry['command']);dest=obj/(name+'.obj')
 c[c.index('-o')+1]=str(dest);c[c.index('-c')+1]=str(source)
 c += ['-MD','-MF',str(dest)+'.d']
 if a.entry_checkpoint:c += ['-DSUNSTONE_NPU_ENTRY_CHECKPOINT=1']
 commands.append(c);run(name,c,cwd=entry['directory']);return dest
monitor=compile_like('/apps/storage-read-single-core/monitor.c',root/'apps/npu-finite/monitor.c','monitor')
platform=e/'platform.c';text=(e/'native/npu-single-core/platform.c').read_text();assert text.count('_Noreturn void platform_exit(')==1
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
checkpoint=compile_like('/apps/npu-convolution/native.c',root/'apps/npu-finite/checkpoint.c','checkpoint') if a.entry_checkpoint else None
# Bind the new monitor/cleanup/return sources into the existing authenticated
# image identity; the original producer identity already covers unchanged inputs.
identity={'base_build_id':(e/'native/npu-single-core/build-id.txt').read_text().strip(),'sources':{str(x.relative_to(root)):sha(x) for x in sorted(root/'apps/npu-finite'/n for n in ('monitor.c','return.c','return.S','return.h','link.ld','build.py'))},'platform_sha256':sha(platform),'profile':'npu-finite-return-v1'}
script=Path(__file__).resolve()
if a.interrupt:
 identity['irq_sources']={str(x.relative_to(root)):sha(x) for x in sorted(root/'apps/npu-interrupt'/n for n in ('irq.c','irq.h','irq-route.c','irq-route.h','build.py'))}
 identity['profile']='npu-interrupt-finite-return-v1';script=root/'apps/npu-interrupt/build.py'
if a.restart:identity['profile']='npu-interrupt-restart-return-v1'
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
 identity['profile']='npu-entry-memory-checkpoint-v1'
 identity['checkpoint_sources']={str(x.relative_to(root)):sha(x) for x in (root/'apps/npu-finite/checkpoint.c',root/'apps/npu-finite/checkpoint.h')}
build_id=hashlib.sha256(json.dumps(identity,sort_keys=True).encode()).hexdigest();source=e/'build-id.c';source.write_text('const unsigned char a733_smp_build_id[32]={'+','.join('0x'+build_id[i:i+2] for i in range(0,64,2))+'};\n')
build_obj=compile_like('/npu-single-core/build-id.c',source,'build-id')
replacements={'/apps/storage-read-single-core/monitor.c.obj':monitor,'/npu-single-core/platform.c.obj':platform_obj,'/npu-single-core/build-id.c.obj':build_obj}
newlink=[]
for value in link:
 if a.entry_checkpoint and value.endswith('/apps/npu-convolution/main.c.obj'):continue
 if a.entry_checkpoint and value.endswith('/apps/npu-convolution/native.c.obj'):value=str(checkpoint)
 elif native and value.endswith('/apps/npu-convolution/native.c.obj'):value=str(native)
 if value.startswith('-Wl,-T,'):value='-Wl,-T,'+str(root/'apps/npu-finite/link.ld')
 elif value.startswith('-Wl,-Map,'):value='-Wl,-Map,'+str(e/'image.map')
 else:
  for suffix,replacement in replacements.items():
   if value.endswith(suffix):value=str(replacement);break
 newlink.append(value)
newlink[-1]=str(e/'image.elf');newlink[-2:-2]=[str(ret),str(assembly),'-Wl,--undefined=a733_npu_application_artifact_hashes']
if a.thermal:newlink[-2:-2]=['-Wl,--undefined=npu_finite_thermal_profile']
if a.entry_checkpoint:newlink[-2:-2]=['-Wl,--undefined='+n for n in ('a733_npu_application_model','a733_npu_application_input','a733_npu_application_reference')]
commands.append(newlink);run('link',newlink,cwd=e/'native')
copy=['/usr/lib/llvm-21/bin/llvm-objcopy','-O','binary',str(e/'image.elf'),str(e/'image.bin')];commands.append(copy);run('binary',copy)
save('commands.json',commands)
# Preserve the effective compiler closure, including generated SRE/platform units.
inputs={root/'apps/npu-finite/link.ld',Path(__file__).resolve(),script,platform,source}
for dep in list((e/'native').rglob('*.obj.d'))+list(obj.glob('*.obj.d')):
 text=dep.read_text().replace('\\\n',' ');inputs.update(Path(x).resolve() for x in shlex.split(text.split(':',1)[1]))
closure={}
for i,path in enumerate(sorted(inputs)):
 dest=e/'compiled-inputs'/str(i)/path.name;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(path,dest);closure[str(path)]={'sha256':sha(path),'snapshot':str(dest.relative_to(e))}
save('input-identities.json',{'compiled_inputs':closure,'identity_inputs':identity,'build_id':build_id,'elf_sha256':sha(e/'image.elf'),'binary_sha256':sha(e/'image.bin'),'physical_access':False,'physical_admission':False,'note':'Complete app-local return implementation. Root architectural/physical authority review and real session/board evidence remain mandatory.'})
save('manifest.json',{'execution_profile':identity['profile'],'build_id':build_id,'final_elf':sha(e/'image.elf'),'binary_sha256':sha(e/'image.bin'),'physical_admission':False})
print('Built NPU interrupt finite returning image:' if a.interrupt else 'Built NPU finite returning image:',sha(e/'image.elf'))
