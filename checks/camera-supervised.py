#!/usr/bin/env python3
"""Build the root-configured camera image; execute only new supervised cases."""
import argparse,hashlib,importlib.util,json,shlex,shutil,subprocess,sys
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_INTR
from unicorn.arm64_const import UC_ARM64_REG_SP,UC_ARM64_REG_X0
ROOT=Path(__file__).resolve().parents[1]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser(description=__doc__)
 for name in ('output','freertos','tinyusb','clang','lld','objcopy'):p.add_argument('--'+name,type=Path,required=True)
 p.add_argument('--resume',action='store_true');a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=a.resume)
 source=out/'source';build=out/'build';steps_path=out/'steps.json';steps=json.loads(steps_path.read_text()) if steps_path.exists() else {}
 if not source.exists():
  for name in subprocess.check_output(['git','ls-files','--cached','--others','--exclude-standard'],cwd=ROOT,text=True).splitlines():
   path=ROOT/name
   if path.is_file():target=source/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(path,target)
  (out/'inputs.json').write_text(json.dumps({str(p.relative_to(source)):sha(p) for p in source.rglob('*') if p.is_file()},indent=2)+'\n')
 for name,value in json.loads((out/'inputs.json').read_text()).items():assert sha(source/name)==value
 tools={str(p.resolve()):sha(p.resolve()) for p in (a.clang,a.lld,a.objcopy,Path(sys.executable))}
 if (out/'tools.json').exists():assert json.loads((out/'tools.json').read_text())==tools
 else:(out/'tools.json').write_text(json.dumps(tools,indent=2)+'\n')
 def run(label,argv,cwd=source):
  argv=list(map(str,argv))
  if steps.get(label,{}).get('exit')==0:assert steps[label]['argv']==argv;return
  r=subprocess.run(argv,cwd=cwd,capture_output=True,text=True,timeout=60)
  (out/(label+'.log')).write_text(r.stdout+r.stderr);steps[label]=dict(argv=argv,exit=r.returncode)
  steps_path.write_text(json.dumps(steps,indent=2)+'\n');r.check_returncode()
 toolchain=out/'toolchain.cmake'
 if not toolchain.exists():toolchain.write_text(f'set(SUNSTONE_LLD "{a.lld}" CACHE FILEPATH "")\ninclude("{source}/cmake/aarch64-clang.cmake")\nset(CMAKE_C_COMPILER "{a.clang.resolve()}")\nset(CMAKE_ASM_COMPILER "{a.clang.resolve()}")\n')
 run('configure',['cmake','-S',source,'-B',build,f'-DCMAKE_TOOLCHAIN_FILE={toolchain}',f'-DFREERTOS_DIR={a.freertos}',f'-DTINYUSB_DIR={a.tinyusb}',
  '-DSUNSTONE_CAMERA_SUPERVISED=ON','-DCMAKE_EXPORT_COMPILE_COMMANDS=ON'])
 run('image-build',['cmake','--build',build,'--target','sunstone-a733-camera','-j4'])
 image=build/'sunstone-a733-camera.elf';commands=json.loads((build/'compile_commands.json').read_text())
 unit=next(u for u in commands if u['file'].endswith('/apps/camera/main.c'))
 flags=shlex.split(unit['command']);flags=flags[:flags.index('-o')]
 # Compile all new fixtures and default-mode compatibility units before cases.
 fixtures={'admission':['checks/camera-supervised-admission.c','apps/camera/admission.c','drivers/storage/storage_digest.c'],
           'usb':['checks/camera-supervised-usb.c','platform/a733/counter_clock.c']}
 for name,files in fixtures.items():
  run(name+'-build',flags+[source/n for n in files]+[source/'runtime/memory.c','-nostdlib',f'--ld-path={a.lld}',
   '-Wl,-Ttext=0x41000000','-Wl,-e,camera_check_run','-Wl,--gc-sections','-o',out/(name+'.elf')])
 for file in ('main.c','admission.c','capture.c'):
  run('default-compile-'+file,flags+['-USUNSTONE_CAMERA_SUPERVISED','-MD','-MF',out/(file+'.d'),'-c',source/'apps/camera'/file,'-o',out/(file+'.o')])
 common=[a.clang,'-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections','-fsanitize=address,undefined',
  '-DINCLUDE_xSemaphoreGetMutexHolder=1','-DINCLUDE_xTaskGetCurrentTaskHandle=1','-DINCLUDE_xTaskGetSchedulerState=1',
  '-I'+str(source/'include'),'-I'+str(a.freertos/'include'),'-I'+str(a.freertos/'portable/GCC/ARM_AARCH64_SRE')]
 for name in ('a733-camera-capture.c','a733-camera-dma.c'):
  run('compatibility-compile-'+name,common+['-c',source/'checks'/name,'-o',out/(name+'.o')])
 run('proof-build',common+[source/'checks/camera-supervised-proof.c',source/'drivers/camera/a733_camera_platform.c','-o',out/'proof'])
 spec=importlib.util.spec_from_file_location('camera_component_manifest',source/'checks/a733-camera-native.py');component=importlib.util.module_from_spec(spec);spec.loader.exec_module(component)
 run('native-constructor-build',common+['-Wno-unused-function','-DSUNSTONE_CAMERA_MODEL=1',source/'checks/camera-supervised-native.c']+
  [source/n for n in component.SOURCES]+['-Wl,--gc-sections','-o',out/'native-constructor'])
 manifest=out/'image-manifest.json';manifest.write_text(json.dumps(dict(status='PASS',physical=False,image_sha256=sha(image),
  build_id=(build/'camera-application/build-id.txt').read_text().strip()),indent=2)+'\n')
 host=[sys.executable,source/'checks/camera-supervised-host.py','--image',image,'--manifest',manifest,'--evidence',out/'host',
  '--clang',a.clang,'--lld',a.lld,'--objcopy',a.objcopy]
 errors=[]
 for executable in (out/'admission.elf',out/'usb.elf'):
  try:
   with executable.open('rb') as f:
    elf=ELFFile(f);assert elf.elfclass==64 and elf.little_endian and elf['e_machine']=='EM_AARCH64'
    names={s.name for s in elf.get_section_by_name('.symtab').iter_symbols()};assert {'camera_check_run','camera_check_result'}<=names
  except BaseException as e:errors.append(str(executable)+': '+str(e))
 try:run('host-setup',host+['--setup-only'])
 except BaseException as e:errors.append('host setup: '+str(e))
 (out/'preflight.json').write_text(json.dumps(dict(physical=False,errors=errors,image_sha256=sha(image),
  component_fixture_sha256=sha(source/'checks/a733-camera-native.c'),host_fixture_sha256=sha(source/'checks/camera-supervised-host.py')),indent=2)+'\n')
 assert not errors,errors
 run('explicit-recovery-proof',[out/'proof']);run('native-supervised-constructor',[out/'native-constructor'])
 for name,labels in [('admission',['supervised-admission']),('usb',['usb-identity','usb-assert-retention','usb-invalid-interface-retention'])]:
  with (out/(name+'.elf')).open('rb') as f:
   elf=ELFFile(f);segments=[(int(p['p_vaddr']),bytes(p.data()),int(p['p_memsz'])) for p in elf.iter_segments() if p['p_type']=='PT_LOAD']
   symbols={s.name:int(s['st_value']) for s in elf.get_section_by_name('.symtab').iter_symbols()}
  for mode,label in enumerate(labels):
   if steps.get(label,{}).get('exit')==0:continue
   uc=Uc(UC_ARCH_ARM64,UC_MODE_ARM);pages=set()
   for address,data,size in segments:
    for page in range(address&~4095,(address+size+4095)&~4095,4096):
     if page not in pages:uc.mem_map(page,4096);pages.add(page)
    uc.mem_write(address,data)
   uc.mem_map(0x41f00000,0x1000);uc.mem_map(0x50000000,0x10000);uc.reg_write(UC_ARM64_REG_SP,0x5000f000);uc.reg_write(UC_ARM64_REG_X0,mode)
   uc.hook_add(UC_HOOK_INTR,lambda machine,interrupt,unused:machine.emu_stop());uc.emu_start(symbols['camera_check_run'],0,count=1000000)
   result=int.from_bytes(uc.mem_read(symbols['camera_check_result'],4),'little');steps[label]=dict(exit=0 if result==1 else result or -1,result=result)
   steps_path.write_text(json.dumps(steps,indent=2)+'\n');assert result==1,(label,result)
 run('host-cases',host)
 dependencies={}
 for dep in list(build.rglob('*.obj.d'))+list(build.rglob('*.o.d'))+list(out.glob('*.d')):
  for name in shlex.split(dep.read_text().replace('\\\n',' ').split(':',1)[1]):
   path=Path(name)
   if path.is_file():dependencies[str(path.resolve())]=sha(path)
 for name in dependencies:
  p=Path(name)
  if not p.is_relative_to(out):target=out/'compiled-inputs'/str(p).lstrip('/');target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,target)
 (out/'compiled-dependencies.json').write_text(json.dumps(dependencies,indent=2)+'\n')
 result=json.loads(manifest.read_text());result.update(steps=steps,host_results=str(out/'host/results.json'),physical=False)
 (out/'result.json').write_text(json.dumps(result,indent=2)+'\n');print('PASS root-configured supervised camera and new focused boundaries')
if __name__=='__main__':main()
