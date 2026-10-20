#!/usr/bin/env python3
"""Build one framebuffer image and execute its new native composition once."""
import argparse, copy, hashlib, importlib.util, io, json, pickle, shlex, struct, subprocess, sys, time
from pathlib import Path
import unicorn, elftools
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_LR, UC_ARM64_REG_PSTATE
ROOT=Path(__file__).resolve().parents[1]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def save(p,v):p.write_text(json.dumps(v,indent=2)+'\n')
def main():
 p=argparse.ArgumentParser(description=__doc__)
 for name in ('output','freertos','tinyusb','clang','lld','objcopy','sources'):p.add_argument('--'+name,type=Path,required=True)
 p.add_argument('--resume',action='store_true')
 a=p.parse_args();out=a.output.absolute()
 if a.resume:
  assert not (out/'results.json').exists() and not (out/'geometry.json').exists()
  commands=json.loads((out/'commands.json').read_text())
  assert commands[-1]['name']=='fixture-compile' and commands[-1]['status']!=0
  previous=json.loads((out/'fixture-compile-failure/manifest.json').read_text())
  image=out/'build/sunstone-a733-hdmi.elf'
  assert sha(image)==previous[str(image)]['sha256']
 else:
  out.mkdir(parents=True,exist_ok=False);commands=[]
 def run(name,argv):
  argv=list(map(str,argv))
  if a.resume and any(c['name']==name and c['argv']==argv and c['status']==0 for c in commands):return
  r=subprocess.run(argv,capture_output=True,text=True)
  (out/(name+'.log')).write_text(r.stdout+r.stderr)
  commands.append(dict(name=name,argv=argv,status=r.returncode));save(out/'commands.json',commands)
  if r.returncode:raise SystemExit(name+': '+r.stderr[-3000:])
 toolchain=out/'toolchain.cmake';toolchain.write_text(f'set(SUNSTONE_LLD "{a.lld.absolute()}" CACHE FILEPATH "")\ninclude("{ROOT}/cmake/aarch64-clang.cmake")\nset(CMAKE_C_COMPILER "{a.clang.absolute()}")\nset(CMAKE_ASM_COMPILER "{a.clang.absolute()}")\n')
 build=out/'build'
 run('configure',['cmake','-S',ROOT/'apps/hdmi','-B',build,f'-DCMAKE_TOOLCHAIN_FILE={toolchain}',f'-DFREERTOS_DIR={a.freertos.absolute()}',f'-DTINYUSB_DIR={a.tinyusb.absolute()}', '-DSUNSTONE_HDMI_FRAMEBUFFER=ON','-DCMAKE_EXPORT_COMPILE_COMMANDS=ON'])
 run('image-build',['cmake','--build',build,'--target','sunstone-a733-hdmi','-j4'])
 image=build/'sunstone-a733-hdmi.elf'
 units=[u for u in json.loads((build/'compile_commands.json').read_text()) if 'sunstone-a733-hdmi.dir/' in u['command']]
 selected=['apps/hdmi/framebuffer.c','apps/hdmi/output.c','platform/a733/hdmi_native.c',
  'drivers/display/a733_display_engine.c','drivers/display/a733_tcon_lcd.c','drivers/display/a733_dsi_video.c','drivers/display/a733_dsi_phy.c',
  'drivers/display/a733_hdmi.c','drivers/display/a733_hdmi_phy.c','drivers/display/a733_tcon_tv.c',
  'drivers/clock/a733_clock_leases.c','drivers/clock/a733_media_clock.c','drivers/clock/a733_media_clock_native.c',
  'drivers/iommu/a733_iommu.c','drivers/power/a733_rail_leases.c','drivers/power/a733_pmic.c','drivers/power/a733_pmic_native.c',
  'drivers/pinctrl/a733_pins.c','drivers/pinctrl/a733_pin_tables.c','drivers/pinctrl/a733_pins_native.c',
  'drivers/gpu/a733_gpu_resources.c','platform/a733/counter_clock.c','platform/a733/shared_locks.c',
  'arch/aarch64/device_memory.c','platform/a733/memory_uncached.c','runtime/device_memory_pool.c','runtime/memory.c','runtime/memmove.c']
 objects=[]
 for name in selected:
  u=next(u for u in units if Path(u['file'])==ROOT/name);cmd=shlex.split(u['command']);obj=Path(cmd[cmd.index('-o')+1]);objects.append(obj if obj.is_absolute() else Path(u['directory'])/obj)
 u=next(u for u in units if u['file'].endswith('/apps/hdmi/framebuffer.c'))
 flags=shlex.split(u['command']);flags=flags[1:flags.index('-o')]
 (out/'assert.h').write_text('#pragma once\n_Noreturn void platform_panic(const char *,unsigned);\n#define assert(x) do {if(!(x))platform_panic(__FILE__,__LINE__);} while(0)\n')
 (out/'stdio.h').write_text('#pragma once\nint printf(const char *,...);\n')
 (out/'support.c').write_text('_Noreturn void platform_panic(const char *file,unsigned line){(void)file;(void)line;for(;;)__asm__ volatile("brk #0");}\n')
 for name,src in [('fixture',ROOT/'checks/hdmi-framebuffer.c'),('support',out/'support.c')]:
  obj=out/(name+'.o');objects.append(obj)
  run(name+'-compile',[a.clang.absolute(),*flags,'-I'+str(out),'-Wno-unused-function','-MD','-MF',out/(name+'.d'),'-c',src,'-o',obj])
 # Reuse already-built native objects. A compact fixture link supplies the same
 # real fixed NC pool geometry without booting or replaying eight-core startup.
 (out/'link.ld').write_text('ENTRY(framebuffer_check_run)\nSECTIONS { . = 0x41000000; .text : { *(.text*) } .rodata : { *(.rodata*) } .data : { *(.data*) } .bss (NOLOAD) : { *(.bss*) *(COMMON) } __device_pool_end = .; __uncached_pool_start = 0x41c00000; __uncached_pool_end = 0x41e00000; ASSERT(__device_pool_end < 0x41c00000,"fixture overlap") }\n')
 exe=out/'composition.elf';run('composition-link',[a.lld.absolute(),'--gc-sections','-T',out/'link.ld',*objects,'-o',exe])
 inputs={}
 def keep(path):
  path=path.resolve();data=path.read_bytes();digest=hashlib.sha256(data).hexdigest()
  if str(path) in inputs:assert inputs[str(path)]['sha256']==digest;return
  dest=out/'inputs'/str(path).lstrip('/');dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(data)
  inputs[str(path)]=dict(sha256=digest,snapshot=str(dest))
 for d in [*build.rglob('*.o.d'),*build.rglob('*.obj.d'),*out.glob('*.d')]:
  for name in shlex.split(d.read_text().replace('\\\n',' ').split(':',1)[1]):
   path=Path(name);keep(path if path.is_absolute() else build/path)
 for path in [Path(__file__),ROOT/'apps/hdmi/prepare.py',ROOT/'sources.lock.json',toolchain,out/'link.ld',a.clang,a.lld,a.objcopy,Path(sys.executable).resolve()]:keep(path)
 venv=Path(sys.executable).absolute().parent.parent/'pyvenv.cfg'
 if venv.exists():keep(venv)
 for package in (unicorn,elftools):
  for path in Path(package.__file__).parent.rglob('*'):
   if path.is_file() and (path.suffix in ('.py','.so') or '.so.' in path.name):keep(path)
 # Existing source lock, not a second independent driver implementation.
 lock=json.loads((ROOT/'sources.lock.json').read_text())
 refs=['display-de-de_top-c','display-de-de_channel-c','display-de-de_channel-h',
  'board-gpu-power-pck600-domains-c','board-video-platform-ccu-reset-c']
 refs += [k for k,v in lock.items() if isinstance(v,dict) and v.get('archive')=='display/arch/arm64/boot/dts/allwinner/sun60iw2p1.dtsi']
 source_text={}
 for name in refs:
  entry=lock[name];path=a.sources/entry['archive'];keep(path)
  expected=entry.get('sha256');assert expected and sha(path)==expected
  source_text[name]=path.read_text()
 assert 'DE_FORMAT_RGB_565 = 0x0a' in source_text['display-de-de_channel-h']
 assert 'return DE_FORMAT_RGB_565;' in source_text['display-de-de_channel-c']
 dts=next(t for k,t in source_text.items() if 'disp1_out_tcon3' in t)
 assert 'reg = <4>;' in dts[dts.index('disp1_out_tcon3:'):dts.index('disp1_out_tcon4:')]
 save(out/'source-comparison.json',dict(format=10,tv0_mux=4,iommu_master=8,method='Pinned published format/endpoint definitions; composed MMIO expectations below'))
 save(out/'inputs.json',inputs)
 artifacts={str(x):sha(x) for x in [image,exe,*objects,build/'hdmi-application/image.map',build/'hdmi-application/build-id.txt']};save(out/'artifacts.json',artifacts)
 # Offline geometry and packaging snapshot checks share the actual built ELF.
 spec=importlib.util.spec_from_file_location('hdmi_pack',ROOT/'apps/hdmi/prepare.py');pack=importlib.util.module_from_spec(spec);spec.loader.exec_module(pack)
 raw=image.read_bytes();parts,address,identity,size=pack.image_data(raw);mode,offset=pack.framebuffer_metadata(raw)
 assert mode==1 and offset+104==size;pack.geometry(parts,True)
 try:pack.geometry([(0x41eff000,8192,b'x'*8192)],True)
 except ValueError:pass
 else:raise AssertionError('overlapping session geometry accepted')
 packdir=out/'pack-input';packdir.mkdir();(packdir/'image.elf').write_bytes(raw)
 session=bytearray(size);struct.pack_into('<6Q',session,0,pack.SESSION_MAGIC,1,size,1,2,3)
 session[48:80]=identity;struct.pack_into('<2Q',session,208,4,5);struct.pack_into('<I',session,300,24000000)
 record=dict(clock_profile=dict(raw_dcxo_hz=24000000, reference_hz=24000000, sfr_source=0, sfr_hz=24000000, pixel_hz={'720p60':74250000,'1080p60':148500000}), schema='a733-hdmi-supervised-admission-v1',image_sha256=sha(image),epoch=1,generation=2,lease=3,supervisor=4,
  framebuffer='720p60-rgb565-retained-v1',framebuffer_authority=6)
 for name,position in [('analog',112),('power',144),('supervision',176),('isolation',offset),('memory',offset+32)]:
  data=('Modeled '+name+' evidence, not physical admission').encode();(packdir/name).write_bytes(data)
  record[name]=name;session[position:position+32]=hashlib.sha256(data).digest()
 struct.pack_into('<Q',session,offset+64,6);save(packdir/'admission.json',record)
 session[80:112]=hashlib.sha256((packdir/'admission.json').read_bytes()).digest();(packdir/'session.bin').write_bytes(session)
 accepted=pack.validate(packdir/'image.elf',packdir/'session.bin',packdir/'admission.json')
 assert accepted[-1]['image.elf']==raw and accepted[-1]['memory-record']==(packdir/'memory').read_bytes()
 (packdir/'memory').write_bytes(b'changed physical record')
 try:pack.validate(packdir/'image.elf',packdir/'session.bin',packdir/'admission.json')
 except ValueError:pass
 else:raise AssertionError('changed memory record admitted')
 save(out/'geometry.json',dict(passed=True,record_mutation_refused=True,framebuffer_offset=offset,session_bytes=size,reservations=[(b,n) for b,n,_ in parts],physical=False))
 # Actual compiled native composition, with modeled hardware and a reused
 # prepared-HDMI checkpoint. There is one nominal path and four saved-state
 # fault continuations. No automatic retry on failure or instruction cap.
 with exe.open('rb') as f:
  elf=ELFFile(f);segments=[(int(s['p_vaddr']),bytes(s.data())) for s in elf.iter_segments() if s['p_type']=='PT_LOAD'];sym={s.name:int(s['st_value']) for s in elf.get_section_by_name('.symtab').iter_symbols()}
 uc=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
 regions=[(0x41000000,0x1000000),(0x50000000,0x20000),(0x02002000,0x2000),(0x03000000,0x1000),(0x03900000,0x20000),(0x05000000,0x400000),(0x05510000,0x10000),(0x05520000,0x8000),(0x05600000,0x1000),(0x05730000,0x1000),(0x07010000,0x1000),(0x07061000,0x1000),(0x0706a000,0x1000),(0x07090000,0x1000)]
 for b,n in regions:uc.mem_map(b,n)
 for b,data in segments:uc.mem_write(b,data)
 def put(a,v,n=4):uc.mem_write(a,v.to_bytes(n,'little'))
 def get(a,n=4):return int.from_bytes(uc.mem_read(a,n),'little')
 put(0x07090160,0x10000002);put(0x070101ac,1)
 put(0x02002000,0xf8000000|(99<<16)|(99<<8))
 put(0x020022e0,0xf4000000|(3<<16)|(99<<8));put(0x02003928,0x20002);put(0x020039a8,0x20000)
 put(0x020025c0,1<<5);put(0x020025e0,1<<11)
 put(0x02002580,0x80000000);put(0x02002588,0x80000000);put(0x020025e4,0x40302)
 for a in (0x0200258c,0x020025b4):put(a,0x10007)
 state=dict(events=[],fault=0);checkpoints={};panic=[];which='nominal'
 uc.cpr_write(3,3,14,0,0,24000000);uc.reg_write(UC_ARM64_REG_PSTATE,0x345)
 uc.reg_write(UC_ARM64_REG_SP,0x5001f000);uc.reg_write(UC_ARM64_REG_LR,0x5001e000)
 def instruction(u,a,n,v):
  if a==0x41003ddc: # Reviewed return from native tokens() in lock().
   word=get(a-4);imm=word&0x3ffffff
   if imm&(1<<25):imm-=1<<26
   assert word>>26==0x25 and a-4+4*imm==sym['tokens']
   state['native']=u.reg_read(UC_ARM64_REG_X0+19)
   state['tokens_valid']=u.reg_read(UC_ARM64_REG_X0)==1
   return
  if a==sym['platform_panic']:
   panic.append(dict(file=bytes(u.mem_read(u.reg_read(UC_ARM64_REG_X0),256)).split(b'\0')[0].decode(errors='replace'),line=u.reg_read(UC_ARM64_REG_X1)));u.emu_stop();return
  if a==sym['framebuffer_checkpoint']:
   # Clang omits the unused C parameter. These reviewed call sites belong to
   # this frozen fixture ELF; validate the actual BL target before admission.
   calls={0x41014b28:1,0x41014b58:4,0x41014b84:2,0x41014cbc:3}
   lr=u.reg_read(UC_ARM64_REG_LR);assert lr in calls
   word=get(lr-4);imm=word&0x3ffffff
   if imm&(1<<25):imm-=1<<26
   assert word>>26==0x25 and lr-4+4*imm==sym['framebuffer_checkpoint']
   cp=calls[lr]
   if which=='nominal' and cp not in checkpoints:
    checkpoints[cp]=(u.context_save(),[(b,bytes(u.mem_read(b,n))) for b,n in regions],copy.deepcopy(state))
    (out/f'checkpoint-{cp}.pickle').write_bytes(pickle.dumps(checkpoints[cp]))
   return
  word=get(a);op=word&0xffffffe0;rt=word&31
  values={0xd5384240:4,0xd5381000:5,0xd53b0020:(4<<24)|(4<<16),0xd53be020:get(sym['framebuffer_test_time'],8)*24}
  if op in values:
   assert rt<29;u.reg_write(UC_ARM64_REG_X0+rt,values[op]);u.reg_write(UC_ARM64_REG_PC,a+4)
  elif op in (0xd5087800,0xd5087820):
   address=u.reg_read(UC_ARM64_REG_X0+rt);nc=0x41c00000<=address<0x41e00000
   u.cpr_write(3,0,7,4,0,(address&~4095)|(0x44<<56 if nc else 0)|0x100);u.reg_write(UC_ARM64_REG_PC,a+4)
 def read(u,access,a,n,value,v):
  if a in (0x07061008,0x0706a008):put(a,get(a-8)&15)
  elif a==0x05600040:put(a,1)
  elif a==0x05523004:put(a,3,1)
  elif a==0x05008140 and state['fault']==3 and state.get('de_disabled'):put(a,1)
 def write(u,access,a,n,value,v):
  if a==0x020025c0:
   native=state['native']
   assert state['tokens_valid'] and get(sym['holders'],8)==1 and get(native+0x800)==1
   assert get(native+0x668,8)==11 and get(native+0x670,16)==0x4001 and get(native+0x680,16)==1<<63
   assert (get(a)^value)==1<<4
   state['vo1_gate_authorized']=True
  state['events'].append((a,n,value,get(sym['framebuffer_test_time'],8)))
  if a==0x05008140 and value==0:state['de_disabled']=True
 uc.hook_add(UC_HOOK_MEM_READ,read,begin=0x02000000,end=0x08000000);uc.hook_add(UC_HOOK_MEM_WRITE,write,begin=0x02000000,end=0x08000000)
 hooks={sym['platform_panic'],sym['framebuffer_checkpoint'],0x41003ddc}
 for b,data in segments:
  for i in range(0,len(data)-3,4):
   if struct.unpack_from('<I',data,i)[0]&0xffffffe0 in (0xd5384240,0xd5381000,0xd53b0020,0xd53be020,0xd5087800,0xd5087820):hooks.add(b+i)
 for address in hooks:uc.hook_add(UC_HOOK_CODE,instruction,begin=address,end=address)
 results=[];began=time.monotonic()
 def execute(label,start,count):
  panic.clear();remaining=120-(time.monotonic()-began);assert remaining>0
  error=None
  try:uc.emu_start(start,0x5001e000,count=count,timeout=int(remaining*1000000))
  except Exception as e:error=str(e)
  passed=not error and not panic and uc.reg_read(UC_ARM64_REG_PC)==0x5001e000 and get(sym['framebuffer_test_result'])==1
  result=dict(case=label,passed=passed,error=error,panic=list(panic),pc=hex(uc.reg_read(UC_ARM64_REG_PC)),instruction_limit=count,wall_seconds=time.monotonic()-began)
  results.append(result);save(out/'results.json',results);print(json.dumps(result),flush=True)
  if not passed:
   for i,(b,n) in enumerate(regions):(out/f'failed-memory-{i}.bin').write_bytes(bytes(uc.mem_read(b,n)))
   save(out/'failed-state.json',dict(pc=uc.reg_read(UC_ARM64_REG_PC),pstate=uc.reg_read(UC_ARM64_REG_PSTATE),events=state,elf_sha256=sha(exe)))
   raise SystemExit(1)
 execute('frame-allocation-translation-DE-TV0-output-stop',sym['framebuffer_check_run'],100000000)
 save(out/'nominal-trace.json',state)
 events=state['events'];start=next(i for i,e in enumerate(events) if e[0]==0x05008140 and e[2]==1)
 tv=next(i for i,e in enumerate(events) if e[0]==0x05730090 and e[2]&0x80000000)
 stop=max(i for i,e in enumerate(events) if e[0]==0x05730090 and not e[2]&0x80000000)
 de_stop=max(i for i,e in enumerate(events) if e[0]==0x05008140 and e[2]==0)
 assert start<tv<stop<de_stop
 assert state['vo1_gate_authorized']
 assert [(e[0],e[2]) for e in events if e[0] in (0x020025c0,0x020025e0)]==[(0x020025c0,0x30)]
 assert not any(e[0] in (0x020025c0,0x020025e0) for e in events[len(checkpoints[1][2]['events']):])
 assert not any(0x03900400<=e[0]<0x03901000 or 0x03910400<=e[0]<0x03910800 or 0x03910a00<=e[0]<0x03911000 for e in events)
 for mode,cp,label in [(1,1,'initial-authority-mutation-retains-parent'),(2,2,'registry-fault-during-lock-before-TV-enable'),(3,3,'DE-stop-readback-uncertain-retains-parent'),(4,4,'cancel-pending-DE-start-retains-without-enabling')]:
  which=label;context,memory,old=checkpoints[cp]
  for b,data in memory:uc.mem_write(b,data)
  uc.context_restore(context);state=copy.deepcopy(old);state['fault']=mode;put(sym['framebuffer_fault_case'],mode)
  before_events=len(state['events'])
  execute(label,uc.reg_read(UC_ARM64_REG_PC),10000000)
  if mode==4:assert not any(0x05000000<=e[0]<0x05400000 for e in state['events'][before_events:])
 assert all(sha(Path(p))==v['sha256'] for p,v in inputs.items())
 save(out/'completed.json',dict(passed=True,nominal_runs=1,checkpoint_faults=4,physical=False,image_sha256=sha(image),composition_sha256=sha(exe),total_instruction_limit=140000000,total_wall_limit_seconds=120))
if __name__=='__main__':main()
