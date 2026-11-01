#!/usr/bin/env python3
"""Execute the sustained client in a host-only ELF with modeled job and IRQ replies.

No monitor, launcher or board binary is linked. Device outputs and interrupts
are modeled. This verifies client control flow and report parsing, not hardware.
"""
import argparse,hashlib,importlib.util,json,shlex,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'checks'))
from harness import Image,LLD,environment,refused,hook_addresses,instruction_sites,system_register_access
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--out',type=Path,required=True)
p.add_argument('--baseline',type=Path,default=Path.home()/'.cache/sunstoneos/build/npu-wdt-1031/images/runtime')
p.add_argument('--before-tree',type=Path,help='unchanged checkout whose parser must refuse the actual sustained log')
a=p.parse_args();a.out.mkdir(parents=True,exist_ok=False)
commands=json.loads((a.baseline/'commands.json').read_text())
base=next(c for c in commands if any(str(v).endswith('/apps/npu-finite/runtime-client.c') for v in c))
old_root=next(v[:-len('/apps/npu-finite/runtime-client.c')] for v in base if v.endswith('/apps/npu-finite/runtime-client.c'))
flags=[];skip=False
for value in base[1:]:
 if skip:skip=False;continue
 if value in ('-o','-c','-MF'):skip=True;continue
 if value=='-MD':continue
 flags.append(value.replace(old_root,str(ROOT)))
flags+=['-fstandalone-debug','-Werror','-DSUNSTONE_NPU_SUSTAINED=1']
compiled=[]
def compile_source(path,name):
 dest=a.out/(name+'.o');cmd=[base[0],*flags,'-MD','-MF',str(dest)+'.d','-c',str(path),'-o',str(dest)]
 subprocess.run(cmd,check=True,env=environment());compiled.append(cmd);return dest
client=compile_source(ROOT/'apps/npu-finite/runtime-client.c','client')
digest=compile_source(ROOT/'drivers/storage/storage_digest.c','digest')
receipt_writer=compile_source(ROOT/'apps/npu-finite/return.c','receipt-writer')
net=a.out/'networks.c';net.write_text((a.baseline/'generated/networks.c').read_text().replace(old_root,str(ROOT)))
networks=compile_source(net,'networks')
# All unresolved callbacks are intercepted below. No physical provider exists.
undefined=set()
for obj in (client,digest,networks,receipt_writer):
 for line in subprocess.check_output(['nm','-u',str(obj)],text=True).splitlines():undefined.add(line.split()[-1])
defined=set()
for obj in (client,digest,networks,receipt_writer):
 for line in subprocess.check_output(['nm','--defined-only',str(obj)],text=True).splitlines():
  if len(line.split())>=3:defined.add(line.split()[-1])
undefined-=defined
undefined-={'__npu_clean_start','__device_pool_end','__npu_platform_log'}
scalars={'a733_npu_application_failed':8,'a733_observe_identity':128,'a733_npu_application_model':4096,'a733_npu_application_input':4096}
stub=a.out/'callbacks.S';stub.write_text('.text\n'+''.join(f'.global {n}\n{n}: ret\n' for n in sorted(undefined-scalars.keys()))+'\n.bss\n'+''.join(f'.balign 16\n.global {n}\n{n}: .space {size}\n' for n,size in scalars.items() if n in undefined))
stubs=compile_source(stub,'callbacks')
# A typed definition retains the observer layout for DWARF-based access.
layout_source=a.out/'layout.c';layout_source.write_text('#include "result.h"\nstruct a733_observe_identity fixture_identity_layout;\n')
layout=compile_source(layout_source,'layout')
image=a.out/'client-host-only.elf'
link=[str(LLD),'--image-base=0x41100000','-Ttext=0x41200000','-e','a733_smp_diagnostic_start','--defsym=__npu_clean_start=0x41200000','--defsym=__device_pool_end=0x41f00000','--defsym=__npu_platform_log=0x42000000',str(client),str(digest),str(networks),str(stubs),str(layout),str(receipt_writer),'-o',str(image)]
subprocess.run(link,check=True,env=environment());compiled.append(link)
(a.out/'commands.json').write_text(json.dumps(compiled,indent=2)+'\n')
im=Image(image,layout_names=('npu_runtime','npu_runtime_slot','npu_runtime_counters','npu_service','a733_npu_controller','a733_observe_identity','npu_finite_network'))
L=im.layouts;enums={}
with image.open('rb') as f:
 elf=ELFFile(f);sizes={s.name:int(s['st_size']) for s in elf.get_section_by_name('.symtab').iter_symbols()}
 for cu in elf.get_dwarf_info().iter_CUs():
  for die in cu.iter_DIEs():
   if die.tag=='DW_TAG_enumerator':enums[die.attributes['DW_AT_name'].value.decode()]=int(die.attributes['DW_AT_const_value'].value)
def load_host(root,name):
 spec=importlib.util.spec_from_file_location(name,root/'apps/npu-interrupt/host.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
host=load_host(ROOT,'sustained_host');h=host.finite
rows=[];logs={};references=None
variants=('matched','first-output-mismatch','middle-output-mismatch','last-output-mismatch','last-IRQ-missing','last-counter-missing','middle-timeout','global-timeout','close-failed','slow-helper','wrong-counter-frequency')
for variant in variants:
 u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0x40000000,0x04000000)
 for at,data in im.parts:
  if data:u.mem_write(at,data)
 def put(at,v,n=8):u.mem_write(at,int(v).to_bytes(n,'little'))
 def get(at,n=8):return int.from_bytes(u.mem_read(at,n),'little')
 def ret(v=0):u.reg_write(UC_ARM64_REG_X0,v);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
 rt=0x43000000;service=rt+L['npu_runtime']['service'];slot=rt+L['npu_runtime']['slot'];controller=service+L['npu_service']['controller']
 for field,value in [('epoch',7),('generation',8),('lease',9),('deadline',480000000)]:put(im.address('a733_observe_identity')+L['a733_observe_identity'][field],value)
 refs=[bytes(u.mem_read(im.address(f'npu_finite_reference_{n}'),sizes[f'npu_finite_reference_{n}'])) for n in range(2)]
 references=[r.hex() for r in refs]
 assert [get(im.address('npu_startup_return_profile')+i*8) for i in range(4)]==[h.STARTUP_CLIENT[h.RUNTIME][0],1,200,201]
 assert [get(im.address('npu_sustained_return_profile')+i*8) for i in range(4)]==[0x4e50555355533031,1,200,2]
 state={'ticks':24000000,'models':[],'exit':None,'retained':False,'closed':False,'log':'','close_calls':0,'native_receipt':None,'compares':0,'retained_reason':None}
 names=undefined|{'storage_digest_bytes','storage_digest_add','storage_digest_end','storage_digest_init'}
 hooks={im.address(n):n for n in names if n in im.symbols}
 def code(cpu,at,size,cookie):
  word=get(at,4)
  if word&0xffe0001f==0xd4000001:
   assert (word>>5)&0xffff in (0x5c0d,0x5c0e);u.reg_write(UC_ARM64_REG_PC,at+4);return
  if word&0xffffffe0==0xd53be020:
   u.reg_write(UC_ARM64_REG_X0+(word&31),state['ticks']);state['ticks']+=24;u.reg_write(UC_ARM64_REG_PC,at+4);return
  if word&0xffffffe0==0xd53be000:
   u.reg_write(UC_ARM64_REG_X0+(word&31),12000000 if variant=='wrong-counter-frequency' else 24000000);u.reg_write(UC_ARM64_REG_PC,at+4);return
  if system_register_access(word):u.reg_write(UC_ARM64_REG_PC,at+4);return
  name=hooks.get(at)
  if name is None:return
  args=[u.reg_read(UC_ARM64_REG_X0+i) for i in range(6)]
  if name.startswith('storage_digest_'):return # Execute the linked SHA256 implementation.
  if name=='a733_smp_counter':ret(state['ticks']);state['ticks']+=24
  elif name=='a733_npu_application_ready':ret(1)
  elif name=='a733_npu_application_runtime':ret(rt)
  elif name=='npu_job_run':
   job=len(state['models']);n=job%2;assert args[0]==n and not state['close_calls'] and job<200
   network=im.address('npu_finite_networks')+n*48;fields=L['npu_finite_network']
   assert args[1:3]==[get(network+fields['input']),get(network+fields['input_bytes'])]
   assert args[4]==get(network+fields['reference_bytes']) and args[5]==1000000
   state['models'].append(n);state['ticks']+=2400+24*job
   if variant=='middle-timeout' and job==100:ret((1<<64)-1);return
   if variant=='global-timeout' and job==100:state['ticks']=480000000
   if variant=='slow-helper' and job==100:state['ticks']+=24000000
   output=refs[n]
   bad={'first-output-mismatch':0,'middle-output-mismatch':100,'last-output-mismatch':199}.get(variant)
   if job==bad:output=bytes([output[0]^1])+output[1:]
   u.mem_write(args[3],output)
   put(rt+L['npu_runtime']['counters']+L['npu_runtime_counters']['jobs'],job+int(not(variant=='last-counter-missing' and job==199)))
   put(service+L['npu_service']['irq_completions'],1+job+int(not(variant=='last-IRQ-missing' and job==199)),4);ret(1)
  elif name=='npu_job_close':
   state['close_calls']+=1;assert len(state['models'])==200
   if variant=='close-failed':ret(0);return
   state['closed']=True
   for field,value in [('opened',1),('closed',1),('state',enums['NPU_RUNTIME_CLOSED'])]:put(slot+L['npu_runtime_slot'][field],value,4)
   put(controller+L['a733_npu_controller']['state'],enums['A733_NPU_CLOSED'],4)
   put(service+L['npu_service']['irq_stopped'],1,4);ret(1)
  elif name=='npu_service_irq_completions':ret(get(service+L['npu_service']['irq_completions'],4))
  elif name=='npu_service_irq_stopped':ret(int(state['closed']))
  elif name in ('a733_npu_application_stopped','a733_npu_application_quiesce','a733_npu_application_quiescent'):ret(int(state['closed']))
  elif name=='memset':u.mem_write(args[0],bytes([args[1]&255])*args[2]);ret(args[0])
  elif name=='memcpy':u.mem_write(args[0],bytes(u.mem_read(args[1],args[2])));ret(args[0])
  elif name=='memcmp':
   state['compares']+=1;ret(int(bytes(u.mem_read(args[0],args[2]))!=bytes(u.mem_read(args[1],args[2]))))
  elif name=='a733_npu_application_fail':state['retained_reason']=args[0];ret()
  elif name=='platform_puts':
   text=bytearray();ptr=args[0]
   while (v:=bytes(u.mem_read(ptr,1)))!=b'\0':text.extend(v);ptr+=1
   state['log']+=text.decode();put(im.address('__npu_platform_log')+24,len(state['log']));ret()
  elif name=='platform_hex':state['log']+=f'0x{args[0]:016x}';put(im.address('__npu_platform_log')+24,len(state['log']));ret()
  elif name=='aarch64_device_cache_stride':ret(64)
  elif name=='npu_finite_smc':
   state['exit']=args[0];state['native_receipt']=bytes(u.mem_read(im.address('npu_finite_receipt'),88)).hex();u.emu_stop()
  elif name=='vTaskSuspend':state['retained']=True;u.emu_stop()
  elif name=='platform_panic':raise AssertionError('unexpected panic')
  else:ret()
 sites=instruction_sites(im.parts,lambda word:word&0xffe0001f==0xd4000001 or system_register_access(word))
 hook_addresses(u,set(hooks)|set(sites),code);u.reg_write(UC_ARM64_REG_SP,0x43ffe000);u.reg_write(UC_ARM64_REG_LR,0x43fff000)
 u.emu_start(im.address('npu_runtime_client'),0,count=20000000)
 expected_jobs=0 if variant=='wrong-counter-frequency' else 101 if variant in ('middle-timeout','global-timeout','slow-helper') else 200
 assert state['models']==[n%2 for n in range(expected_jobs)],variant
 if variant in variants[:4]:
  status=int(variant!='matched');assert state['exit']==status and not state['retained'] and state['close_calls']==1,state
  assert len(state['log'].encode())<=16127 and state['compares']==200
  value=h.sustained_report(state['log'],status,480000000,{'runtime':True,'sustained':True,'references':references})
  assert value['jobs']==value['outputs_checked']==200 and value['model_jobs']==[100,100]
  assert value['timing']['maximum_api_ticks']==7200 and value['timing']['jobs_per_second']>0
  assert value['first_mismatch']==(None if not status else {'job':{'first-output-mismatch':0,'middle-output-mismatch':100,'last-output-mismatch':199}[variant],'offset':0,'byte':refs[0 if variant!='last-output-mismatch' else 1][0]^1})
 else:
  reason={'last-IRQ-missing':22,'last-counter-missing':28,'middle-timeout':12,'global-timeout':12,'close-failed':17,'slow-helper':25,'wrong-counter-frequency':23}[variant]
  assert state['retained'] and state['exit'] is None and state['retained_reason']==reason,state
 logs[variant]=state['log'];(a.out/(variant+'.log')).write_text(state['log'])
 if state['native_receipt']:(a.out/(variant+'-receipt.bin')).write_bytes(bytes.fromhex(state['native_receipt']))
 rows.append({'case':variant,'passed':True,**state})
# The pre-feature parser refuses the actual producer log.
if a.before_tree:
 before=load_host(a.before_tree,'before_sustained').finite
 refused(lambda:before.runtime_report(logs['matched'],0,480000000,{'runtime':True,'references':references}),'Runtime stage')
 rows.append({'case':'before-feature-parser-refuses','passed':True})
# Full monitor/platform/receipt frames preserve the actual producer log.
import struct
def frames(text,status=0,native_receipt=None):
 raw=text.encode();assert len(raw)<=16127
 monitor=[0x415337334e505231,7,8,5,status,0x4e505552,1000000,1,26000000]+[0]*7
 receipt=[0x4e50555254523031,7,8,9,status,status+1,0x41200000,0x41f00000,(0x41f00000-0x41200000)//64,480000000,len(raw)]
 platform=bytearray(16384);struct.pack_into('<16Q',platform,0,0x534b4552,5,status,len(raw),0,0,0,0,1000000,26000000,*([0]*6));platform[256:256+len(raw)]=raw
 return struct.pack('<16Q',*monitor),bytes(platform),native_receipt if native_receipt is not None else struct.pack('<11Q',*receipt)
prepared={'epoch':7,'generation':8,'lease':9,'execution_profile':h.RUNTIME,'geometry':{'runtime':True,'sustained':True,'startup_only':True,'references':references,'clean_start':0x41200000,'clean_end':0x41f00000}}
for name in variants[:4]:
 native_receipt=(a.out/(name+'-receipt.bin')).read_bytes()
 value=host.validate_reports(*frames(logs[name],int(name!='matched'),native_receipt),prepared)
 assert value['interrupt_completions']==201 and value['interrupt_stopped'] and value['jobs']==200
 rows.append({'case':name+'-complete-receipts','passed':True})
def read_native(name):
 at=im.address(name);size=sizes[name]
 return next(data[at-base:at-base+size] for base,data in im.parts if base<=at and at+size<=base+len(data))
marker=struct.unpack('<4Q',read_native('npu_startup_return_profile'));names=set(im.symbols)
assert h.sustained_extension(marker,names,read_native)
for name,changed_marker,changed_names,reader,reason in (
 ('missing-extension',marker,names-{'npu_sustained_return_profile'},read_native,'extension/startup'),
 ('ordinary-marker-with-extension',(marker[0],1,2,3),names,read_native,'extension/startup'),
 ('wrong-extension',marker,names,lambda n:bytes(32),'immutable extension'),
 ('extra-network',marker,names|{'npu_finite_reference_2'},read_native,'exactly two')):
 refused(lambda:h.sustained_extension(changed_marker,changed_names,reader),reason)
 rows.append({'case':name+'-refused','passed':True})
# Existing frozen image geometries retain their exact contracts.
for name in ('runtime','model-switch','sdk-access'):
 old_image=a.baseline.parent/name/'image.elf'
 parts,address,build,layout,geometry=host.image_profile(old_image.read_bytes(),{})
 assert not geometry.get('sustained') and len(geometry['references'])==(3 if name=='model-switch' else 2)
 rows.append({'case':name+'-immutable-image-compatible','passed':True})
text=logs['matched'];lines=text.splitlines(keepends=True);summary=lines.index('NPU_SUSTAINED=\n')+1;ticks=lines.index('NPU_JOB_TICKS=\n')+1;digest_line=lines.index('NPU_OUTPUT_DIGEST=\n')+1
cases={}
for name,index,value in [('jobs199',summary+4,199),('checked199',summary+5,199),('mismatch-status',summary+6,1),('model99',summary+7,99),('maximum-wrong',summary+3,1),('frequency-wrong',summary,12000000),('digest-wrong',digest_line,255),('digest-byte-overflow',digest_line,256)]:
 changed=lines.copy();changed[index]=f'0x{value:016x}\n';cases[name]=''.join(changed)
changed=lines.copy();changed[ticks]=changed[ticks].split()[1]+' '+changed[ticks].split()[0]+'\n';cases['reversed-window']=''.join(changed)
changed=lines.copy();changed[ticks+1]=changed[ticks];cases['overlapping-windows']=''.join(changed)
changed=lines.copy();changed[ticks]=f'0x{24000000:016x} 0x{48000001:016x}\n';cases['API-budget-exceeded']=''.join(changed)
cases['missing-job-window']=''.join(lines[:ticks+199]+lines[ticks+200:])
cases['wrong-IRQ']=text.replace('NPU_IRQ_COMPLETIONS=0x00000000000000c9','NPU_IRQ_COMPLETIONS=0x00000000000000c8')
cases['intermediate-close']=text.replace('NPU_STAGE=SUSTAINED\n','NPU_STAGE=SUSTAINED\nNPU_STAGE=CLOSE\n')
for name,text in cases.items():
 refused(lambda:host.validate_reports(*frames(text),prepared),'Sustained')
 rows.append({'case':name+'-refused','passed':True})
closure={}
for dependency in a.out.glob('*.o.d'):
 for item in shlex.split(dependency.read_text().replace('\\\n','').split(':',1)[1]):
  path=Path(item)
  if path.is_file():closure[str(path)]={'sha256':hashlib.sha256(path.read_bytes()).hexdigest()}
for path in (Path(__file__),ROOT/'apps/npu-finite/host.py',ROOT/'apps/npu-interrupt/host.py',a.baseline/'commands.json'):
 closure[str(path)]={'sha256':hashlib.sha256(path.read_bytes()).hexdigest()}
(a.out/'input-identities.json').write_text(json.dumps({'compiled_inputs':closure,'physical_access':False,'board_image':False},indent=2)+'\n')
(a.out/'result.json').write_text(json.dumps({'passed':True,'physical_access':False,'scope':__doc__,'image_sha256':hashlib.sha256(image.read_bytes()).hexdigest(),'cases':rows},indent=2)+'\n')
print('PASS sustained client and host cases:',len(rows))
