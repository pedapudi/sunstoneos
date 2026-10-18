#!/usr/bin/env python3
"""Build the root-configured mimus image; execute only new supervised cases."""
import argparse,hashlib,importlib.util,json,shlex,shutil,subprocess,sys
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_INTR
from unicorn.arm64_const import UC_ARM64_REG_SP,UC_ARM64_REG_X0
ROOT=Path(__file__).resolve().parents[1]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser(description=__doc__)
 for name in ('output','freertos','tinyusb','clang','clangxx','lld','objcopy','mimus','newlib','newlib-smp'):p.add_argument('--'+name,type=Path,required=True)
 p.add_argument('--resume',action='store_true');a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=a.resume)
 source=out/'source';build=out/'build';steps_path=out/'steps.json';steps=json.loads(steps_path.read_text()) if steps_path.exists() else {}
 if not source.exists():
  for name in subprocess.check_output(['git','ls-files','--cached','--others','--exclude-standard'],cwd=ROOT,text=True).splitlines():
   path=ROOT/name
   if path.is_file():target=source/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(path,target)
  (out/'inputs.json').write_text(json.dumps({str(p.relative_to(source)):sha(p) for p in source.rglob('*') if p.is_file()},indent=2)+'\n')
 for name,value in json.loads((out/'inputs.json').read_text()).items():assert sha(source/name)==value
 tools={str(p.resolve()):sha(p.resolve()) for p in (a.clang,a.clangxx,a.lld,a.objcopy,Path(sys.executable))}
 if (out/'tools.json').exists():assert json.loads((out/'tools.json').read_text())==tools
 else:(out/'tools.json').write_text(json.dumps(tools,indent=2)+'\n')
 def run(label,argv,cwd=source):
  argv=list(map(str,argv))
  if steps.get(label,{}).get('exit')==0:assert steps[label]['argv']==argv;return
  r=subprocess.run(argv,cwd=cwd,capture_output=True,text=True,timeout=180)
  (out/(label+'.log')).write_text(r.stdout+r.stderr);steps[label]=dict(argv=argv,exit=r.returncode)
  steps_path.write_text(json.dumps(steps,indent=2)+'\n');r.check_returncode()
 toolchain=out/'toolchain.cmake'
 if not toolchain.exists():toolchain.write_text(f'set(SUNSTONE_LLD "{a.lld}" CACHE FILEPATH "")\ninclude("{source}/cmake/aarch64-clang.cmake")\nset(CMAKE_C_COMPILER "{a.clang.resolve()}")\nset(CMAKE_ASM_COMPILER "{a.clang.resolve()}")\nset(CMAKE_CXX_COMPILER "{a.clangxx}")\n')
 run('configure',['cmake','-S',source,'-B',build,f'-DCMAKE_TOOLCHAIN_FILE={toolchain}',f'-DFREERTOS_DIR={a.freertos}',f'-DTINYUSB_DIR={a.tinyusb}',
  f'-DMIMUS_DIR={a.mimus}',f'-DNEWLIB_ROOT={a.newlib}',f'-DNEWLIB_SMP_ROOT={a.newlib_smp}','-DSUNSTONE_MIMUS_SUPERVISED=ON','-DCMAKE_EXPORT_COMPILE_COMMANDS=ON'])
 run('image-build',['cmake','--build',build,'--target','sunstone-a733-mimus-native','-j4'])
 image=build/'sunstone-a733-mimus-native.elf';commands=json.loads((build/'compile_commands.json').read_text())
 # Compile changed default-mode units without executing existing watchdog cases.
 for file in ('native.c','admission.c','main.cc','parallel_runtime.cc','replay_runtime.cc'):
  unit=next(u for u in commands if u['file']==str(source/'apps/mimus'/file))
  flags=shlex.split(unit['command']);flags=flags[:flags.index('-o')]
  run('default-compile-'+file,flags+['-USUNSTONE_MIMUS_SUPERVISED','-MD','-MF',out/(file+'.d'),'-c',source/'apps/mimus'/file,'-o',out/(file+'.o')])
 manifest=out/'image-manifest.json';manifest.write_text(json.dumps(dict(status='PASS',physical=False,image_sha256=sha(image),
  build_id=(build/'mimus-native/build-id.txt').read_text().strip()),indent=2)+'\n')
 host=[sys.executable,source/'checks/mimus-supervised-host.py','--image',image,'--manifest',manifest,'--evidence',out/'host',
  '--clang',a.clang,'--lld',a.lld,'--objcopy',a.objcopy]
 # All setup signatures, wire values, ELF geometry, asset/reference identities
 # and lifecycle prerequisites are checked together before instruction cases.
 run('host-setup',host+['--setup-only'])
 run('host-cases',host)
 run('native-new-boundaries',[sys.executable,'-B',source/'checks/mimus-supervised-native.py',
  '--image',image,'--bundle',out/'host/bundle','--evidence',out/'native'])
 dependencies={}
 for dep in list(build.rglob('*.obj.d'))+list(build.rglob('*.o.d'))+list(out.glob('*.d')):
  for name in shlex.split(dep.read_text().replace('\\\n',' ').split(':',1)[1]):
   path=Path(name)
   if path.is_file():dependencies[str(path.resolve())]=sha(path)
 for unit in commands:
  if '/CMakeFiles/sunstone-a733-mimus-native.dir/' in unit['command'] or '/CMakeFiles/mimus_asr_smp.dir/' in unit['command']:
   path=Path(unit['file']);dependencies[str(path.resolve())]=sha(path)
 for link in (build/'CMakeFiles/sunstone-a733-mimus-native.dir/link.txt',build/'CMakeFiles/mimus_asr_smp.dir/link.txt'):
  for word in shlex.split(link.read_text()):
   path=Path(word)
   if path.is_absolute() and path.is_file() and path.suffix=='.a':dependencies[str(path.resolve())]=sha(path)
 for name in dependencies:
  p=Path(name)
  if not p.is_relative_to(out):target=out/'compiled-inputs'/str(p).lstrip('/');target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,target)
 (out/'compiled-dependencies.json').write_text(json.dumps(dependencies,indent=2)+'\n')
 result=json.loads(manifest.read_text());result.update(steps=steps,host_results=str(out/'host/results.json'),native_results=str(out/'native/result.json'),physical=False)
 (out/'result.json').write_text(json.dumps(result,indent=2)+'\n');print('PASS root-configured supervised mimus and new focused boundaries')
if __name__=='__main__':main()
