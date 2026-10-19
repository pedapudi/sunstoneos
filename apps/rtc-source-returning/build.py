#!/usr/bin/env python3
"""Build source-only or alarm RTC operation with frozen returning objects."""
import argparse,hashlib,json,shlex,shutil,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--operation',choices=('source','alarm'),default='source');p.add_argument('--evidence',type=Path,required=True)
p.add_argument('--baseline',type=Path,default=Path.home()/'.cache/sunstoneos/evidence/pmic-provider-probe-20261018')
a=p.parse_args();r=Path(__file__).resolve().parents[2];e=a.evidence.resolve();base=a.baseline.resolve();app=r/f'apps/rtc-{a.operation}-returning';b=e/'build';o=b/'objects';o.mkdir(parents=True,exist_ok=True)
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def save(n,v):(e/n).write_text(json.dumps(v,indent=2)+'\n')
accepted=json.loads((base/'input-identities.json').read_text());old=json.loads((base/'commands.json').read_text())
shutil.copytree(base/'snapshot',e/'baseline-snapshot',dirs_exist_ok=True);shutil.copytree(base/'host',e/'host',dirs_exist_ok=True)
m=json.loads((e/'baseline-snapshot/manifest.json').read_text())
for item in list(m['sources'].values())+m['artifacts']:assert sha(e/'baseline-snapshot'/item['snapshot'])==item['sha256']
flags=[x.replace('/home/sunil/git/sunstoneos-worktrees/pmic-provider-probe-20261018',str(r)) for x in old[0][:old[0].index('-MMD')]]
flags+=['-iquote',str(app)]
sources=[app/'main.c',app/'monitor.c',r/'drivers/rtc/a733_rtc.c',r/('drivers/rtc/a733_rtc_initialize.c' if a.operation=='source' else 'drivers/gpu/gicv3_spi.c')]
commands=[];objects=[]
for i,source in enumerate(sources):
 obj=o/f'new-{i}.obj';objects.append(obj);commands.append(flags+['-MD','-MF',str(obj)+'.d','-c',str(source),'-o',str(obj)])
items=json.loads((base/'reused-objects.json').read_text())
for name in ('02-counter_clock.obj','13-tasks.obj','14-queue.obj'):
 path=base/'build/objects'/name;pin=next(x for x in accepted['new_objects'] if x['path']==str(path));items.append(pin)
reused=[]
for i,item in enumerate(items):
 path=Path(item['path']);assert sha(path)==item['sha256'];dest=o/f'reused-{i}.obj';shutil.copy2(path,dest);objects.append(dest)
 reused.append({'original':str(path),'path':str(dest),'sha256':sha(dest)})
wraps=['monitor_init']+(['monitor_exception','vApplicationFPUSafeIRQHandler'] if a.operation=='alarm' else [])
link=[old[-2][0],'--target=aarch64-none-elf',old[-2][2],'-nostdlib','-static','-Wl,--gc-sections',*[f'-Wl,--wrap={x}' for x in wraps],f'-Wl,-T,{r}/platform/a733/link.ld',f'-Wl,-Map,{b}/probe.map',*map(str,objects),'-o',str(b/'probe.elf')]
commands += [link,[old[-1][0],'-O','binary',str(b/'probe.elf'),str(e/'probe.bin')]]
save('commands.json',commands);save('reused-objects.json',reused)
for i,command in enumerate(commands):
 with (e/f'build-{i}.log').open('w') as log:subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True)
inputs={r/'platform/a733/link.ld',Path(__file__).resolve()}
for dep in o.glob('new-*.obj.d'):inputs.update(Path(x) for x in shlex.split(dep.read_text().replace('\\\n',' ').split(':',1)[1]))
closure={}
for i,source in enumerate(sorted(inputs)):
 dest=e/'compiled-inputs'/str(i)/source.name;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,dest);closure[str(source)]={'sha256':sha(source),'snapshot':str(dest.relative_to(e))}
tools={}
for command in commands:
 for value in command[:3]:
  path=Path(value.split('=',1)[-1])
  if path.is_absolute() and path.is_file() and (value==command[0] or value.startswith('--ld-path=')):
   assert sha(path)==accepted['tools'][str(path)]['sha256'];tools[str(path)]={'sha256':sha(path),'resolved':str(path.resolve())}
save('input-identities.json',{'operation':a.operation,'repository_revision_at_build':subprocess.check_output(['git','rev-parse','HEAD'],cwd=r,text=True).strip(),'compiler_inputs':closure,'toolchain':tools,'baseline_inputs_sha256':sha(base/'input-identities.json'),'elf_sha256':sha(b/'probe.elf'),'binary_sha256':sha(e/'probe.bin'),'wraps':wraps,'physical_access':False})
shutil.copy2(base/'input-identities.json',e/'baseline-input-identities.json');shutil.copy2(r/'checks/storage-read-native.py',e/'layout-helper.py')
print(f'Built {a.operation} returning RTC image; four target units,15 accepted unchanged objects.')
