#!/usr/bin/env python3
"""Compose GPU-local returning monitor around the accepted finite device adapter."""
import argparse,hashlib,json,shutil,subprocess,shlex
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--evidence',type=Path,required=True)
p.add_argument('--baseline',type=Path,default=Path.home()/'.cache/sunstoneos/evidence/gpu-finite-adapter-20261018/layout-correction')
a=p.parse_args();r=Path(__file__).resolve().parents[2];e=a.evidence.resolve();base=a.baseline.resolve();b=e/'build';o=b/'objects';o.mkdir(parents=True,exist_ok=True)
def digest(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def save(name,value):(e/name).write_text(json.dumps(value,indent=2)+'\n')
identity=json.loads((base/'input-identities.json').read_text());assert digest(base/'build/adapter.elf')==identity['elf_sha256']
old=json.loads((base/'commands.json').read_text());bindings=json.loads((base/'accepted-source-bindings.json').read_text());retained=json.loads((base/'retained-objects.json').read_text())
monitor=next(x for x in bindings if x['original_dependency'].endswith('/apps/storage-read-single-core/monitor.c'))
assert monitor['sha256']=='b7510ffd067298d4289201d9dddd9987d2775efe13460aaac5124921814ae22e';assert digest(monitor['frozen'])==monitor['sha256']
subprocess.run(['patch','--silent','--output',str(b/'gpu-return-monitor.c'),monitor['frozen'],str(r/'apps/gpu-finite/monitor.patch')],check=True)
subprocess.run(['patch','--silent','--output',str(b/'link.ld'),str(base/'build/link.ld'),str(r/'apps/gpu-finite/link.patch')],check=True)
main=old[0];flags=main[:main.index('-MMD')];oldroot=str(Path(main[main.index('-c')+1]).parents[2]);flags=[x.replace(oldroot,str(r)) for x in flags];flags.insert(2,'-I'+str(r/'apps/gpu-finite'))
platform=next(x for x in bindings if x['original_dependency'].endswith('/gpu-single-core/platform.c'))
assert digest(platform['frozen'])==platform['sha256']
platform_text=Path(platform['frozen']).read_text()
assert platform_text.count('_Noreturn void platform_exit(')==1
(b/'gpu-return-platform.c').write_text(platform_text.replace('_Noreturn void platform_exit(', '_Noreturn void gpu_previous_platform_exit('))
platform_object=next(x['object'] for x in retained if x['accepted']['source'].endswith('/gpu-single-core/platform.c'))
new_sources=[r/'apps/gpu-finite/exit.c',b/'gpu-return-platform.c',r/'apps/gpu-finite/finite.c',r/'apps/gpu-finite/return.c',r/'apps/gpu-finite/return.S',r/'apps/gpu-finite/return.h',r/'apps/gpu-finite/monitor.patch',r/'apps/gpu-finite/link.patch',b/'gpu-return-monitor.c',b/'link.ld']
newid=hashlib.sha256(json.dumps({'baseline':identity['elf_sha256'],'sources':{str(p):digest(p) for p in new_sources}},sort_keys=True).encode()).hexdigest()
(b/'build-id.c').write_text('const unsigned char a733_smp_build_id[32]={'+','.join('0x'+newid[i:i+2] for i in range(0,64,2))+'};\n')
monitor_object=next(x['object'] for x in retained if x['accepted']['source'].endswith('/apps/storage-read-single-core/monitor.c'))
replacements={};kept=[]
for i,value in enumerate(old[-1]):
 if not value.endswith(('.obj','.o')):continue
 if value in [platform_object,monitor_object,str(base/'build/objects/finite.obj'),str(base/'build/objects/build-id.obj')]:continue
 source=Path(value);dest=o/source.name;shutil.copy2(source,dest);replacements[value]=str(dest);kept.append({'original':str(source),'object':str(dest),'sha256':digest(dest)})
commands=[]
for name,source in [('exit.c',r/'apps/gpu-finite/exit.c'),('gpu-return-platform.c',b/'gpu-return-platform.c'),('finite.c',r/'apps/gpu-finite/finite.c'),('return.c',r/'apps/gpu-finite/return.c'),('return.S',r/'apps/gpu-finite/return.S'),('gpu-return-monitor.c',b/'gpu-return-monitor.c'),('build-id.c',b/'build-id.c')]:
 obj=o/(name+'.o');cmd=flags+['-MMD','-MF',str(obj)+'.d','-o',str(obj),'-c',str(source)];commands.append(cmd)
replacements[platform_object]=str(o/'gpu-return-platform.c.o');replacements[monitor_object]=str(o/'gpu-return-monitor.c.o');replacements[str(base/'build/objects/finite.obj')]=str(o/'finite.c.o');replacements[str(base/'build/objects/build-id.obj')]=str(o/'build-id.c.o')
link=[]
for value in old[-1]:
 if value.endswith(('.obj','.o')):link.append(replacements[value])
 elif value.startswith('-Wl,-T,'):link.append('-Wl,-T,'+str(b/'link.ld'))
 elif value.startswith('-Wl,-Map,'):link.append('-Wl,-Map,'+str(b/'return.map'))
 else:link.append(value)
link[link.index('-o')+1]=str(b/'return.elf');link[link.index('-o'):link.index('-o')]=[str(o/'exit.c.o'),str(o/'return.c.o'),str(o/'return.S.o')];commands.append(link);save('commands.json',commands)
cache_path=e/'compile-cache.json';cache=json.loads(cache_path.read_text()) if cache_path.exists() else {}
for i,cmd in enumerate(commands):
 obj=Path(cmd[cmd.index('-o')+1]);dep=Path(str(obj)+'.d');key=str(i)
 inputs={}
 if '-c' in cmd and dep.exists():
  for value in shlex.split(dep.read_text().replace('\\\n',' '))[1:]:
   source=Path(value);source=source if source.is_absolute() else r/source;inputs[str(source)]=digest(source)
 prior=cache.get(key,{})
 if inputs and prior.get('command')==cmd and prior.get('inputs')==inputs and obj.exists() and prior.get('sha256')==digest(obj):continue
 with (e/f'build-{i}.log').open('w') as log:subprocess.run(cmd,cwd=r,stdout=log,stderr=subprocess.STDOUT,check=True)
 if '-c' in cmd:
  inputs={value:digest(value) for value in shlex.split(dep.read_text().replace('\\\n',' '))[1:]}
  cache[key]={'command':cmd,'inputs':inputs,'sha256':digest(obj)};cache_path.write_text(json.dumps(cache,indent=2)+'\n')
closure={}
for dep in o.glob('*.o.d'):
 for value in shlex.split(dep.read_text().replace('\\\n',' '))[1:]:
  source=Path(value);source=source if source.is_absolute() else r/source;closure[str(source)]={'sha256':digest(source)}
for source in new_sources+[Path(__file__).resolve()]:closure[str(source)]={'sha256':digest(source)}
for i,(name,entry) in enumerate(sorted(closure.items())):
 source=Path(name);dest=e/'compiled-inputs'/str(i)/source.name;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,dest);entry['frozen']=str(dest)
shutil.copy2(base/'accepted-source-bindings.json',e/'prior-source-bindings.json');shutil.copy2(base/'input-identities.json',e/'prior-input-identities.json');save('retained-objects.json',kept)
save('input-identities.json',{'repository':subprocess.check_output(['git','rev-parse','HEAD'],cwd=r,text=True).strip(),'baseline_elf_sha256':identity['elf_sha256'],'compiled_inputs':closure,'generated_monitor_base':monitor,'retained_objects':kept,'build_id':newid,'elf_sha256':digest(b/'return.elf'),'tools':{commands[0][0]:digest(commands[0][0]),'/home/sunil/.cache/sunstoneos/host/usr/bin/ld.lld-21':digest('/home/sunil/.cache/sunstoneos/host/usr/bin/ld.lld-21')},'physical_access':False})
save('manifest.json',{'target':'sunstone-a733-gpu-finite-returning','execution_profile':'gpu-single-core-returning-v1','workload':'fixed-arithmetic-repeat-return','build_id':newid,'final_elf':digest(b/'return.elf'),'physical_eligible':False,'root_architecture_and_admission_review_required':True})
print('Linked complete GPU-local returning image:',digest(b/'return.elf'))
