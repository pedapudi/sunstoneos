#!/usr/bin/env python3
"""Link a development-only finite GPU adapter with accepted driver/startup objects."""
import argparse, hashlib, json, shlex, shutil, subprocess, sys
from pathlib import Path
sys.dont_write_bytecode=True;sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'tools'))
from returning_build import image_object_closures,require_unchanged_headers
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--evidence',type=Path,required=True)
p.add_argument('--baseline',type=Path,default=Path.home()/'.cache/sunstoneos/evidence/gpu-returning-integration-20261018/frozen')
a=p.parse_args();r=Path(__file__).resolve().parents[2];e=a.evidence.resolve();base=a.baseline.resolve();b=e/'build';o=b/'objects';o.mkdir(parents=True,exist_ok=True)
def digest(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def save(name,value):(e/name).write_text(json.dumps(value,indent=2)+'\n')
manifest=json.loads((base/'manifest.json').read_text());assert digest(base/'image.elf')==manifest['final_elf']
assert manifest['target']=='sunstone-a733-gpu-compute-single-core'
accepted=json.loads((base/'image-objects.json').read_text());bindings=json.loads((base/'source-bindings.json').read_text());old=json.loads((base/'image-commands.json').read_text())
for entry in bindings:assert digest(entry['frozen'])==entry['sha256'],entry['frozen']
main=next(cmd for cmd in old if '-c' in cmd and cmd[cmd.index('-c')+1].endswith('/apps/gpu-render/main.c'))
oldroot=str(Path(main[main.index('-c')+1]).parents[2]);flags=main[:main.index('-o')]
flags=[x.replace(oldroot,str(r)) for x in flags];flags+=['-DSUNSTONE_GPU_FINITE_RETURN=1']
identity={'baseline_image':manifest['final_elf'],'selection':'SUNSTONE_GPU_FINITE_RETURN=1','new_sources':{n:digest(r/n) for n in ['apps/gpu-render/main.c','apps/gpu-finite/finite.c','apps/gpu-finite/finite.h']}}
id=hashlib.sha256(json.dumps(identity,sort_keys=True).encode()).hexdigest();(b/'build-id.c').write_text('const unsigned char a733_smp_build_id[32]={'+','.join('0x'+id[n:n+2] for n in range(0,64,2))+'};\n')
# Retained objects keep the layouts of the headers they were compiled with.
recorded=image_object_closures(base/'image-objects.json','apps/gpu-render/main.c')
headers=require_unchanged_headers([recorded[x['sha256']] for x in accepted if not x['source'].endswith(('/apps/gpu-render/main.c','/build-id.c'))],main[0])
objects={};reused=[];commands=[]
for i,entry in enumerate(accepted):
 if entry['source'].endswith(('/apps/gpu-render/main.c','/build-id.c')):continue
 source=Path(entry['object']);assert digest(source)==entry['sha256'];dest=o/f'reused-{i:03d}-{source.name}';shutil.copy2(source,dest);objects[str(source)]=str(dest)
 reused.append({'accepted':entry,'object':str(dest),'sha256':digest(dest)})
for name,source in [('main',r/'apps/gpu-render/main.c'),('finite',r/'apps/gpu-finite/finite.c'),('build-id',b/'build-id.c')]:
 obj=o/(name+'.obj');cmd=flags+['-MMD','-MF',str(obj)+'.d','-o',str(obj),'-c',str(source)];commands.append(cmd)
 if name!='finite':
  original=next(x['object'] for x in accepted if x['source'].endswith('/apps/gpu-render/main.c' if name=='main' else '/build-id.c'));objects[original]=str(obj)
link=[]
for x in old[-1]:
 if x.endswith('.obj') or x.endswith('.o'):link.append(objects[x])
 elif x.startswith('-Wl,-T,'):
  shutil.copy2(base/'link.ld',b/'link.ld');link.append('-Wl,-T,'+str(b/'link.ld'))
 elif x.startswith('-Wl,-Map,'):link.append('-Wl,-Map,'+str(b/'adapter.map'))
 else:link.append(x)
link.insert(link.index('-o'),str(o/'finite.obj'));link[link.index('-o')+1]=str(b/'adapter.elf');commands.append(link)
save('commands.json',commands)
for i,cmd in enumerate(commands):
 with (e/f'build-{i}.log').open('w') as f:subprocess.run(cmd,cwd=r,stdout=f,stderr=subprocess.STDOUT,check=True)
# Preserve all accepted source bindings, including prior monitor/runtime source.
inputs=e/'accepted-inputs';inputs.mkdir(exist_ok=True);frozen=[]
for entry in bindings:
 source=Path(entry['frozen']);dest=inputs/source.name
 if not dest.exists():shutil.copy2(source,dest)
 copy=dict(entry);copy['frozen']=str(dest);frozen.append(copy)
save('accepted-source-bindings.json',frozen);save('retained-objects.json',reused)
closure={}
for dep in o.glob('*.obj.d'):
 for name in shlex.split(dep.read_text().replace('\\\n',' '))[1:]:
  source=Path(name)
  if not source.is_absolute():source=r/source
  closure[str(source)]={'sha256':digest(source)}
for source in [Path(__file__).resolve(),b/'link.ld']:closure[str(source)]={'sha256':digest(source)}
for i,(name,entry) in enumerate(sorted(closure.items())):
 source=Path(name);dest=e/'compiled-inputs'/str(i)/source.name;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,dest);entry['frozen']=str(dest)
save('input-identities.json',{'development_only':True,'repository':subprocess.check_output(['git','rev-parse','HEAD'],cwd=r,text=True).strip(),'identity':identity,'build_id':id,'new_compiler_inputs':closure,'reused_object_headers':headers,'tools':{cmd[0]:digest(cmd[0]) for cmd in commands},'elf_sha256':digest(b/'adapter.elf'),'baseline_manifest_sha256':digest(base/'manifest.json')})
save('manifest.json',{'target':'gpu-finite-adapter-development','execution_profile':'gpu-finite-adapter-development-v1','workload':'fixed-arithmetic-repeat-finite-client','development_only':True,'physical_eligible':False,'build_id':id,'final_elf':digest(b/'adapter.elf')})
print('Linked development-only finite GPU adapter:',digest(b/'adapter.elf'))
