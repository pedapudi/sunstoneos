#!/usr/bin/env python3
"""Actual constructor refusal exits and untouched-return proof over modeled setup APIs.

Constructor APIs before the selected refusal return modeled RAM metadata. The
linked startup branch, refusal marker, worker suspension, all receipt predicates
and copied record execute. No GPU/MMIO effects, scheduling or power qualification.
"""
import argparse, hashlib, importlib.util, json, struct, re
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_PC, UC_ARM64_REG_LR, UC_ARM64_REG_SP
from harness import Image, save, refused, skip_crumbs
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--image',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--case',action='append');p.add_argument('--session',type=Path)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
types={'board_application','a733_board_session','a733_gpu_platform','a733_gpu_power','a733_pmic_consumer',
 'os_thermal_service','a733_thermal_policy','a733_gpu_backend','a733_gpu_reserved','a733_gpu_mimus','pvr_mimus_service','pvr_gpu_service','pvr_gpu_service_client',
 'os_dispatch_record','a733_fault_report','a733_fault_record','a733_power_domain_row','a733_pmic_request','a733_rail_token','pvr_mimus_tensor'}
im=Image(a.image,types);enums={};sizes={}
with a.image.open('rb') as f:
 for cu in ELFFile(f).get_dwarf_info().iter_CUs():
  for die in cu.iter_DIEs():
   if die.tag=='DW_TAG_structure_type' and die.attributes.get('DW_AT_name') and 'DW_AT_byte_size' in die.attributes:sizes[die.attributes['DW_AT_name'].value.decode()]=die.attributes['DW_AT_byte_size'].value
   if die.tag=='DW_TAG_enumerator':enums[die.attributes['DW_AT_name'].value.decode()]=die.attributes['DW_AT_const_value'].value
   if die.tag=='DW_TAG_variable' and die.attributes.get('DW_AT_name') and die.attributes['DW_AT_name'].value==b'gpu' and cu.get_top_DIE().attributes['DW_AT_name'].value.decode().endswith('/apps/os/gpu.c'):
    im.layouts['gpu_private']={d.attributes['DW_AT_name'].value.decode():d.attributes['DW_AT_data_member_location'].value for d in die.get_DIE_from_attribute('DW_AT_type').iter_children() if d.tag=='DW_TAG_member'}
spec=importlib.util.spec_from_file_location('gpu_return',ROOT/'apps/os/gpu-return.py');decoder=importlib.util.module_from_spec(spec);spec.loader.exec_module(decoder)
spec=importlib.util.spec_from_file_location('thermal_capture',ROOT/'apps/os/cpu-return-thermal.py');thermal_capture=importlib.util.module_from_spec(spec);spec.loader.exec_module(thermal_capture)
thermal_layout=thermal_capture.geometry(a.image)
def require(ok,why):
 if not ok:raise ValueError(why)
def field(base,typ,n):return base+im.layouts[typ][n]
client_count=int(re.search(r'#define PVR_GPU_SERVICE_CLIENTS (\d+)u',(ROOT/'include/pvr_gpu_service.h').read_text())[1])
steps={4:'a733_gpu_platform_native_init',5:'a733_gpu_platform_init',6:'a733_pmic_identity',7:'a733_gpu_platform_bind_pmic',
 9:'a733_gpu_reserved_prepare',10:'a733_gpu_mimus_prepare',11:'a733_gpu_backend_bind',12:'pvr_gpu_service_init',13:'pvr_gpu_service_bind'}
cases=[('constructor-'+str(n),n,None) for n in (1,2,3,4,5,6,7,9,10,11,12,13)]
cases += [(name,13,name) for name in ('started','opened','ticket','epoch','job','runtime-fault','not-suspended',
 'power-token','pmic-request','rail','row-request','row-state','row-count','backend-deadline','service-epoch','reply-pending','dispatch-placed')]
if a.case:
 assert set(a.case)<={x[0] for x in cases}
 cases=[x for x in cases if x[0] in a.case]
epoch,generation=struct.unpack_from('<2Q',a.session.read_bytes(),24) if a.session else (11,12)
rows=[]
for name,fail_step,mutation in cases:
 u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0x41000000,0x1200000);u.mem_map(0x80000000,0x20000)
 for address,data in im.parts:
  if data:u.mem_write(address,data)
 skip_crumbs(u,im)
 def put(address,value,n=4):u.mem_write(address,int(value).to_bytes(n,'little'))
 def get(address,n=4):return int.from_bytes(u.mem_read(address,n),'little')
 def sf(base,typ,n,value,size=4):put(field(base,typ,n),value,size)
 gpu=im.address('gpu','/apps/os/gpu.c');power=field(gpu,'gpu_private','power');backend=field(gpu,'gpu_private','backend');service=field(gpu,'gpu_private','service')
 session=field(gpu,'gpu_private','session');component=field(session,'a733_gpu_mimus','component');pmic=field(power,'a733_gpu_platform','pmic')
 controller=field(power,'a733_gpu_platform','power');row=field(controller,'a733_gpu_power','row');record=im.address('os_dispatch_record')
 board=im.address('board');bs=field(board,'board_application','session');os=0x80002000
 # Modeled current board metadata; no evidence file or physical authority.
 sf(bs,'a733_board_session','deadline_us',60000000,8);sf(bs,'a733_board_session','epoch',epoch,8);sf(bs,'a733_board_session','generation',generation,8)
 put(im.address('os_recognizer'),0x1234,8)
 if fail_step==1:
  previous=im.address('a733_fault_previous');sf(previous,'a733_fault_report','header',0x31544c46|(1<<32),8)
  slot=field(previous,'a733_fault_report','slot');sf(slot,'a733_fault_record','info',int(re.search(r'#define A733_FAULT_KIND_EXIT (\d+)',(ROOT/'include/a733_fault.h').read_text())[1]),8)
  sf(slot,'a733_fault_record','status',5,8);sf(slot,'a733_fault_record','detail',0x4750554c,8)
 state={'time_us':1000000,'scheduler':1,'suspended':False,'setup':[],'getter':0,'queue_reads':0}
 hooks={im.address(n):n for n in [*steps.values(),'xTaskGetSchedulerState','os_session_admitted','xTaskGetCurrentTaskHandle','a733_smp_counter','vTaskSuspend','eTaskGetState','uxQueueMessagesWaiting']}
 def ret(value=0):u.reg_write(UC_ARM64_REG_X0,value);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
 def hook(cpu,address,size,unused):
  n=hooks.get(address)
  if not n:return
  if n=='xTaskGetSchedulerState':ret(state['scheduler'])
  elif n=='os_session_admitted':ret(os)
  elif n=='xTaskGetCurrentTaskHandle':ret(0x1234)
  elif n=='a733_smp_counter':ret(state['time_us']*24)
  elif n=='vTaskSuspend':
   assert state['scheduler']==1 and u.reg_read(UC_ARM64_REG_X0)==0x4567;state['suspended']=True;ret()
  elif n=='eTaskGetState':state['getter']+=1;ret(enums['eSuspended'] if state['suspended'] else enums['eReady'])
  elif n=='uxQueueMessagesWaiting':state['queue_reads']+=1;ret(0)
  else:
   stage=next(k for k,v in steps.items() if v==n);state['setup'].append(stage)
   if stage==fail_step:ret(0 if stage!=6 else -1);return
   if stage==5:sf(power,'a733_gpu_platform','initialized',1)
   elif stage==10:
    sf(component,'pvr_mimus_service','config',field(session,'a733_gpu_mimus','config'),8);sf(component,'pvr_mimus_service','service',service,8)
    sf(component,'pvr_mimus_service','phase',enums['PVR_MIMUS_SERVICE_BOUND']);sf(component,'pvr_mimus_service','slot_state',enums['PVR_MIMUS_SLOT_CLOSED']);sf(component,'pvr_mimus_service','completions',0x8100,8)
   elif stage==11:
    sf(backend,'a733_gpu_backend','initialized',1);sf(backend,'a733_gpu_backend','phase',enums['A733_GPU_BACKEND_BOUND'])
    for key,val in [('service',service),('power',power),('memory',field(field(gpu,'gpu_private','firmware'),'a733_gpu_reserved','memory'))]:sf(backend,'a733_gpu_backend',key,val,8)
   elif stage==12:
    sf(service,'pvr_gpu_service','initialized',1);sf(service,'pvr_gpu_service','worker',0x4567,8);sf(service,'pvr_gpu_service','requests',0x8200,8)
    clients=field(service,'pvr_gpu_service','clients')
    for i in range(client_count):sf(clients+i*client_size,'pvr_gpu_service_client','replies',0x8300+i*16,8)
   ret(field(field(gpu,'gpu_private','firmware'),'a733_gpu_reserved','config') if stage==9 else component if stage==10 else enums['A733_PMIC_READY'] if stage==6 else 1)
 u.hook_add(UC_HOOK_CODE,hook)
 def call(fn,arg=0,arg1=0):
  u.reg_write(UC_ARM64_REG_SP,0x8001f000);u.reg_write(UC_ARM64_REG_LR,0x80000000);u.reg_write(UC_ARM64_REG_X0,arg);u.reg_write(UC_ARM64_REG_X1,arg1)
  u.emu_start(im.address(fn,'return-thermal.c' if fn=='record_reading' else None),0x80000000,count=2000000);assert u.reg_read(UC_ARM64_REG_PC)==0x80000000,fn
  return u.reg_read(UC_ARM64_REG_X0)
 client_size=sizes['pvr_gpu_service_client']
 # Let real session/tensor checks execute, injecting only their intended refusal.
 # os_session_admitted sees a nonaliasing zero grant except the owner case.
 # The grant offset is resolved from the OS layout in the companion type reader.
 if fail_step==2:
  # Any fixed board owner may supply the exact forbidden identity.
  sf(bs,'a733_board_session','pmic_owner',0x4f53475055464d31,8)
 if fail_step==3:
  ordinal=int.from_bytes(u.mem_read(im.address('os_placement_tensors')+4,2),'little')
  tensor_size=sizes['pvr_mimus_tensor']
  sf(field(gpu,'gpu_private','tensors')+ordinal*tensor_size,'pvr_mimus_tensor','job_rows',1)
 assert call('a733_gpu_mimus_start',6)==1,(name,state)
 assert get(field(gpu,'gpu_private','constructor_refusal'))==fail_step,(name,state,get(field(gpu,'gpu_private','constructor_refusal')))
 assert call('a733_gpu_mimus_start',6)==0 # immutable reason, no second construction
 state['scheduler']=2;sf(record,'os_dispatch_record','open_failed',1)
 if mutation:
  direct={'started':'started','opened':'opened','ticket':'accepted_ticket','epoch':'epoch','job':'job_pending','runtime-fault':'failed'}
  if mutation in direct:put(field(gpu,'gpu_private',direct[mutation]),1,8 if mutation in ('ticket','epoch') else 4)
  elif mutation=='not-suspended':state['suspended']=False
  elif mutation=='power-token':sf(power,'a733_gpu_platform','token',1,8)
  elif mutation=='pmic-request':sf(field(pmic,'a733_pmic_consumer','request'),'a733_pmic_request','id',1,8)
  elif mutation=='rail':sf(field(pmic,'a733_pmic_consumer','rail'),'a733_rail_token','id',1,8)
  elif mutation.startswith('row-'):sf(row,'a733_power_domain_row',{'row-request':'request','row-state':'state','row-count':'counted'}[mutation],1)
  elif mutation=='backend-deadline':sf(backend,'a733_gpu_backend','deadline',1,8)
  elif mutation=='service-epoch':sf(service,'pvr_gpu_service','epoch',1,8)
  elif mutation=='reply-pending':sf(field(service,'pvr_gpu_service','clients'),'pvr_gpu_service_client','pending',1,8)
  elif mutation=='dispatch-placed':sf(record,'os_dispatch_record','placed',1)
 result=call('a733_gpu_mimus_return_untouched')
 assert bool(result) is (mutation is None),(name,result,state)
 if result:
  raw=bytes(u.mem_read(im.address('os_gpu_return_report'),64));decoded=decoder.decode(raw,epoch,generation,require,allow_untouched=True)
  assert decoded=={'jobs':0,'gpu_untouched':True,'constructor_refusal':fail_step,'constructor_stage':decoder.CONSTRUCTOR_STAGES[fail_step],'qualification_passed':False}
  refused(lambda:decoder.decode(raw,epoch,generation,require),'not admitted')
  assert call('os_gpu_return_ready')==1 and call('a733_gpu_mimus_return_untouched')==0
  (a.output/(name+'-report.bin')).write_bytes(raw)
  text=b'MODEL ONLY CPU fallback after refused GPU construction\n';u.mem_write(0x80001000,text)
  call('os_cpu_return_begin',0x80001000,len(text))
  output=bytes(u.mem_read(im.address('os_cpu_return_output'),48+16384))
  assert struct.unpack_from('<2Q',output,24)==(epoch,generation) and output[48:48+len(text)]==text
  (a.output/(name+'-output.bin')).write_bytes(output)
  policy=field(im.address('thermal','return-thermal.c'),'os_thermal_service','policy')
  sf(policy,'a733_thermal_policy','seen',31,1);sf(policy,'a733_thermal_policy','cpu_critical_millicelsius',70000)
  for i in range(5):
   put(field(policy,'a733_thermal_policy','millicelsius')+4*i,40000+1000*i)
   put(field(policy,'a733_thermal_policy','seen_us')+8*i,state['time_us'],8)
  call('record_reading',state['time_us'],0);call('os_thermal_record_begin')
  for n in range(1,4):
   state['time_us']+=100000;sf(policy,'a733_thermal_policy','sequence',n)
   for i in range(5):put(field(policy,'a733_thermal_policy','seen_us')+8*i,state['time_us'],8)
   call('record_reading',state['time_us'],0)
  call('os_thermal_record_end',1)
  copied={label:bytes(u.mem_read(address,size)) for label,address,size in thermal_capture.captures(thermal_layout)}
  assert thermal_capture.decode(thermal_layout,copied,epoch,generation)['classification']=='complete'
  for label,data in copied.items():(a.output/(name+'-'+label+'.bin')).write_bytes(data)
 rows.append({'name':name,'passed':True,'constructor_apis':state['setup'],'getter_calls':state['getter']})
 save(a.output/'results.json',{'passed':True,'physical_access':False,'image_sha256':hashlib.sha256(a.image.read_bytes()).hexdigest(),'scope':__doc__,'cases':rows})
save(a.output/'results.json',{'passed':True,'physical_access':False,'image_sha256':hashlib.sha256(a.image.read_bytes()).hexdigest(),'scope':__doc__,'cases':rows})
print('PASS untouched GPU constructor/return:',len(rows),'cases')
