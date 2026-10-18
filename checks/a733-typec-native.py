#!/usr/bin/env python3
"""Native Type-C composition with modeled hardware and saved fault checkpoints."""
import argparse, copy, gzip, hashlib, json, shutil, subprocess, sys, time
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_LR, UC_ARM64_REG_FP, UC_ARM64_REG_PSTATE

parser=argparse.ArgumentParser()
parser.add_argument('--repository',type=Path,default=Path(__file__).resolve().parents[1])
parser.add_argument('--output',type=Path,required=True)
parser.add_argument('--mode',choices=['all','build','execute'],default='all')
args=parser.parse_args();repo=args.repository.resolve();out=args.output.resolve()
cache=Path.home()/'.cache/sunstoneos';source=cache/'sources'
compiler=Path('/usr/lib/llvm-21/bin/clang').resolve()
linker=cache/'host/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-elf/bin/aarch64-none-elf-ld'
fr=source/'FreeRTOS-Kernel-97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2'
smp=cache/'evidence/clock-width-20261016/build/freertos-smp'
elfpath=out/'typec-native.elf';inputs={};commands=[]
def keep(p):
 p=p.resolve();data=p.read_bytes();digest=hashlib.sha256(data).hexdigest()
 if str(p) in inputs:
  assert inputs[str(p)]['sha256']==digest;return
 dest=out/'inputs'/str(p).lstrip('/');dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(data)
 inputs[str(p)]={'sha256':digest,'snapshot':str(dest)}
def run(cmd,name):
 r=subprocess.run([str(x) for x in cmd],cwd=repo,capture_output=True,text=True)
 commands.append({'name':name,'command':[str(x) for x in cmd],'returncode':r.returncode})
 (out/(name+'.stdout')).write_text(r.stdout);(out/(name+'.stderr')).write_text(r.stderr)
 (out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
 if r.returncode:raise SystemExit(name+': '+r.stderr[-5000:])
def dependencies(dep):
 for f in dep.read_text().replace('\\\n',' ').split(':',1)[1].split():keep(Path(f) if Path(f).is_absolute() else repo/f)
 (out/'inputs.json').write_text(json.dumps(inputs,indent=2)+'\n')
if args.mode!='execute':
 out.mkdir(parents=True,exist_ok=False)
 (out/'assert.h').write_text('#pragma once\n_Noreturn void platform_panic(const char *,unsigned);\n#define assert(x) do {if(!(x))platform_panic(__FILE__,__LINE__);} while(0)\n')
 (out/'support.c').write_text('_Noreturn void platform_panic(const char *file,unsigned line){(void)file;(void)line;for(;;)__asm__ volatile("brk #0");}\n')
 (out/'link.ld').write_text('ENTRY(typec_native_check)\nSECTIONS { . = 0x40000000; .text : { *(.text*) } .rodata : { *(.rodata*) } .data : { *(.data*) } .bss : { *(.bss*) *(COMMON) } }\n')
 units=['checks/a733-typec.c','drivers/usb/a733_typec.c','platform/a733/typec_native.c','drivers/i2c/a733_twi.c',
  'drivers/clock/a733_clock_leases.c','drivers/power/a733_rail_leases.c','drivers/power/a733_pmic.c','drivers/power/a733_pmic_native.c',
  'drivers/pinctrl/a733_pins.c','drivers/pinctrl/a733_pin_tables.c','drivers/pinctrl/a733_pins_native.c','drivers/gpu/a733_gpu_resources.c',
  'platform/a733/counter_clock.c','arch/aarch64/device_memory.c','platform/a733/shared_locks.c','runtime/memory.c']
 for p in [Path(__file__),compiler,linker,Path(sys.executable).resolve(),*(repo/u for u in units),out/'assert.h',out/'support.c',out/'link.ld']:keep(p)
 references=['typec-host/fusb302-primary-web-extract.json','usb2-electrical/sy6280-rev1.0e.pdf','usb2-electrical/usb-typec-r2.0.pdf',
  'board-documents/schematic-v1.2.pdf','camera-research/A733_User_Manual_V1.00.pdf',
  'usb2-research/drivers/usb/typec/tcpm/fusb302.c','usb2-research/drivers/usb/typec/tcpm/fusb302_reg.h']
 for p in references:keep(source/p)
 flags=['--target=aarch64-none-elf','-std=c11','-march=armv8-a','-mgeneral-regs-only','-mstrict-align','-mno-outline-atomics',
  '-ffreestanding','-fno-builtin','-fno-stack-protector','-ffunction-sections','-fdata-sections','-fstack-usage','-O2','-g','-Wall','-Wextra','-Werror',
  '-DINCLUDE_xSemaphoreGetMutexHolder=1','-DINCLUDE_xTaskGetCurrentTaskHandle=1','-DINCLUDE_xTaskGetSchedulerState=1']
 incs=[out,repo/'include',repo/'runtime/include',fr/'include',fr/'portable/GCC/ARM_AARCH64_SRE']
 objects=[]
 for i,p in enumerate([repo/u for u in units]+[out/'support.c']):
  obj=out/(str(i)+'-'+p.stem+'.o');dep=obj.with_suffix('.d');objects.append(obj)
  run([compiler,*flags,*('-I'+str(x) for x in incs),'-MD','-MF',dep,'-c',p,'-o',obj],'compile-'+str(i));dependencies(dep)
 smp_dep=out/'typec-native-smp.d'
 run([compiler,*flags,*('-I'+str(x) for x in [out,repo/'platform/a733-smp-cached',repo/'include',repo/'runtime/include',fr/'include',smp]),
  '-MD','-MF',smp_dep,'-c',repo/'platform/a733/typec_native.c','-o',out/'typec-native-smp.o'],'compile-native-smp');dependencies(smp_dep)
 run([linker,'--gc-sections','-T',out/'link.ld',*objects,'-o',elfpath],'link')
 import unicorn,elftools
 venv_config=Path(sys.prefix)/'pyvenv.cfg'
 if venv_config.is_file():keep(venv_config)
 for package in [unicorn,elftools]:
  for p in Path(package.__file__).parent.rglob('*'):
   if p.is_file() and (p.suffix in ('.py','.so') or '.so.' in p.name):keep(p)
 (out/'inputs.json').write_text(json.dumps(inputs,indent=2)+'\n')
 (out/'artifacts.json').write_text(json.dumps({str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in [elfpath,*objects,out/'typec-native-smp.o']},indent=2)+'\n')
 if args.mode=='build':raise SystemExit(0)
else:
 inputs=json.loads((out/'inputs.json').read_text())
 assert all(hashlib.sha256(Path(p).read_bytes()).hexdigest()==v['sha256'] for p,v in inputs.items())
 assert hashlib.sha256(elfpath.read_bytes()).hexdigest()==json.loads((out/'artifacts.json').read_text())[str(elfpath)]
assert not (out/'results.json').exists(),'Preserve previous execution; do not replay it implicitly.'
with elfpath.open('rb') as f:
 elf=ELFFile(f);segments=[(s['p_vaddr'],s.data()) for s in elf.iter_segments() if s['p_type']=='PT_LOAD']
 symbols={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
u=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
regions=[(0x40000000,0x800000),(0x03400000,0x1000),(0x07010000,0x1000),(0x07025000,0x1000),
 (0x07083000,0x2000),(0x07090000,0x1000)]
for a,n in regions:u.mem_map(a,n)
for a,data in segments:u.mem_write(a,data)
def put(a,v,n=4):u.mem_write(a,int(v).to_bytes(n,'little'))
def get(a,n=4):return int.from_bytes(u.mem_read(a,n),'little')
def variable(name,n=4):return get(symbols[name],n)
def test_time():return variable('typec_test_time',8)
put(0x07090160,0x10000002);put(0x0701019c,0x10001)
put(0x07025000,0x22);put(0x07025014,0x11);put(0x07025024,5)
for base,control in [(0x07083000,0x40),(0x07084000,0)]:
 for off,v in [(0xc,control),(0x10,0xf8),(0x14,0x28),(0x20,0x30),(0x200,0xf81000),(0x210,0x80c0),(0x218,0x100010)]:put(base+off,v)
u.cpr_write(3,3,14,0,0,24000000);u.reg_write(UC_ARM64_REG_PSTATE,0x345)
u.reg_write(UC_ARM64_REG_SP,0x407ff000);u.reg_write(UC_ARM64_REG_LR,0x407fe000)
state={'bus':{0x07083000:{'wire':0},0x07084000:{'wire':0}},'fusb':{1:0x80,0xa:255,0xe:255,0xf:1},
 'pmic':{0xe:3,0x10:1,0x11:16,0x12:23},'interrupt':0,'status':0,'gpio':0,'events':[],
 'translations':0,'mmio':0,'delayed_stop':False,'vbus_low_samples':[],'debounce_reads':[]}
checkpoints={};mode='nominal';panic=[];results=[];start_wall=time.monotonic();instruction_allowance=0

def status():
 f=state['fusb'];sw=f.get(2,0);mdac=f.get(4,0);partner=variable('typec_partner_cc')
 on=bool(state['gpio']&256) or bool(variable('typec_external_vbus'))
 if mdac&0x40:voltage=5000 if on else 0;threshold=((mdac&63)+1)*420
 else:
  selected=1 if sw&4 else 2 if sw&8 else 0
  voltage=500 if selected and selected==partner else 3300 if selected else 0
  threshold=((mdac&63)+1)*42
 value=(0x80 if on else 0)|(0x20 if voltage>threshold else 0)
 state['interrupt']|=(state['status']^value)&0xa0;state['status']=value
 return value

def fusb_read(reg):
 value=status()
 if reg==0x3d:return (variable('typec_partner_cc')<<3) if state['fusb'].get(8,0)&1 else 0
 if reg==0x40:
  if state['fusb'].get(4,0)==0x40:state['vbus_low_samples'].append([test_time(),value,state['fusb'].get(2,0)])
  return value
 if reg==0x41:return 0
 if reg==0x42:
  v=state['interrupt'];state['interrupt']=0;state['debounce_reads'].append([test_time(),v]);return v
 if reg in (0x3e,0x3f):return 0
 return state['fusb'].get(reg,0)
def fusb_write(reg,value):
 status()
 if reg==0xc and value==1:state['fusb']={1:0x80,0xa:255,0xe:255,0xf:1};state['interrupt']=0
 else:state['fusb'][reg]=value
 status();state['events'].append(['fusb',reg,value,test_time()])

def code(uc,a,n,context):
 word=get(a);masked=word&0xffffffe0;rt=word&31
 values={0xd5384240:4,0xd5381000:5,0xd53b0020:(4<<24)|(4<<16)}
 if masked==0xd53be020:values[masked]=test_time()*24
 if masked in values:
  assert rt<29;uc.reg_write(UC_ARM64_REG_X0+rt,values[masked]);uc.reg_write(UC_ARM64_REG_PC,a+4)
 elif masked in (0xd5087800,0xd5087820):
  address=uc.reg_read(UC_ARM64_REG_X0+rt);uc.cpr_write(3,0,7,4,0,(address&~4095)|0x100)
  state['translations']+=1;put(symbols['typec_translation_count'],state['translations']);uc.reg_write(UC_ARM64_REG_PC,a+4)
def cpu_record():
 return {'pc':u.reg_read(UC_ARM64_REG_PC),'sp':u.reg_read(UC_ARM64_REG_SP),'pstate':u.reg_read(UC_ARM64_REG_PSTATE),
  'x':[u.reg_read(UC_ARM64_REG_X0+i) for i in range(29)],'lr':u.reg_read(UC_ARM64_REG_LR),'fp':u.reg_read(UC_ARM64_REG_FP),
  'system':{'cntfrq':u.cpr_read(3,3,14,0,0),'par_el1':u.cpr_read(3,0,7,4,0),
   'tpidr_el1':u.cpr_read(3,0,13,0,4),'sctlr_el1':u.cpr_read(3,0,1,0,0)}}
def archive_state(name,memory,hardware):
 dest=out/name;dest.mkdir(exist_ok=False)
 (dest/'cpu.json').write_text(json.dumps(cpu_record(),indent=2)+'\n')
 (dest/'hardware.json').write_text(json.dumps(hardware,indent=2)+'\n')
 for a,data in memory:
  with gzip.open(dest/(hex(a)+'.bin.gz'),'wb') as f:f.write(data)
def checkpoint(uc,a,n,context):
 which=uc.reg_read(UC_ARM64_REG_X0)
 if mode=='nominal' and which not in checkpoints:
  assert which in (1,2,3,4)
  memory=[(b,bytes(uc.mem_read(b,size))) for b,size in regions]
  checkpoints[which]=(uc.context_save(),memory,copy.deepcopy(state))
  archive_state('checkpoint-'+str(which),memory,state)
def failure(uc,a,n,context):
 text=bytes(uc.mem_read(uc.reg_read(UC_ARM64_REG_X0),512)).split(b'\0')[0].decode(errors='replace')
 panic.append({'file':text,'line':uc.reg_read(UC_ARM64_REG_X1)});uc.emu_stop()
def read(uc,access,a,n,value,context):
 if a>=0x40000000:return
 state['mmio']+=1;put(symbols['typec_mmio_count'],state['mmio'])
 if a==0x07025010:
  status();active=not(state['fusb'].get(6,0)&0x20) and bool(state['interrupt']&~state['fusb'].get(0xa,255))
  put(a,state['gpio']|(0 if active else 512));return
 base=a&~4095
 if base not in state['bus']:return
 if base==0x07084000:assert state['translations']>=8
 b=state['bus'][base];off=a-base
 if off in (8,0xc,0x10):
  if off==0xc and 'due' in b:
   if test_time()<b['due']:put(a,0x58 if b.get('stop') else 0x40);return
   if b.get('stop'):
    b.update(wire=0,control=0x40,status=0xf8,stop=False);b.pop('due')
  if off==8 and 'data' in b:put(a,b['data'])
  elif off==0xc and 'control' in b:put(a,b['control'])
  elif off==0x10 and 'status' in b:put(a,b['status'])
def write(uc,access,a,n,value,context):
 if a>=0x40000000:return
 state['mmio']+=1;put(symbols['typec_mmio_count'],state['mmio'])
 state['events'].append(['mmio',a,value,test_time()])
 if a==0x07025010:
  old=state['gpio'];state['gpio']=value
  if value&256 and not old&256:
   assert not variable('typec_external_vbus')
   assert state['vbus_low_samples'] and state['vbus_low_samples'][-1][1]&0xa0==0
   assert state['vbus_low_samples'][-1][2]&12==0
  status();return
 base=a&~4095
 if base not in state['bus']:return
 b=state['bus'][base];off=a-base
 if off==8:b['data']=value;return
 if off!=0xc or not value&8:return
 wire=b['wire'];data=b.get('data',0)
 if value==0x68:
  assert wire in (0,3);b['status']=8 if wire==0 else 0x10;b['wire']=wire+1
 elif value==0x48:
  if wire==1:assert data==(0x6c if base==0x07083000 else 0x44);b['status']=0x18
  elif wire==2:b['selected']=data;b['status']=0x28
  elif wire==3:
   assert base==0x07084000;fusb_write(b['selected'],data);b['status']=0x28
  elif wire==4:assert data==(0x6d if base==0x07083000 else 0x45);b['status']=0x40
  elif wire==5:b['data']=state['pmic'].get(b['selected'],0) if base==0x07083000 else fusb_read(b['selected']);b['status']=0x58
  else:raise AssertionError(('wire',base,wire,data))
  b['wire']=wire+1
 elif value==0x58:
  assert wire in (4,6)
  delay=250 if base==0x07084000 and b['selected']==0x40 and variable('typec_delay_stop') else 25
  if delay==250:put(symbols['typec_delay_stop'],0);state['delayed_stop']=True
  b.update(control=0x58,stop=True,due=test_time()+delay);return
 else:raise AssertionError(('control',value))
 if base==0x07084000 and variable('typec_wire_fault'):b['status']=0x20
 b['control']=0x48;b['due']=test_time()+90
# Only special instructions need callbacks; ordinary instructions execute natively.
for base,data in segments:
 for i in range(0,len(data)-3,4):
  word=int.from_bytes(data[i:i+4],'little')&0xffffffe0
  if word in (0xd5384240,0xd5381000,0xd53b0020,0xd53be020,0xd5087800,0xd5087820):u.hook_add(UC_HOOK_CODE,code,begin=base+i,end=base+i)
u.hook_add(UC_HOOK_CODE,checkpoint,begin=symbols['typec_checkpoint'],end=symbols['typec_checkpoint'])
u.hook_add(UC_HOOK_CODE,failure,begin=symbols['platform_panic'],end=symbols['platform_panic'])
for base,size in regions[1:]:
 u.hook_add(UC_HOOK_MEM_READ,read,begin=base,end=base+size-1)
 u.hook_add(UC_HOOK_MEM_WRITE,write,begin=base,end=base+size-1)
def execute(name,start):
 global panic,instruction_allowance
 panic=[];error=None;u.reg_write(UC_ARM64_REG_PC,start)
 for chunk in range(35):
  if instruction_allowance>=600000000 or time.monotonic()-start_wall>=600:break
  try:u.emu_start(u.reg_read(UC_ARM64_REG_PC),0x407fe000,timeout=max(1,int(min(10,600-(time.monotonic()-start_wall))*1000000)),count=10000000)
  except Exception as exc:error=str(exc)
  instruction_allowance+=10000000
  print(json.dumps({'case':name,'chunk':chunk,'pc':hex(u.reg_read(UC_ARM64_REG_PC)),'modeled_us':test_time(),'mmio':state['mmio']}),flush=True)
  if panic or error or u.reg_read(UC_ARM64_REG_PC)==0x407fe000:break
 passed=not panic and not error and u.reg_read(UC_ARM64_REG_PC)==0x407fe000 and u.reg_read(UC_ARM64_REG_X0)==0
 result={'name':name,'passed':passed,'panic':panic,'error':error,'cpu':cpu_record(),'modeled_us':test_time(),'mmio':state['mmio']}
 results.append(result);(out/'results.json').write_text(json.dumps({'elf_sha256':hashlib.sha256(elfpath.read_bytes()).hexdigest(),'cases':results},indent=2)+'\n')
 if not passed:
  archive_state('stopped-'+name,[(b,bytes(u.mem_read(b,size))) for b,size in regions],state)
  raise SystemExit('Stopped without retry: inspect preserved CPU, hardware and memory state.')
execute('attach-cc1-delayed-stop-detach',symbols['typec_native_check'])
assert state['delayed_stop']
(out/'nominal-trace.json').write_text(json.dumps(state,indent=2)+'\n')
# Source-derived expectations applied to the actual native trace, independently
# of controller state fields. No additional device execution is required.
events=state['events']
enabled=next(t for kind,a,v,t in events if kind=='mmio' and a==0x07025010 and v&256)
assert enabled-state['debounce_reads'][0][0]>=120000
assert [v for kind,a,v,t in events if kind=='mmio' and a==0x0701019c]==[0x30001,0x30003]
divider=[v for kind,a,v,t in events if kind=='mmio' and a==0x07084014]
assert len(divider)==1 and 24000000==100000*(1<<(divider[0]&7))*(((divider[0]>>3)&15)+1)*10
assert all(not(v&0x30) for kind,a,v,t in events if kind=='fusb' and a==2)
assert [v for kind,a,v,t in events if kind=='fusb' and a==3]==[0xb0]

assert all(which in checkpoints for which in (1,2,3,4))
for fault,point,name in [(1,1,'attach-cc2-detach'),(2,2,'external-vbus-refusal'),(3,3,'retained-i2c-independent-power-off'),
 (4,3,'provider-stall-power-off'),(5,3,'invalid-host-token'),(6,3,'registry-busy-pending-shutdown'),(7,4,'pre-pin-authority-loss')]:
 mode=name;context,memory,hardware=checkpoints[point]
 for a,data in memory:u.mem_write(a,data)
 u.context_restore(context);state=copy.deepcopy(hardware);put(symbols['typec_fault_case'],fault)
 execute(name,u.reg_read(UC_ARM64_REG_PC))
assert all(hashlib.sha256(Path(p).read_bytes()).hexdigest()==v['sha256'] for p,v in inputs.items())
(out/'completed.json').write_text(json.dumps({'result':'pass','nominal_runs':1,'checkpoint_continuations':7,'physical':False,
 'inputs':len(inputs),'instruction_allowance_upper_bound':instruction_allowance,'scope':'actual native providers; modeled MMIO, scheduler and electrical qualification'},indent=2)+'\n')
