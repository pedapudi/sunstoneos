#!/usr/bin/env python3
"""Build finite USB client on the frozen returning platform and real providers."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import shutil
import subprocess
import sys

sys.dont_write_bytecode=True;sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'tools'))
from returning_build import image_object_closures,object_closure,require_unchanged_headers

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--evidence',type=Path,required=True)
p.add_argument('--baseline',type=Path,default=Path.home()/'.cache/sunstoneos/evidence/pmic-provider-probe-20261018')
p.add_argument('--transport',type=Path,default=Path.home()/'.cache/sunstoneos/evidence/gpu-transport-only-20261018')
# Per-object compiler dependencies of the accepted TinyUSB objects.
p.add_argument('--transport-records',type=Path,default=Path.home()/'.cache/sunstoneos/evidence/gpu-returning-integration-20261018/frozen/image-objects.json')
a=p.parse_args();r=Path(__file__).resolve().parents[2];e=a.evidence.resolve();base=a.baseline.resolve();transport=a.transport.resolve()
b=e/'build';o=b/'objects';o.mkdir(parents=True,exist_ok=True)
def digest(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def save(name,value):(e/name).write_text(json.dumps(value,indent=2)+'\n')
shutil.copytree(base/'snapshot',e/'baseline-snapshot',dirs_exist_ok=True)
shutil.copytree(base/'host',e/'host',dirs_exist_ok=True)
accepted=json.loads((base/'input-identities.json').read_text())
manifest=json.loads((e/'baseline-snapshot/manifest.json').read_text())
for item in list(manifest['sources'].values())+manifest['artifacts']:
 assert digest(e/'baseline-snapshot'/item['snapshot'])==item['sha256'],item['snapshot']
old=json.loads((base/'commands.json').read_text());flags=old[0][:old[0].index('-MMD')]
flags=[x.replace('/home/sunil/git/sunstoneos-worktrees/pmic-provider-probe-20261018',str(r)) for x in flags]
tiny=Path.home()/'.cache/sunstoneos/sources/tinyusb-d668b0abb7219b9ce324fa88d46604bb9e869973/src'
flags += ['-DSUNSTONE_A733_USB=1',f'-I{r}/platform/a733-storage-update',f'-I{r}/drivers/usb',f'-I{tiny}']
config_pin=Path.home()/'.cache/sunstoneos/evidence/gpu-software-failure-20261018/snapshot/inputs/188-tusb_config.h'
assert digest(r/'platform/a733-storage-update/tusb_config.h')==digest(config_pin)
commands=[];objects=[]
# Recompile the layout-dependent provider for this platform's FreeRTOS config.
for i,source in enumerate([r/'apps/usb-returning/main.c',r/'platform/a733/native_usb.c',r/'drivers/usb/a733_usb_platform.c',r/'drivers/usb/a733_usb_platform_native.c']):
 obj=o/f'new-{i}.obj';objects.append(obj)
 commands.append(flags+['-MD','-MF',str(obj)+'.d','-c',str(source),'-o',str(obj)])
reused=[]
items=json.loads((base/'reused-objects.json').read_text())
for pin in accepted['new_objects']:
 if Path(pin['path']).name!='00-pmic-provider-probe.obj':items.append(pin)
# TinyUSB uses OPT_OS_NONE. Its objects consume no FreeRTOS layouts or APIs.
usb=json.loads((transport/'reused-objects.json').read_text())
for item in usb:
 if any(Path(item['path']).name.endswith(name) for name in ('dcd_sunxi_musb.c.obj','tusb.c.obj','tusb_fifo.c.obj','usbd.c.obj','vendor_device.c.obj')):
  items.append(item)
assert len(items)==31,len(items)
# Accepted objects keep the layouts of the headers they were compiled with.
recorded=image_object_closures(a.transport_records,'apps/gpu-render/main.c')
headers=require_unchanged_headers([('accepted' in x and recorded.get(x['sha256'])) or object_closure(x['path'],x.get('original')) for x in items],old[-2][0])
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
save('input-identities.json',{'repository_revision_at_build':subprocess.check_output(['git','rev-parse','HEAD'],cwd=r,text=True).strip(),'new_compiler_inputs':closure,'baseline_input_records_sha256':digest(base/'input-identities.json'),'transport_input_records_sha256':digest(transport/'input-identities.json'),'toolchain':tools,'reused_object_headers':headers,'elf_sha256':digest(b/'probe.elf'),'binary_sha256':digest(e/'probe.bin'),'physical_access':False})
for root,name in [(base,'baseline'),(transport,'transport')]:
 for source in ['input-identities.json','reused-objects.json']:
  shutil.copy2(root/source,e/(name+'-'+source))
for name in ('check.py','prepare.py','README.md'):
 source=r/'apps/usb-returning'/name
 if source.exists():shutil.copy2(source,e/name)
shutil.copy2(r/'checks/storage-read-native.py',e/'layout-helper.py')
print(f'Built bounded USB returning image; {len(reused)} accepted objects and four new units.')
# Freeze the runnable host closure, including the updated kernel observer hook.
host_files=['tools/check-a733-usb-returning.py','tools/check-a733-kernel.py','tools/usb-access.py','tools/check-a733-smp.py']
host_inputs={}
for name in host_files:
 source=r/name;dest=e/'host'/name;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,dest)
 host_inputs[name]=digest(source)
# The existing runner deliberately uses the configured external guard path.
# Record that runtime dependency; do not substitute another implementation.
external=Path.home()/'git/mimus/tools/usb_guard.py';host_inputs[str(external)]=digest(external)
save('host-inputs.json',host_inputs)
for name in ('host-check.py','check.py','prepare.py','README.md'):
 source=r/'apps/usb-returning'/name;shutil.copy2(source,e/name)
# Preserve the reviewed ROM bytes/disassembly and the actual compiled PIO source.
return_sources={}
source_paths=[Path.home()/'.cache/sunstoneos/evidence/rom-copy-20261016-084055'/name for name in ('physical-rom.bin','physical-rom.dis','control-flow.json')]
source_paths += [Path.home()/'.cache/sunstoneos/sources/brom'/name for name in ('ns-brom.bin','ns-brom.dis')]
ancestor=Path.home()/'.cache/sunstoneos/evidence/gpu-software-failure-20261018/snapshot/manifest.json'
for item in json.loads(ancestor.read_text())['inputs']:
 if any(part in item['source']['path'] for part in ('dcd_sunxi_musb.c','musb_def.h','vendor_device.c','tusb_config.h')):
  source=Path(item['frozen']['path']);assert digest(source)==item['frozen']['sha256'];source_paths.append(source)
for i,source in enumerate(source_paths):
 dest=e/'return-sources'/f'{i:02d}-{source.name}';dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,dest)
 return_sources[str(source)]={'snapshot':str(dest.relative_to(e)),'sha256':digest(source)}
save('return-source-identities.json',return_sources)
