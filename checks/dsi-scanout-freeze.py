#!/usr/bin/env python3
"""Freeze the selected DSI image's original Make recipes and linked dependencies."""
import argparse,hashlib,importlib.util,json,re,shlex,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser(description=__doc__)
 for name in ('build','evidence','output'):p.add_argument('--'+name,type=Path,required=True)
 a=p.parse_args();build=a.build.resolve();target='sunstone-a733-dsi-scanout';link=build/f'CMakeFiles/{target}.dir/link.txt';objects=[(build/n).resolve() for n in shlex.split(link.read_text()) if n.endswith(('.obj','.o'))];commands=[];deps={};records={};metadata={link};cache={}
 for obj in objects:
  parent=next(q for q in obj.parents if (q/'build.make').is_file())
  if parent not in cache:
   make=parent/'build.make';flags=parent/'flags.make';variables=dict(re.findall(r'^(\w+) = (.*)$',flags.read_text(),re.M));recipes=[]
   for line in make.read_text().splitlines():
    if not line.startswith('\t') or ' -c ' not in line or ' -o ' not in line:continue
    raw=line.strip();expanded=re.sub(r'\$\((\w+)\)',lambda m:variables[m[1]],raw);args=shlex.split(expanded);directory=build
    if args[0]=='cd':directory=Path(args[1]);assert args[2]=='&&';args=args[3:]
    destination=(directory/args[args.index('-o')+1]).resolve();source=(directory/args[args.index('-c')+1]).resolve()
    recipes.append((destination,dict(directory=str(directory),file=str(source),output=str(destination),arguments=args,original_recipe=raw,recipe_record=str(make),flags_record=str(flags))))
   cache[parent]=dict(recipes);metadata|={make,flags}
   for name in ('compiler_depend.make','compiler_depend.internal','depend.internal','DependInfo.cmake'):
    if (parent/name).is_file():metadata.add(parent/name)
  command=cache[parent][obj];source=Path(command['file']);dep=Path(str(obj)+'.d');paths=[]
  if dep.is_file():paths=[(Path(command['directory'])/n).resolve() for n in shlex.split(dep.read_text().replace('\\\n',' ').split(':',1)[1])]
  else:
   dep=parent/'depend.internal';current=None
   for line in dep.read_text().splitlines():
    if not line or line.startswith('#'):continue
    if not line.startswith(' '):current=(build/line).resolve()
    elif current==obj:paths.append(Path(line.strip()))
   assert source.suffix=='.S' and paths,(obj,dep)
  assert source in paths,(source,dep)
  if source.suffix=='.S':paths += [Path(n) for n in re.findall(r'^\s*\.incbin\s+"([^"]+)"',source.read_text(),re.M)]
  for path in paths:assert path.is_file(),path;deps[str(path)]=sha(path)
  assert obj.is_file();commands.append(command);records[str(obj)]=dict(sha256=sha(obj),source=str(source),dependency_record=str(dep),record_sha256=sha(dep));metadata.add(dep)
 a.evidence.mkdir(exist_ok=True);closure=a.evidence/'compiled-closure.json'
 closure.write_text(json.dumps(dict(target=target,execution_profile='dsi-single-core-supervised-v1',final_elf=sha(build/(target+'.elf')),dependencies=deps,objects=records,commands=commands,command_authority='Original CMake build.make compiler recipes expanded with their flags.make variables, plus original link.txt; no rebuild or generated CMake compilation database',feature_scope='Single-core SRE GUEST1, supervised, extended memory, DSI application. No USB host IRQ/USB3/SMP_DIAGNOSTIC capability.'),indent=2)+'\n')
 sources=set(ROOT/'apps/dsi-scanout'/name.name for name in (ROOT/'apps/dsi-scanout').iterdir() if name.is_file())|set(ROOT/'checks'/name for name in ('dsi-scanout.py','dsi-scanout-profile.c','dsi-scanout-native.c','dsi-scanout-linked.py','dsi-scanout-launch.py','dsi-scanout-host.py','dsi-scanout-freeze.py'))|{ROOT/'cmake/a733-dsi-scanout.cmake',ROOT/'docs/display-scanout.md',ROOT/'tools/evidence-snapshot.py'}
 sources|={ROOT/n for n in ('checks/display-scanout.c','checks/display-clock-handoff.c','checks/a733-pmic.c','checks/a733-smp-supervised-launch.py','checks/usb-host-hid-native.py','apps/board/prepare.py','tools/check-a733-smp.py','tools/check-a733-smp-supervised.py','tools/check-a733-smp-observe.py','tools/usb-access.py')}
 host=json.loads((a.evidence/'application/compiled-host-inputs.json').read_text())['inputs']
 for name,digest in host.items():assert sha(Path(name))==digest,name
 artifacts={Path(n) for n in host}|{Path(n) for n in deps}|set(objects)|metadata|{build/(target+'.elf'),build/'dsi-scanout/link.ld',build/'dsi-scanout/image.map',build/'CMakeCache.txt'}
 artifacts|={p for p in a.evidence.rglob('*') if p.is_file() and a.output not in p.parents and not any(part in ('frozen','final-frozen') for part in p.relative_to(a.evidence).parts)}
 command=[sys.executable,str(ROOT/'tools/evidence-snapshot.py'),'--repository',str(ROOT),'--output',str(a.output),'--note','Standalone DSI selected native image and actual original linked-object/compiler-dependency closure. Private app/launcher/seal/host evidence and root static linked-layout review only. Explicit MODEL_ONLY facts; no shared startup/GIC/device execution or physical panel qualification. Earlier failed fixture/build logs retained.']
 for path in sorted(sources):command+=['--source',str(path.relative_to(ROOT))]
 for path in sorted(artifacts):command+=['--artifact',str(path)]
 for path in (Path('/usr/bin/clang-21'),Path('/home/sunil/.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld'),Path(sys.executable)):command+=['--tool',str(path)]
 subprocess.run(command,check=True)
 for name,digest in deps.items():assert sha(Path(name))==digest,name
 print(json.dumps(dict(objects=len(objects),compiled_inputs=len(deps),sources=len(sources),artifacts=len(artifacts),manifest=str(a.output/'manifest.json'),closure=str(closure))))
if __name__=='__main__':main()
