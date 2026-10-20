#!/usr/bin/env python3
"""One raw26 HDMI native composition, with modeled architectural/MMIO effects.

Build the pattern target once. Reuse its compiled native provider/controller
objects in the focused fixture; no boot, GIC or physical hardware executes.
Earlier reference matrices and frozen executables remain separate.
"""
import argparse
import copy
import hashlib
import importlib.util
import json
import pickle
import re
import shlex
import subprocess
import sys
from pathlib import Path
import elftools
import unicorn
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_LR, UC_ARM64_REG_PSTATE
ROOT=Path(__file__).resolve().parents[1]
def sha(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def save(p,v): p.write_text(json.dumps(v,indent=2)+'\n')
def module(name,path):
 spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value

def main():
 p=argparse.ArgumentParser(description=__doc__)
 for name in ('output','freertos','tinyusb','clang','lld','objcopy','sources'):p.add_argument('--'+name,type=Path,required=True)
 p.add_argument('--resume',action='store_true',help='reuse the frozen build and completed nominal path; run only remaining checks')
 a=p.parse_args();out=a.output.absolute()
 if a.resume:
  prior_results=json.loads((out/'results.json').read_text());assert len(prior_results)==1
  assert prior_results[0]['case']=='native-raw26-720p-pattern-prepare-start-stop' and prior_results[0]['passed'] and prior_results[0]['physical'] is False
  commands=json.loads((out/'commands.json').read_text())
  for path,digest in json.loads((out/'artifacts.json').read_text()).items():assert sha(path)==digest
  (out/'continuation-source.py').write_bytes(Path(__file__).read_bytes())
  save(out/'continuation-inputs.json',dict(source_sha256=sha(Path(__file__)),image_and_checkpoints={str(q):sha(q) for q in out.glob('*') if q.is_file() and (q.suffix in ('.pickle','.bin','.json') or q.name=='composition.elf')}))
 else:out.mkdir(parents=True,exist_ok=False);commands=[]
 def run(label,argv,cwd=None):
  argv=list(map(str,argv))
  if a.resume:
   assert any(row['label']==label and row['argv']==argv and row['status']==0 for row in commands), 'changed frozen build command'
   return
  r=subprocess.run(argv,cwd=cwd,capture_output=True,text=True)
  (out/(label+'.log')).write_text(r.stdout+r.stderr);commands.append(dict(label=label,argv=argv,status=r.returncode));save(out/'commands.json',commands);r.check_returncode()
 toolchain=out/'toolchain.cmake';toolchain.write_text(f'set(SUNSTONE_LLD "{a.lld.absolute()}" CACHE FILEPATH "")\ninclude("{ROOT}/cmake/aarch64-clang.cmake")\nset(CMAKE_C_COMPILER "{a.clang.absolute()}")\nset(CMAKE_ASM_COMPILER "{a.clang.absolute()}")\n')
 build=out/'build'
 run('configure',['cmake','-S',ROOT/'apps/hdmi','-B',build,f'-DCMAKE_TOOLCHAIN_FILE={toolchain}',f'-DFREERTOS_DIR={a.freertos.absolute()}',f'-DTINYUSB_DIR={a.tinyusb.absolute()}','-DSUNSTONE_HDMI_FRAMEBUFFER=OFF','-DCMAKE_EXPORT_COMPILE_COMMANDS=ON'])
 run('image-build',['cmake','--build',build,'--target','sunstone-a733-hdmi','-j4'])
 image=build/'sunstone-a733-hdmi.elf'
 units=[u for u in json.loads((build/'compile_commands.json').read_text()) if 'sunstone-a733-hdmi.dir/' in u['command']]
 selected=['platform/a733/hdmi_native.c','drivers/display/a733_hdmi.c','drivers/display/a733_hdmi_phy.c','drivers/display/a733_tcon_tv.c','drivers/clock/a733_clock_leases.c','drivers/power/a733_rail_leases.c','drivers/power/a733_pmic.c','drivers/pinctrl/a733_pins.c','drivers/pinctrl/a733_pin_tables.c','drivers/gpu/a733_gpu_resources.c','platform/a733/counter_clock.c','arch/aarch64/device_memory.c','platform/a733/shared_locks.c','runtime/memory.c']
 objects=[]
 for name in selected:
  unit=next(u for u in units if Path(u['file'])==ROOT/name);cmd=shlex.split(unit['command']);obj=Path(cmd[cmd.index('-o')+1]);objects.append(obj if obj.is_absolute() else Path(unit['directory'])/obj)
 flags=shlex.split(next(u for u in units if u['file'].endswith('/platform/a733/hdmi_native.c'))['command']);flags=flags[1:flags.index('-o')]
 (out/'assert.h').write_text('#pragma once\n_Noreturn void platform_panic(const char *,unsigned);\n#define assert(x) do {if(!(x))platform_panic(__FILE__,__LINE__);} while(0)\n')
 (out/'stdio.h').write_text('#pragma once\nint printf(const char *,...);\n')
 (out/'support.c').write_text('_Noreturn void platform_panic(const char *file,unsigned line){(void)file;(void)line;for(;;)__asm__ volatile("brk #0");}\n')
 for name,source in [('fixture',ROOT/'checks/a733-hdmi-native.c'),('support',out/'support.c')]:
  obj=out/(name+'.o');objects.append(obj);run(name+'-compile',[a.clang.absolute(),*flags,'-I'+str(out),'-Wno-unused-function','-MD','-MF',out/(name+'.d'),'-c',source,'-o',obj])
 (out/'link.ld').write_text('ENTRY(hdmi_native_check)\nSECTIONS { . = 0x41000000; .text : { *(.text*) } .rodata : { *(.rodata*) } .data : { *(.data*) } .bss (NOLOAD) : { *(.bss*) *(COMMON) } ASSERT(. < 0x41800000,"fixture overflow") }\n')
 exe=out/'composition.elf';run('composition-link',[a.lld.absolute(),'--gc-sections','-T',out/'link.ld',*objects,'-o',exe])
 inputs=json.loads((out/'inputs.json').read_text()) if a.resume else {}
 def keep(path):
  path=path.resolve();data=path.read_bytes();digest=hashlib.sha256(data).hexdigest()
  if str(path) in inputs:
   previous=inputs[str(path)];assert sha(Path(previous['snapshot']))==previous['sha256']
   if not (a.resume and path==Path(__file__).resolve()):assert previous['sha256']==digest
   return
  dest=out/'inputs'/str(path).lstrip('/');dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(data);inputs[str(path)]=dict(sha256=digest,snapshot=str(dest))
 for dep in [*build.rglob('*.o.d'),*build.rglob('*.obj.d'),*out.glob('*.d')]:
  for name in shlex.split(dep.read_text().replace('\\\n',' ').split(':',1)[1]):
   path=Path(name);keep(path if path.is_absolute() else build/path)
 for path in [Path(__file__),ROOT/'checks/a733-hdmi-reference.py',ROOT/'apps/hdmi/prepare.py',ROOT/'apps/gpu-display/prepare.py',ROOT/'sources.lock.json',toolchain,out/'link.ld',a.clang,a.lld,a.objcopy,Path(sys.executable).resolve(),Path(sys.prefix)/'pyvenv.cfg']:
  keep(path)
 for package in (unicorn,elftools):
  for path in Path(package.__file__).parent.rglob('*'):
   if path.is_file() and (path.suffix=='.py' or '.so' in path.name):keep(path)
 lock=json.loads((ROOT/'sources.lock.json').read_text())
 for row in lock.values():
  if isinstance(row,dict) and row.get('archive') and ('lowlevel_hdmi20/phy_top.' in row['archive'] or row['archive'].endswith('ccu-sun60iw2.c') or 'sunxi_drm_hdmi.c' in row['archive'] or row['archive'].endswith('/phy_snps.c') or row['archive'].endswith('/dw-hdmi.c') or row['archive'].endswith('/dw_i2cm.c') or row['archive'].endswith('/sun60i-a733-orangepi-zero3w.dts')):
   path=a.sources/row['archive'];assert sha(path)==row['sha256'];keep(path)
 manual=a.sources/'camera-research/A733_User_Manual_V1.00.txt'
 assert sha(manual)=='219d3c21e5377d18a096765f429555d0693f7fa9b2ae2eb5bb3f225d8b6a09c2';keep(manual)
 save(out/'inputs.json',inputs)
 save(out/'artifacts.json',{str(p):sha(p) for p in [image,exe,*objects,build/'hdmi-application/image.map',build/'hdmi-application/build-id.txt']})
 # Shared host admission is exercised independently of packaging/launch builds.
 if not a.resume:
  host=module('hdmi_clock_pack',ROOT/'apps/hdmi/prepare.py');profiles=[]
  for raw,n in [(24000000,99),(26000000,91)]:
   profile=dict(raw_dcxo_hz=raw,reference_hz=24000000,sfr_source=0,sfr_hz=24000000,pixel_hz={'720p60':raw*n//32,'1080p60':raw*n//16});assert host.clock_profile({'clock_profile':profile},raw)==profile;profiles.append(profile)
  for profile in [None,dict(profiles[1],raw_dcxo_hz=19200000),dict(profiles[1],sfr_source=1),dict(profiles[1],pixel_hz=profiles[0]['pixel_hz']),dict(profiles[1],reference_hz=26000000)]:
   try:host.clock_profile({'clock_profile':profile},26000000)
   except ValueError:pass
   else:raise AssertionError('invalid host clock profile accepted')
  low=a.sources/'hdmi-research/kernel/bsp/drivers/drm/sunxi_device/hardware/lowlevel_hdmi20'
  ref=module('hdmi_clock_reference',ROOT/'checks/a733-hdmi-reference.py')
  for profile in profiles:
   table=ref.block((low/'phy_top.c').read_text(),'sun60i_phypll_'+str(profile['raw_dcxo_hz']//1000000)+'m')
   for nominal,mode in [(74250,'720p60'),(148500,'1080p60')]:
    row=re.search(r'\{\s*'+str(nominal)+r'\s*,([^}]+)\}',table);assert row
    word=int(re.findall(r'0x[0-9a-fA-F]+',row.group(1))[0],16);assert profile['raw_dcxo_hz']*(((word>>8)&255)+1)//((((word>>16)&127)+1)*(((word>>1)&1)+1))==profile['pixel_hz'][mode]
  save(out/'clock-profiles.json',profiles)
 else:
  profiles=json.loads((out/'clock-profiles.json').read_text());ref=module('hdmi_clock_reference',ROOT/'checks/a733-hdmi-reference.py')
 with exe.open('rb') as stream:
  elf=ELFFile(stream);segments=[(int(s['p_vaddr']),s.data()) for s in elf.iter_segments() if s['p_type']=='PT_LOAD'];sym={s.name:int(s['st_value']) for s in elf.get_section_by_name('.symtab').iter_symbols()}
  immutable=[(int(s['p_vaddr']),s.data()) for s in elf.iter_segments() if s['p_type']=='PT_LOAD' and not(int(s['p_flags'])&2)]
  stage_offsets=set()
  for cu in elf.get_dwarf_info().iter_CUs():
   for die in cu.iter_DIEs():
    if die.tag=='DW_TAG_structure_type' and die.attributes.get('DW_AT_name') and die.attributes['DW_AT_name'].value==b'a733_hdmi_native':
     for member in die.iter_children():
      if member.tag=='DW_TAG_member' and member.attributes.get('DW_AT_name') and member.attributes['DW_AT_name'].value==b'stage':stage_offsets.add(int(member.attributes['DW_AT_data_member_location'].value))
  assert len(stage_offsets)==1;native_stage=stage_offsets.pop()
 u=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
 regions=[(0x41000000,0x800000),(0x43f00000,0x100000),(0x02002000,0x2000),(0x03000000,0x1000),(0x05510000,0x10000),(0x05520000,0x8000),(0x05600000,0x1000),(0x05730000,0x1000),(0x07010000,0x1000),(0x0706a000,0x1000),(0x07090000,0x1000)]
 for at,size in regions:u.mem_map(at,size)
 for at,data in segments:u.mem_write(at,data)
 def verify_code():
  for at,data in immutable:assert bytes(u.mem_read(at,len(data)))==data, 'checkpoint executable identity differs'
 def saved_state(label):
  path=out/(label+'.pickle');assert path.resolve().parent==out
  saved=pickle.loads(path.read_bytes());assert len(saved['ram'])==len(regions)
  memory=[]
  for index,((at,size,name),expected) in enumerate(zip(saved['ram'],regions)):
   source=Path(name).resolve();assert (at,size)==expected
   assert source.parent==out and source.name==label+f'-ram-{index}.bin'
   data=source.read_bytes();assert len(data)==size;memory.append((at,data))
  return saved,memory
 def restore(context,memory):
  assert [(at,len(data)) for at,data in memory]==regions
  for at,data in memory:u.mem_write(at,data)
  verify_code();u.context_restore(context)
 def put(at,value,size=4):u.mem_write(at,int(value).to_bytes(size,'little'))
 def get(at,size=4):return int.from_bytes(u.mem_read(at,size),'little')
 put(0x07090160,0x10008002);put(0x02002000,0xf8675f00);put(0x070101ac,0x40)
 for off in (0x16ec,0x16cc,0x1604):put(0x02002000+off,0x10000)
 put(0x0200368c,0x70000)
 for off,value in zip((0,4,0x20,0x24,0x28,0x2c,0x30,0x34),(8,0xa0,0x00110001,0x10000000,0x76543210,0,0,0x001f0040)):put(0x05600000+off,value)
 put(0x03000160,0xa5a50000);put(0x03000164,0xabcdef12)
 u.cpr_write(3,3,14,0,0,24000000);u.reg_write(UC_ARM64_REG_PSTATE,0x345)
 edid=bytearray(256);edid[:8]=bytes((0,255,255,255,255,255,255,0));edid[18:21]=bytes((1,4,128));edid[126]=1;edid[128:132]=bytes((2,3,12,0));edid[132:140]=bytes((0x41,4,0x65,3,12,0,0x10,0))
 for off in (0,128):edid[off+127]=(-sum(edid[off:off+127]))&255
 state=dict(internal={},pending={},events=[],reads=0,translations=0);checkpoints={};panic=[];mode='nominal'
 def snapshot(label):
  ram=[]
  for index,(at,size) in enumerate(regions):
   path=out/(label+f'-ram-{index}.bin');path.write_bytes(bytes(u.mem_read(at,size)));ram.append([at,size,str(path)])
  (out/(label+'.pickle')).write_bytes(pickle.dumps(dict(cpu=u.context_save(),ram=ram,model=copy.deepcopy(state))))
 def instruction(cpu,at,size,unused):
  for address,value in state['pending'].items():put(address,value,1)
  state['pending'].clear()
  if at==sym['platform_panic']:
   panic.append(dict(file=bytes(cpu.mem_read(cpu.reg_read(UC_ARM64_REG_X0),512)).split(b'\0')[0].decode(errors='replace'),line=cpu.reg_read(UC_ARM64_REG_X1)));cpu.emu_stop();return
  if at==sym['hdmi_checkpoint'] and mode=='nominal':
   stage=get(sym['hdmi']+native_stage);which=1 if stage==8 else 3 if stage==15 else 0
   if which in (1,3) and which not in checkpoints:
    checkpoints[which]=(cpu.context_save(),[(a,bytes(cpu.mem_read(a,n))) for a,n in regions],copy.deepcopy(state))
    snapshot('before-baseline' if which==1 else 'prepared')
  word=get(at);masked=word&0xffffffe0;rt=word&31
  values={0xd5384240:4,0xd5381000:5,0xd53b0020:(4<<24)|(4<<16)}
  if masked==0xd53be020:values[masked]=get(sym['hdmi_test_time'],8)*24
  if masked in values:
   assert rt<29;cpu.reg_write(UC_ARM64_REG_X0+rt,values[masked]);cpu.reg_write(UC_ARM64_REG_PC,at+4)
  elif masked in (0xd5087800,0xd5087820):
   address=cpu.reg_read(UC_ARM64_REG_X0+rt);cpu.cpr_write(3,0,7,4,0,(address&~4095)|0x100);state['translations']+=1;put(sym['hdmi_translation_count'],state['translations']);cpu.reg_write(UC_ARM64_REG_PC,at+4)
 def read(cpu,access,at,size,value,unused):
  if at>=0x41000000:return
  state['reads']+=1;put(sym['hdmi_mmio_count'],get(sym['hdmi_mmio_count'])+1)
  if at==0x0706a008:put(at,get(0x0706a000)&15)
  elif at==0x05600040:put(at,1 if get(0x05600020)&0xe0000000==0xe0000000 else 0)
  elif at==0x05523004:put(at,2|(1 if get(0x05523000,1)&8 else 0),1)
 def write(cpu,access,at,size,value,unused):
  if at>=0x41000000:return
  put(sym['hdmi_mmio_count'],get(sym['hdmi_mmio_count'])+1);state['events'].append([at,size,value,get(sym['hdmi_test_time'],8)])
  if at in (0x05520105,0x05520108):state['pending'][at]=get(at,1)&~value
  elif at==0x05527e04:
   index=get(0x05527e0a,1)*256+get(0x05527e01,1);assert value in (1,2) and index<len(edid);put(0x05527e03,edid[index],1);put(0x05520105,2,1)
  elif at==0x05523026:
   index=get(0x05523021,1)
   if value==0x10:state['internal'][index]=get(0x05523022,1)*256+get(0x05523023,1)
   else:
    assert value==1;v=state['internal'][index];put(0x05523024,v>>8,1);put(0x05523025,v&255,1)
   put(0x05520108,2,1)
 # Exact architectural sites retain a bounded hook set rather than tracing
 # every instruction. Architectural execution is modeled, not port-qualified.
 sites={sym['platform_panic'],sym['hdmi_checkpoint']}
 for at,data in segments:
  for off in range(0,len(data)-3,4):
   masked=int.from_bytes(data[off:off+4],'little')&0xffffffe0
   if masked in (0xd5384240,0xd5381000,0xd53b0020,0xd53be020,0xd5087800,0xd5087820):sites.add(at+off)
 for site in sites:u.hook_add(UC_HOOK_CODE,instruction,begin=site,end=site)
 u.hook_add(UC_HOOK_MEM_READ,read,begin=0x02000000,end=0x07ffffff);u.hook_add(UC_HOOK_MEM_WRITE,write,begin=0x02000000,end=0x07ffffff)
 u.reg_write(UC_ARM64_REG_SP,0x43fff000);u.reg_write(UC_ARM64_REG_LR,0x43ffe000);results=[]
 def execute(name,start):
  panic.clear()
  try:
   for chunk in range(16):
    u.emu_start(start,0x43ffe000,count=20000000)
    if panic or u.reg_read(UC_ARM64_REG_PC)==0x43ffe000:break
    snapshot(name+'-budget-'+str(chunk));start=u.reg_read(UC_ARM64_REG_PC)
   else:raise AssertionError('bounded instruction allowance exhausted; checkpoint retained')
   assert not panic and u.reg_read(UC_ARM64_REG_PC)==0x43ffe000 and u.reg_read(UC_ARM64_REG_X0)==0,panic
   results.append(dict(case=name,passed=True,time_us=get(sym['hdmi_test_time'],8),physical=False));save(out/'results.json',results)
  except BaseException as error:
   snapshot(name+'-stopped');save(out/'failure.json',dict(case=name,error=repr(error),panic=panic,pc=hex(u.reg_read(UC_ARM64_REG_PC))));raise
 if not a.resume:
  execute('native-raw26-720p-pattern-prepare-start-stop',sym['hdmi_native_check'])
  snapshot('nominal-stopped-retained');save(out/'nominal-trace.json',state)
 else:
  saved,memory=saved_state('nominal-stopped-retained');restore(saved['cpu'],memory)
  assert u.reg_read(UC_ARM64_REG_PC)==0x43ffe000
  state=copy.deepcopy(saved['model']);results=json.loads((out/'results.json').read_text())
  assert state['events']==json.loads((out/'nominal-trace.json').read_text())['events']
  saved,memory=saved_state('before-baseline');checkpoints[1]=(saved['cpu'],memory,saved['model'])
 assert get(0x02003690)&0x87000000==0x80000000
 assert get(0x05600020)&0x00ffffff==0x001f5a00 | (get(0x05600020)&0xff)
 assert get(0x0200368c)&0x40000
 # Independently compare actual executed PLL/PHY/DDC/CSC writes with pinned code.
 dts=a.sources/'gpu-power/arch/arm64/boot/dts/allwinner/sun60i-a733-orangepi-zero3w.dts';keep(dts)
 prepared=checkpoints[3][2]['events'] if 3 in checkpoints else state['events'][:next(i for i,event in enumerate(state['events']) if event[0]==0x05730090 and event[2]&0x80000000)]
 comparison=ref.verify(dict(mode=4,range=0,phy_reference_hz=26000000,writes=prepared),a.sources/'hdmi-research/kernel',dts.read_text())
 save(out/'source-comparison.json',comparison)
 for name,oscillator,reference,case in [('raw19.2-refusal',0x10004002,0xf84f6300,4),('bad-ref-lock-refusal',0x10008002,0xe8675f00,5),('admitted26-observed24-refusal',0x10000002,0xf85f5f00,6)]:
  mode=name;context,memory,old=checkpoints[1]
  restore(context,memory);state=copy.deepcopy(old)
  assert u.reg_read(UC_ARM64_REG_PC)==sym['hdmi_checkpoint']
  assert get(sym['hdmi']+native_stage)==8
  put(0x07090160,oscillator);put(0x02002000,reference);put(sym['hdmi_fault_case'],case)
  execute(name,u.reg_read(UC_ARM64_REG_PC))
  assert state['events']==old['events'], 'reference refusal must precede VO1/HDMI writes'
 for path,row in inputs.items():
  assert sha(Path(row['snapshot']))==row['sha256'],path
  if not (a.resume and Path(path)==Path(__file__).resolve()):assert sha(path)==row['sha256'],path
 save(out/'inputs.json',inputs)
 files={str(p.relative_to(out)):dict(bytes=p.stat().st_size,sha256=sha(p)) for p in out.rglob('*') if p.is_file()}
 save(out/'manifest.json',dict(schema='a733-hdmi-clock-native-evidence-v1',physical=False,image_sha256=sha(image),composition_sha256=sha(exe),scope=__doc__,files=files));print('PASS raw26 native HDMI path and changed parent guards',flush=True)

if __name__=='__main__':main()
