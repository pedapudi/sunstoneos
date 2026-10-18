#!/usr/bin/env python3
"""Freeze the corrected PD rail profile; run only new rail/admission boundaries."""
import argparse,hashlib,importlib.util,json,os,shutil,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
CACHE=Path.home()/'.cache/sunstoneos'
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def save(p,value):p.write_text(json.dumps(value,indent=2)+'\n')
def module(name,p):
 s=importlib.util.spec_from_file_location(name,p);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m

def extract(text,signature):
 a=text.index(signature);b=text.index('{',a);depth=1;end=b+1
 while depth:depth+=(text[end]=='{')-(text[end]=='}');end+=1
 return text[a:end]

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);p.add_argument('--resume-setup',action='store_true')
 mode=p.add_mutually_exclusive_group(required=True);mode.add_argument('--setup-only',action='store_true');mode.add_argument('--run-only',action='store_true')
 a=p.parse_args();out=a.output.resolve();source=out/'source'
 if a.setup_only:
  out.mkdir(parents=True,exist_ok=a.resume_setup)
  for name in subprocess.check_output(['git','ls-files','--cached','--others','--exclude-standard'],cwd=ROOT,text=True).splitlines():
   src=ROOT/name
   if src.is_file():dst=source/name;dst.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(src,dst)
  locks=json.loads((source/'sources.lock.json').read_text());pins={}
  for name in ('board-schematic','board-dts','pcie-pcie-sunxi-rc-c','pcie-sunxi-cadence-combophy-c'):
   # Board DTS uses the lock's actual key discovered by matching its archive.
   if name=='board-dts':name='board-gpu-power-sun60i-a733-orangepi-zero3w-dts'
   value=locks[name];aliases={'pcie-pcie-sunxi-rc-c':'pcie/kernel/bsp/drivers/pci/controller/allwinner/pcie-sunxi-rc.c','pcie-sunxi-cadence-combophy-c':'pcie/kernel/bsp/drivers/phy/sunxi-cadence-combophy.c'}
   original=CACHE/'sources'/aliases.get(name,value['archive']);assert digest(original)==value['sha256'],name
   target=out/'references'/Path(value['archive']).name;target.parent.mkdir(exist_ok=True);shutil.copy2(original,target);pins[name]=dict(path=str(original),sha256=digest(original),preserved=str(target))
  for name in ('page6.png','audit.json'):
   target=out/'references'/name;shutil.copy2(CACHE/'evidence/pwm-adc-topology-20261018'/name,target)
  save(out/'reference-pins.json',pins)
  address='#include "a733_pcie_native.h"\n'+extract((source/'drivers/pci/a733_pcie_native.c').read_text(),'int a733_pcie_native_address(')+'\n'
  (out/'address.c').write_text(address)
  save(out/'source-inputs.json',{str(p.relative_to(source)):digest(p) for p in source.rglob('*') if p.is_file()})
 else:
  for path,h in json.loads((out/'runtime-inputs.json').read_text()).items():assert digest(Path(path))==h,path
 steps_path=out/'steps.json';steps=json.loads(steps_path.read_text()) if steps_path.exists() else {}
 def command(name,values):
  values=list(map(str,values))
  if steps.get(name,{}).get('exit')==0:assert steps[name]['command']==values;return
  steps[name]={'state':'running','command':values};save(steps_path,steps)
  r=subprocess.run(values,capture_output=True,env={**os.environ,'PYTHONDONTWRITEBYTECODE':'1'},timeout=240)
  (out/(name+'.log')).write_bytes(r.stdout+r.stderr);steps[name].update(exit=r.returncode,state='complete');save(steps_path,steps)
  if r.returncode:raise RuntimeError(name+' failed; inspect preserved log')
 compiler=Path('/usr/bin/clang-21');linker=CACHE/'host/usr/bin/ld.lld-21';image=out/'build/sunstone-a733-pcie.elf'
 if a.setup_only:
  units=['checks/a733-pcie-pd-rail.c','drivers/pci/a733_pcie.c','drivers/pci/a733_pcie_phy.c','drivers/power/a733_pmic.c','drivers/power/a733_rail_leases.c','drivers/clock/a733_clock_leases.c','drivers/pinctrl/a733_pins.c','drivers/pinctrl/a733_pin_tables.c','drivers/gpu/a733_gpu_resources.c']
  command('rail-build',[compiler,'-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(source/'include'),*[source/u for u in units],out/'address.c','-o',out/'rail'])
  command('nominal-provider-preflight',[out/'rail','--preflight'])
  (out/'toolchain.cmake').write_text(f'set(SUNSTONE_LLD "{linker}" CACHE FILEPATH "")\ninclude("{source}/cmake/aarch64-clang.cmake")\nset(CMAKE_C_COMPILER "{compiler}")\nset(CMAKE_ASM_COMPILER "{compiler}")\n')
  deps=CACHE/'evidence/h264-native-20261018/dependencies'
  command('configure',['cmake','-S',source,'-B',out/'build','-DCMAKE_TOOLCHAIN_FILE='+str(out/'toolchain.cmake'),'-DPython3_EXECUTABLE='+sys.executable,'-DFREERTOS_DIR='+str(deps/'freertos'),'-DTINYUSB_DIR='+str(deps/'tinyusb'),'-DSUNSTONE_TCP_DIR='+str(deps/'tcp'),'-DSUNSTONE_RSN_DIR='+str(deps/'wpa_supplicant-2.12'),'-DSUNSTONE_NETWORK_C_HEADERS='+str(deps/'headers'),'-DSUNSTONE_BOARD_SUPERVISED=ON','-DSUNSTONE_A733_PCIE=ON','-DCMAKE_EXPORT_COMPILE_COMMANDS=ON'])
  command('native-build',['cmake','--build',out/'build','--target','sunstone-a733-pcie','-j4'])
  snapshot=module('pcie_pd_evidence',source/'tools/evidence-snapshot.py');dependencies,commands,records=snapshot.collect_dependencies(out/'build','sunstone-a733-pcie')
  save(out/'compiled-dependencies.json',dependencies);save(out/'compiled-commands.json',commands);save(out/'dependency-records.json',records)
  for path,h in dependencies.items():
   path=Path(path)
   if not path.is_relative_to(out):target=out/'compiled-inputs'/str(path).lstrip('/');target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(path,target);assert digest(target)==h
  fixture=module('pcie_pd_producer',source/'checks/pcie-application-fixture.py');
  if not (out/'fixture').exists():fixture.produce(image,out/'fixture')
  native=module('pcie_pd_native',source/'checks/pcie-pd-rail-native.py');old,elf=native.setup(image,out,source)
  raw=(out/'fixture/session-template.bin').read_bytes();m=old.Machine(elf,raw,'setup')
  assert m.call('pcie_session_valid',0x41f00000)==1 and m.call('pcie_construct')==1
  assert m.call('admitted',m.base,m.port+elf.layouts['a733_pcie']['config'],unit='/apps/pcie/native.c')==1
  save(out/'native-validator-preflight.json',{'passed':True,'session_sha256':hashlib.sha256(raw).hexdigest(),'actual_producer':'checks/pcie-application-fixture.py','authority':'Software model, nondeployable','pmic_apbs0_offset':elf.layouts['a733_pmic']['apbs0']})
  manifest=json.loads((out/'build/pcie-application/manifest.json').read_text());save(out/'image-manifest.json',manifest)
  import unicorn,elftools
  paths=[p for p in out.rglob('*') if p.is_file() and p.name not in ('steps.json','runtime-inputs.json')]+list(map(Path,dependencies))+[compiler,linker,Path(sys.executable)]
  paths += [p for package in (unicorn,elftools) for p in Path(package.__file__).parent.rglob('*') if p.is_file() and '__pycache__' not in p.parts]
  save(out/'runtime-inputs.json',{str(p):digest(p) for p in dict.fromkeys(paths)})
  print('Setup passed: actual provider/producer/native validators; no new rail behavior cases yet.');return
 command('rail-cases',[out/'rail'])
 for name,path in [('native-requests','pcie-pd-rail-native.py'),('electrical-admission','pcie-pd-rail-host.py')]:
  if steps.get(name,{}).get('exit')==0:continue
  steps[name]={'state':'running'};save(steps_path,steps)
  try:
   m=module('pcie_pd_'+name,source/'checks'/path)
   if name=='native-requests':m.run(image,out,source,(out/'fixture/session-template.bin').read_bytes())
   else:m.run(out,source)
  except BaseException:
   import traceback
   (out/(name+'-failure.log')).write_text(traceback.format_exc());steps[name]={'exit':1};save(steps_path,steps);raise
  steps[name]={'exit':0};save(steps_path,steps)
 save(out/'result.json',{'passed':True,'physical_access':False,'image_sha256':digest(image),'scope':'Corrected BLDO1 PD reservation/snapshot and electrical admission; actual composed shared providers, changed native request instructions over modeled controller/physical boundaries; prior PHY/protocol results reused.'})
 print('PASS corrected PCIe PD-bank rail boundaries')
if __name__=='__main__':main()
