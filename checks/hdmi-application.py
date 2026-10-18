#!/usr/bin/env python3
"""Build the HDMI image once and check its new application/admission boundaries."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
import sys
import unicorn
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_INTR
from unicorn.arm64_const import UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_X0
ROOT=Path(__file__).resolve().parents[1]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def save(p,v):p.write_text(json.dumps(v,indent=2)+'\n')
def main():
 p=argparse.ArgumentParser(description=__doc__)
 for name in ('output','freertos','tinyusb','clang','lld','objcopy'):p.add_argument('--'+name,type=Path,required=True)
 p.add_argument('--resume',action='store_true')
 p.add_argument('--continue-package',action='store_true')
 a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=a.resume or a.continue_package)
 commands=json.loads((out/'commands.json').read_text()) if a.resume or a.continue_package else []
 assert not (a.resume and a.continue_package)
 if a.continue_package:
  assert commands[-1]['label']=='package' and commands[-1]['exit']!=0
  assert not (out/'results.json').exists() and not (out/'result.json').exists() and not (out/'modeled-bundle').exists()
  assert sha(out/'build/sunstone-a733-hdmi.elf')==sha(out/'failed-package/image.elf')
 if a.resume:
  assert (out/'failed-link/manifest.json').is_file() and commands[-1]['label']=='image-build' and commands[-1]['exit']!=0
  assert not (out/'results.json').exists() and not (out/'result.json').exists() and not (out/'modeled-bundle').exists(), 'Resume is limited to the preserved pre-execution link failure'
 def run(label,argv):
  argv=list(map(str,argv))
  if (label!='configure' or a.continue_package) and any(c['label']==label and c['command']==argv and c['exit']==0 for c in commands):return
  r=subprocess.run(argv,capture_output=True,text=True)
  attempt=sum(c['label']==label for c in commands)
  (out/(label+('' if not attempt else '-'+str(attempt+1))+'.log')).write_text(r.stdout+r.stderr)
  commands.append(dict(label=label,command=argv,exit=r.returncode));save(out/'commands.json',commands)
  r.check_returncode()
 toolchain=out/'toolchain.cmake'
 toolchain.write_text(f'set(SUNSTONE_LLD "{a.lld.absolute()}" CACHE FILEPATH "")\ninclude("{ROOT}/cmake/aarch64-clang.cmake")\nset(CMAKE_C_COMPILER "{a.clang.resolve()}")\nset(CMAKE_ASM_COMPILER "{a.clang.resolve()}")\n')
 build=out/'build'
 run('configure',['cmake','-S',ROOT/'apps/hdmi','-B',build,f'-DCMAKE_TOOLCHAIN_FILE={toolchain}',f'-DSUNSTONE_LLD={a.lld.absolute()}',f'-DCMAKE_EXE_LINKER_FLAGS=--ld-path={a.lld.absolute()}',
  f'-DFREERTOS_DIR={a.freertos.resolve()}',f'-DTINYUSB_DIR={a.tinyusb.resolve()}', '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON'])
 run('image-build',['cmake','--build',build,'--target','sunstone-a733-hdmi','-j4'])
 image=build/'sunstone-a733-hdmi.elf'
 # Exercise the real offline packer/launcher build with explicitly synthetic
 # records. The encoded session is not physically admitted and cannot be deployed.
 spec=importlib.util.spec_from_file_location('hdmi_prepare',ROOT/'apps/hdmi/prepare.py')
 pack=importlib.util.module_from_spec(spec);spec.loader.exec_module(pack)
 parts,address,identity,size=pack.image_data(image)
 packdir=out/'pack-input';packdir.mkdir(exist_ok=a.continue_package)
 original=image.read_bytes();(packdir/'image.elf').write_bytes(original)
 raw=bytearray(size);struct.pack_into('<6Q',raw,0,pack.SESSION_MAGIC,1,size,1,2,3);raw[48:80]=identity
 struct.pack_into('<2Q',raw,208,4,5)
 proof=b'explicit modeled proof, not physical qualification'
 for name,offset in (('analog',112),('power',144),('supervision',176)):
  (packdir/name).write_bytes(proof);raw[offset:offset+32]=hashlib.sha256(proof).digest()
 record=dict(schema='a733-hdmi-supervised-admission-v1',image_sha256=hashlib.sha256(original).hexdigest(),
  epoch=1,generation=2,lease=3,supervisor=4,analog='analog',power='power',supervision='supervision')
 save(packdir/'admission.json',record);raw[80:112]=hashlib.sha256((packdir/'admission.json').read_bytes()).digest()
 (packdir/'session.bin').write_bytes(raw)
 run('package',[Path(sys.executable).absolute(),ROOT/'apps/hdmi/prepare.py','--image',packdir/'image.elf',
  '--session',packdir/'session.bin','--admission',packdir/'admission.json','--output',out/'modeled-bundle',
  '--clang',a.clang.resolve(),'--lld',a.lld.absolute(),'--objcopy',a.objcopy.resolve()])
 units=[u for u in json.loads((build/'compile_commands.json').read_text()) if 'sunstone-a733-hdmi.dir/' in u['command']]
 unit=next(u for u in units if u['file'].endswith('/apps/hdmi/output.c'))
 flags=shlex.split(unit['command']);flags=flags[1:flags.index('-o')]
 objects=[]
 for u in units:
  command=shlex.split(u['command']);obj=Path(command[command.index('-o')+1])
  objects.append(obj if obj.is_absolute() else Path(u['directory'])/obj)
 cases={
  'output':(['apps/hdmi/output.c'],['normal-stop','session-expiry','health-fault','stop-fault','wrong-task','time-regression']),
  'admission':(['apps/hdmi/admission.c','drivers/storage/storage_digest.c'],['immutable-session']),
  'controller':(['drivers/display/a733_hdmi.c','drivers/display/a733_hdmi_phy.c','drivers/display/a733_tcon_tv.c'],
   ['independent','supervised','ambiguous','missing','wrong-mode']),
  'native':([],['independent','supervised','ambiguous','missing','wrong-mode'])}
 images={}
 for group,(sources,labels) in cases.items():
  files=['checks/hdmi-application.c']+sources+([] if group=='native' else ['runtime/memory.c'])
  objs=[]
  for i,name in enumerate(files):
   obj=out/f'{group}-{i}.o';objs.append(obj)
   run(f'{group}-{i}-compile',[a.clang.resolve(),*flags,'-DHDMI_CHECK_'+group.upper(),'-MD','-MF',out/f'{group}-{i}.d',
    '-c',ROOT/name,'-o',obj])
  exe=out/(group+'.elf')
  linker=['-nostdlib','-static',f'-fuse-ld={a.lld.absolute()}','-Wl,--gc-sections','-Wl,-e,hdmi_check_run']
  if group=='native':linker+=['-Wl,-T,'+str(build/'storage-maintenance/link.ld')];objs+=objects
  else:linker+=['-Wl,-Ttext=0x41000000']
  run(group+'-link',[a.clang.resolve(),'--target=aarch64-none-elf',*objs,*linker,'-o',exe]);images[group]=exe
 # Preserve compiler-derived bytes and all executed artifacts before execution.
 inputs={}
 for d in [*build.rglob('*.o.d'),*build.rglob('*.obj.d'),*out.glob('*.d'),*(out/'modeled-bundle').glob('*.d')]:
  for name in shlex.split(d.read_text().replace('\\\n',' ').split(':',1)[1]):
   source=Path(name)
   if source.is_file():inputs[str(source.resolve())]=sha(source)
 for path in [Path(u['file']) for u in units]+list((ROOT/'apps/hdmi').glob('*'))+[Path(__file__),ROOT/'cmake/a733-hdmi-application.cmake']:
  if path.is_file():inputs[str(path.resolve())]=sha(path)
 for name,value in inputs.items():
  target=out/'inputs'/name.lstrip('/');target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(name,target)
  assert sha(target)==value
 save(out/'inputs.json',inputs)
 tools=[a.clang.resolve(),a.lld.resolve(),a.objcopy.resolve(),Path(sys.executable).resolve()]
 tools += [x for x in Path(unicorn.__file__).parent.rglob('*') if x.is_file() and (x.suffix=='.py' or '.so' in x.name)]
 tools += [Path(m.__file__).resolve() for m in tuple(sys.modules.values())
           if getattr(m,'__file__',None) and Path(m.__file__).is_file()]
 if (Path(sys.prefix)/'pyvenv.cfg').is_file():tools.append(Path(sys.prefix)/'pyvenv.cfg')
 tools=list(dict.fromkeys(tools))
 for tool in tools:
  target=out/'tools'/str(tool).lstrip('/');target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(tool,target)
 save(out/'tools.json',{str(t):sha(t) for t in tools})
 save(out/'artifacts.json',{str(f.relative_to(out)):sha(f) for f in [image,*images.values(),*objects,*out.glob('*.o'),*list((out/'modeled-bundle').glob('launch.*'))]})
 results=[]
 for group,(_,labels) in cases.items():
  with images[group].open('rb') as f:
   elf=ELFFile(f);segments=[(int(s['p_vaddr']),bytes(s.data()),int(s['p_memsz'])) for s in elf.iter_segments() if s['p_type']=='PT_LOAD']
   symbols={s.name:int(s['st_value']) for s in elf.get_section_by_name('.symtab').iter_symbols()}
  for mode,label in enumerate(labels):
   u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);pages=set()
   for base,data,size in segments:
    for page in range(base&~4095,(base+size+4095)&~4095,4096):
     if page not in pages:u.mem_map(page,4096);pages.add(page)
    u.mem_write(base,data)
   for page in range(0x50000000,0x50010000,4096):u.mem_map(page,4096)
   if 0x41f00000 not in pages:u.mem_map(0x41f00000,4096)
   u.reg_write(UC_ARM64_REG_SP,0x5000f000);u.reg_write(UC_ARM64_REG_X0,mode)
   stops=[]
   def stopped(machine,interrupt,unused):stops.append(interrupt);machine.emu_stop()
   u.hook_add(UC_HOOK_INTR,stopped)
   u.emu_start(symbols['hdmi_check_run'],0,count=3000000,timeout=10000000)
   result=int.from_bytes(u.mem_read(symbols['hdmi_check_result'],4),'little')
   results.append(dict(group=group,case=label,result=result,pc=u.reg_read(UC_ARM64_REG_PC),interrupts=stops))
   save(out/'results.json',results);assert result==1,(group,label,result)
 with image.open('rb') as f:
  elf=ELFFile(f);symbols={s.name for s in elf.get_section_by_name('.symtab').iter_symbols()}
 assert {'a733_hdmi_native_init','a733_hdmi_prepare','a733_hdmi_service','a733_pmic_poll','hdmi_output_task'}<=symbols
 assert not {'a733_de_init','tusb_init','sunxi_mmc_init','a733_watchdog_hw_admit'}&symbols
 # Verify the new encoding and input-snapshot correction without reexecuting
 # the launcher or rebuilding any input.
 request,contract=pack.request_bytes(address,identity,raw,1,2,3,4)
 assert len(request)==128 and len(contract)==4096 and request[96:]==hashlib.sha256(raw).digest()
 assert contract[192:224]==request[96:] and not any(contract[80:112])
 validated=pack.validate(packdir/'image.elf',packdir/'session.bin',packdir/'admission.json')
 snapshots=validated[-1]
 for path in packdir.iterdir():path.write_bytes(b'replaced after validation')
 assert snapshots['image.elf']==original and snapshots['analog-record']==proof
 assert snapshots['admission.json']!=b'replaced after validation' and validated[3]==raw
 save(out/'result.json',dict(image_sha256=sha(image),build_id=(build/'hdmi-application/build-id.txt').read_text().strip(),
  checked_cases=len(results),packing=True,physical=False))
 print('PASS HDMI image and new application/admission boundaries; no physical output qualification')
if __name__=='__main__':main()
