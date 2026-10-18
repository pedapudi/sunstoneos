#!/usr/bin/env python3
"""Freeze the exact already-compiled pwm object/header closure."""
import argparse,hashlib,json,shlex
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def freeze(build,out):
 out.mkdir(parents=True,exist_ok=False);image=build/'sunstone-a733-pwm.elf';assert image.is_file()
 inputs=set();objects=[];commands=[]
 for c in json.loads((build/'compile_commands.json').read_text()):
  args=shlex.split(c['command']);obj=Path(c['directory'])/args[args.index('-o')+1]
  if not any(x in str(obj) for x in ('CMakeFiles/sunstone-a733-pwm.dir/','CMakeFiles/a733-board-usb2-host.dir/')):continue
  assert obj.is_file(),obj
  dep=Path(str(obj)+'.d')
  if dep.is_file():names=shlex.split(dep.read_text().replace('\\\n',' ').split(':',1)[1])
  else:
   dep=build/Path(*obj.relative_to(build).parts[:2])/'depend.make'
   assert dep.is_file(),dep
   logical=dep.read_text().replace('\\\n',' ');key=str(obj.relative_to(build))+':'
   lines=[line for line in logical.splitlines() if line.startswith(key)]
   assert len(lines)<=1
   names=shlex.split(lines[0].split(':',1)[1]) if lines else []
   if not lines:
    source=Path(c['file']).read_text();assert '#include' not in source and '.include' not in source,c['file']
  inputs.add(Path(c['file']).resolve());inputs.add(dep)
  inputs.update((Path(c['directory'])/n).resolve() for n in names)
  commands.append(c);objects.append(dict(path=str(obj),sha256=sha(obj),dependency_file=str(dep),dependency_sha256=sha(dep)))
 assert len(objects)>50 and any('apps/pwm/native.c' in c['file'] for c in commands)
 link=build/'CMakeFiles/sunstone-a733-pwm.dir/link.txt';assert link.is_file();inputs.add(link)
 inputs.update([ROOT/'cmake/a733-pwm.cmake',ROOT/'cmake/a733-board.cmake',ROOT/'cmake/a733-storage-maintenance.cmake',ROOT/'CMakeLists.txt',ROOT/'apps/pwm/boot.patch',Path('/usr/lib/llvm-21/bin/clang'),Path('/home/sunil/.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld'),build/'storage-maintenance/link.ld'])
 records=[]
 for p in sorted(inputs):
  data=p.read_bytes();h=hashlib.sha256(data).hexdigest();copy=out/'inputs'/h;copy.parent.mkdir(exist_ok=True)
  if not copy.exists():copy.write_bytes(data)
  records.append(dict(path=str(p),sha256=h,snapshot=str(copy.relative_to(out))))
 (out/'compile-commands.json').write_text(json.dumps(commands,indent=2)+'\n');(out/'link.txt').write_bytes(link.read_bytes());(out/'image.elf').write_bytes(image.read_bytes())
 manifest=dict(target='sunstone-a733-pwm',execution_profile='pwm-service-supervised-v1',physical_access=False,final_elf=sha(image),build_id=(build/'pwm-application/build-id.txt').read_text().strip(),objects=objects,compiled_inputs=records,scope='Exact compiled source/header closure and pinned linker/tool inputs. Hardware behavior unqualified.')
 (out/'image-input-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n');return manifest
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--build',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args();freeze(a.build,a.output)
