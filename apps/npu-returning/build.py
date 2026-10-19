#!/usr/bin/env python3
"""Build a finite NPU supply client with the accepted returning platform objects."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import shutil
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--evidence',type=Path,required=True)
p.add_argument('--baseline',type=Path,default=Path.home()/'.cache/sunstoneos/evidence/pmic-provider-probe-20261018')
a=p.parse_args();r=Path(__file__).resolve().parents[2];e=a.evidence.resolve();base=a.baseline.resolve()
b=e/'build';o=b/'objects';o.mkdir(parents=True,exist_ok=True)
def digest(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def save(name,value):(e/name).write_text(json.dumps(value,indent=2)+'\n')
# Preserve the baseline source records, unchanged host tools and runtime-hashed
# assembly inputs. These files provide provenance, not physical admission.
shutil.copytree(base/'snapshot',e/'baseline-snapshot',dirs_exist_ok=True)
shutil.copytree(base/'host',e/'host',dirs_exist_ok=True)
accepted=json.loads((base/'input-identities.json').read_text())
manifest=json.loads((e/'baseline-snapshot/manifest.json').read_text())
for item in list(manifest['sources'].values())+manifest['artifacts']:
 assert digest(e/'baseline-snapshot'/item['snapshot'])==item['sha256'],item['snapshot']
old=json.loads((base/'commands.json').read_text());flags=old[0][:old[0].index('-MMD')]
flags=[x.replace('/home/sunil/git/sunstoneos-worktrees/pmic-provider-probe-20261018',str(r)) for x in flags]
commands=[];objects=[]
for i,source in enumerate([r/'apps/npu-returning/main.c']):
 obj=o/f'new-{i}.obj'
 objects.append(obj)
 commands.append(flags+['-MD','-MF',str(obj)+'.d','-c',str(source),'-o',str(obj)])
reused=[]
items=json.loads((base/'reused-objects.json').read_text())
for name in [f'{i:02d}-{name}.obj' for i,name in enumerate(['unused','shared_locks','counter_clock','a733_clock_leases','a733_rail_leases','a733_pmic','a733_pmic_native','a733_pmic_consumer','a733_pins','a733_pin_tables','a733_pins_native','a733_gpu_resources','a733_gpu_resources_native','tasks','queue']) if i]:
 source=base/'build/objects'/name
 pin=next(x for x in accepted['new_objects'] if x['path']==str(source))
 items.append({'path':str(source),'sha256':pin['sha256'],'original':str(source)})
for i,item in enumerate(items):
 source=Path(item['path']);assert digest(source)==item['sha256'],source
 dest=o/f'reused-{i}.obj';shutil.copy2(source,dest);objects.append(dest)
 reused.append({'original':str(source),'path':str(dest),'sha256':digest(dest)})
link=[old[-2][0],'--target=aarch64-none-elf',old[-2][2],'-nostdlib','-static','-Wl,--gc-sections',f'-Wl,-T,{r}/platform/a733/link.ld',f'-Wl,-Map,{b}/probe.map',*map(str,objects),'-o',str(b/'probe.elf')]
commands += [link,[old[-1][0],'-O','binary',str(b/'probe.elf'),str(e/'probe.bin')]]
save('commands.json',commands);save('reused-objects.json',reused)
for i,command in enumerate(commands):
 with (e/f'build-{i}.log').open('w') as log:
  subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True)
# Record the exact compiler dependency closure for the finite client.
inputs=set([r/'platform/a733/link.ld',Path(__file__).resolve()])
for dep in o.glob('new-*.obj.d'):
 text=dep.read_text().replace('\\\n',' ');inputs.update(Path(x) for x in shlex.split(text.split(':',1)[1]))
closure={}
for i,source in enumerate(sorted(inputs)):
 dest=e/'compiled-inputs'/str(i)/source.name;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,dest)
 closure[str(source)]={'sha256':digest(source),'snapshot':str(dest.relative_to(e))}
tools={}
for command in commands:
 for value in command[:3]:
  path=Path(value.split('=',1)[-1])
  if path.is_absolute() and path.is_file() and (value.startswith('--ld-path=') or value==command[0]):
   assert digest(path)==accepted['tools'][str(path)]['sha256'],path
   tools[str(path)]={'sha256':digest(path),'resolved':str(path.resolve())}
save('input-identities.json',{'repository_revision_at_build':subprocess.check_output(['git','rev-parse','HEAD'],cwd=r,text=True).strip(),'new_compiler_inputs':closure,'baseline_input_records_sha256':digest(base/'input-identities.json'),'baseline_snapshot_manifest_sha256':digest(base/'snapshot/manifest.json'),'toolchain':tools,'elf_sha256':digest(b/'probe.elf'),'binary_sha256':digest(e/'probe.bin'),'physical_access':False})
shutil.copy2(base/'input-identities.json',e/'baseline-input-identities.json')
print(f'Built NPU supply returning image; {len(reused)} unchanged objects, one NPU supply client.')

for name in ('check.py','README.md'):
 source=r/'apps/npu-returning'/name
 if source.exists():shutil.copy2(source,e/name)
shutil.copy2(r/'checks/storage-read-native.py',e/'layout-helper.py')
