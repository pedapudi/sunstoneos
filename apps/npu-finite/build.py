#!/usr/bin/env python3
"""Build the NPU-local returning monitor around the accepted finite client.

With --interrupt, build the optional interrupt-driven finite NPU application
instead; apps/npu-interrupt/build.py selects it.
"""
import argparse,hashlib,json,shlex,shutil,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--evidence',type=Path,required=True)
p.add_argument('--baseline',type=Path,default=Path.home()/'.cache/sunstoneos/evidence/npu-finite-quiescence-20261018')
p.add_argument('--interrupt',action='store_true',help='build the interrupt-driven variant')
a=p.parse_args();root=Path(__file__).resolve().parents[2];e=a.evidence.resolve();base=a.baseline.resolve();e.mkdir(parents=True,exist_ok=True)
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def save(name,value):(e/name).write_text(json.dumps(value,indent=2)+'\n')
def run(name,c,cwd=None):
 with (e/(name+'.log')).open('w') as log:subprocess.run(c,cwd=cwd,stdout=log,stderr=subprocess.STDOUT,check=True)
# Reproduce the real single-core producer, preserving its configured toolchain,
# model, provider and service inputs. No fixture admission is generated here.
old=json.loads((base/'configure-command.json').read_text());old_root=next(v[:-len('/apps/npu-convolution-single-core')] for v in old if v.endswith('/apps/npu-convolution-single-core'))
config=[v.replace(old_root,str(root)).replace(str(base/'native'),str(e/'native')) for v in old]
if a.interrupt:config+=['-DA733_NPU_INTERRUPT=ON']
save('configure-command.json',config);run('configure',config)
run('base-build',['cmake','--build',str(e/'native'),'--target','a733-npu-convolution-single-core','-j8'])
entries=[x for x in json.loads((e/'native/compile_commands.json').read_text()) if 'a733-npu-convolution-single-core.dir/' in x['command']]
link=shlex.split((e/'native/CMakeFiles/a733-npu-convolution-single-core.dir/link.txt').read_text());commands=[]
obj=e/'objects';obj.mkdir(exist_ok=True)
def compile_like(suffix,source,name):
 entry=next(x for x in entries if x['file'].endswith(suffix));c=shlex.split(entry['command']);dest=obj/(name+'.obj')
 c[c.index('-o')+1]=str(dest);c[c.index('-c')+1]=str(source)
 c += ['-MD','-MF',str(dest)+'.d'];commands.append(c);run(name,c,cwd=entry['directory']);return dest
monitor=compile_like('/apps/storage-read-single-core/monitor.c',root/'apps/npu-finite/monitor.c','monitor')
platform=e/'platform.c';text=(e/'native/npu-single-core/platform.c').read_text();assert text.count('_Noreturn void platform_exit(')==1
platform.write_text(text.replace('_Noreturn void platform_exit(','_Noreturn void npu_finite_legacy_exit('))
platform_obj=compile_like('/npu-single-core/platform.c',platform,'platform')
ret=compile_like('/apps/npu-convolution/native.c',root/'apps/npu-finite/return.c','return')
assembly=compile_like('/arch/aarch64/cache.S',root/'apps/npu-finite/return.S','return-assembly')
# Bind the new monitor/cleanup/return sources into the existing authenticated
# image identity; the original producer identity already covers unchanged inputs.
identity={'base_build_id':(e/'native/npu-single-core/build-id.txt').read_text().strip(),'sources':{str(x.relative_to(root)):sha(x) for x in sorted(root/'apps/npu-finite'/n for n in ('monitor.c','return.c','return.S','return.h','link.ld','build.py'))},'platform_sha256':sha(platform),'profile':'npu-finite-return-v1'}
script=Path(__file__).resolve()
if a.interrupt:
 identity['irq_sources']={str(x.relative_to(root)):sha(x) for x in sorted(root/'apps/npu-interrupt'/n for n in ('irq.c','irq.h','irq-route.c','irq-route.h','build.py'))}
 identity['profile']='npu-interrupt-finite-return-v1';script=root/'apps/npu-interrupt/build.py'
build_id=hashlib.sha256(json.dumps(identity,sort_keys=True).encode()).hexdigest();source=e/'build-id.c';source.write_text('const unsigned char a733_smp_build_id[32]={'+','.join('0x'+build_id[i:i+2] for i in range(0,64,2))+'};\n')
build_obj=compile_like('/npu-single-core/build-id.c',source,'build-id')
replacements={'/apps/storage-read-single-core/monitor.c.obj':monitor,'/npu-single-core/platform.c.obj':platform_obj,'/npu-single-core/build-id.c.obj':build_obj}
newlink=[]
for value in link:
 if value.startswith('-Wl,-T,'):value='-Wl,-T,'+str(root/'apps/npu-finite/link.ld')
 elif value.startswith('-Wl,-Map,'):value='-Wl,-Map,'+str(e/'image.map')
 else:
  for suffix,replacement in replacements.items():
   if value.endswith(suffix):value=str(replacement);break
 newlink.append(value)
newlink[-1]=str(e/'image.elf');newlink[-2:-2]=[str(ret),str(assembly),'-Wl,--undefined=a733_npu_application_artifact_hashes']
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
