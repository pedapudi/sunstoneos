#!/usr/bin/env python3
"""Run constructor refusal and cold-service return proofs over modeled setup APIs.

The linked constructor and return branches, worker suspension, receipt predicates
and copied report execute. Cold cases include no CREATE and a collected refused
CREATE. Setup APIs, worker clock samples and queue contents are modeled RAM
metadata. No GPU/MMIO effects, scheduling or power qualification are established.
"""
import argparse, hashlib, importlib.util, json, struct, re
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_PC, UC_ARM64_REG_LR, UC_ARM64_REG_SP
from harness import machine as harness_machine, Image, save, refused, skip_crumbs
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--image',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--case',action='append');p.add_argument('--session',type=Path)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
types={'board_application','a733_board_session','a733_gpu_platform','a733_gpu_power','a733_pmic_consumer',
 'os_thermal_service','a733_thermal_policy','a733_gpu_backend','a733_gpu_reserved','a733_gpu_queue','pvr_compute_queue','pvr_compute_queue_client','pvr_gpu_service','pvr_gpu_service_client',
 'a733_fault_report','a733_fault_record','a733_power_domain_row','a733_pmic_request','a733_rail_token','ss_result'}
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
thermal_producers=[name in im.symbols and any(im.symbols[name])
                   for name in ('os_thermal_record_begin','os_thermal_record_end')]
assert thermal_producers in ([False,False],[True,True]), 'Incomplete thermal window producer pair'
thermal_layout=thermal_capture.geometry(a.image) if all(thermal_producers) else None
diagnostic=None
if 'os_gpu_bisect_configuration' in im.symbols:
 address=im.address('os_gpu_bisect_configuration')
 raw=next(data[address-start:address-start+12] for start,data in im.parts
          if start<=address and address+12<=start+len(data))
 diagnostic=struct.unpack('<3I',raw)
 assert diagnostic[0]==1 and 1<=diagnostic[1]<=4

def require(ok,why):
 if not ok:raise ValueError(why)
def field(base,typ,n):return base+im.layouts[typ][n]
client_count=int(re.search(r'#define PVR_GPU_SERVICE_CLIENTS (\d+)u',(ROOT/'include/pvr_gpu_service.h').read_text())[1])
steps={3:'pvr_compute_queue_client_bind',4:'a733_gpu_platform_native_init',5:'a733_gpu_platform_init',6:'a733_pmic_identity',7:'a733_gpu_platform_bind_pmic',
 9:'a733_gpu_reserved_prepare',10:'a733_gpu_queue_prepare',11:'a733_gpu_backend_bind',12:'pvr_gpu_service_init',13:'pvr_gpu_service_bind'}
cases=[('constructor-'+str(n),n,None) for n in (1,2,3,4,5,6,7,9,10,11,12,13)]
cases += [(name,13,name) for name in ('started','opened','ticket','epoch','owed','runtime-fault','not-suspended',
 'power-token','pmic-request','rail','row-request','row-state','row-count','backend-deadline','service-epoch','reply-pending','context-generation','create-ticket','create-pending','unconsumed-record')]
cases += [(name,0,None) for name in ('cold-no-create','cold-refused-create','cold-refused-create-pending2','cold-refused-create-failed','cold-ready-no-create','cold-ready-refused-create')]
cases += [('cold-'+name,0,name) for name in ('not-started','opened','ticket','epoch','owed','runtime-fault',
 'worker-running','power-token','pmic-request','rail','row-request','row-state','row-count','backend-deadline',
 'service-epoch','published-epoch','service-owner','component-owner','backend-clock','reply-pending',
 'queued-request','queued-reply','queued-record','context-generation','create-ticket','create-pending',
 'unconsumed-record','refused-create-live-ticket','lost','service-error','backend-error','client-task','close-ticket','crumb-pending')]
cases += [('cold-'+member,0,member) for member in ('application_sequence','timeline_serial',
 'timeline_previous','completed_jobs','reset_pending','recovering','close_pending','final_close')]
cases += [('cold-peer-'+member,0,'peer-'+member) for member in ('client_owed','sequence','not_after','reserved','submitted','record')]
cases += [('cold-backend-'+member,0,'backend-'+member) for member in ('recovering','recovery_write','recovery_reset')]
cases += [('cold-service-'+member,0,'service-'+member) for member in ('recovery_detected','recovery_stop_deadline',
 'recovery_ready_deadline','recovery_stage','recovery_count')]
cases += [('cold-manual-closed',0,'manual-closed')]
if a.case:
 assert set(a.case)<={x[0] for x in cases}
 cases=[x for x in cases if x[0] in a.case]
epoch,generation=struct.unpack_from('<2Q',a.session.read_bytes(),24) if a.session else (11,12)
rows=[]
for name,fail_step,mutation in cases:
 u=harness_machine();u.mem_map(0x41000000,0x1200000);u.mem_map(0x80000000,0x20000)
 for address,data in im.parts:
  if data:u.mem_write(address,data)
 skip_crumbs(u,im)
 def put(address,value,n=4):u.mem_write(address,int(value).to_bytes(n,'little'))
 def get(address,n=4):return int.from_bytes(u.mem_read(address,n),'little')
 def sf(base,typ,n,value,size=4):put(field(base,typ,n),value,size)
 gpu=im.address('gpu','/apps/os/gpu.c');power=field(gpu,'gpu_private','power');backend=field(gpu,'gpu_private','backend');service=field(gpu,'gpu_private','service')
 session=field(gpu,'gpu_private','session');component=field(session,'a733_gpu_queue','component');pmic=field(power,'a733_gpu_platform','pmic')
 controller=field(power,'a733_gpu_platform','power');row=field(controller,'a733_gpu_power','row')
 board=im.address('board');bs=field(board,'board_application','session');os=0x80002000
 # Modeled current board metadata; no evidence file or physical authority.
 sf(bs,'a733_board_session','deadline_us',60000000,8);sf(bs,'a733_board_session','epoch',epoch,8);sf(bs,'a733_board_session','generation',generation,8)
 put(im.address('os_gpu_client_task','/apps/os/gpu.c'),0x1234,8)
 if fail_step==1:
  previous=im.address('a733_fault_previous');sf(previous,'a733_fault_report','header',0x31544c46|(1<<32),8)
  slot=field(previous,'a733_fault_report','slot');sf(slot,'a733_fault_record','info',int(re.search(r'#define A733_FAULT_KIND_EXIT (\d+)',(ROOT/'include/a733_fault.h').read_text())[1]),8)
  sf(slot,'a733_fault_record','status',5,8);sf(slot,'a733_fault_record','detail',0x4750554c,8)
 state={'time_us':1000000,'scheduler':1,'suspended':False,'worker_ready':False,'worker_state':None,'setup':[],'getter':0,'queue_reads':0}
 hooks={im.address(n):n for n in [*steps.values(),'xTaskGetSchedulerState','os_session_admitted','xTaskGetCurrentTaskHandle','a733_smp_counter','vTaskSuspend','eTaskGetState','uxQueueMessagesWaiting']+(['os_application_activity_generation'] if thermal_layout is not None else [])}
 def ret(value=0):u.reg_write(UC_ARM64_REG_X0,value);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
 def hook(cpu,address,size,unused):
  n=hooks.get(address)
  if not n:return
  if n=='os_application_activity_generation':ret(1)
  elif n=='xTaskGetSchedulerState':ret(state['scheduler'])
  elif n=='os_session_admitted':ret(os)
  elif n=='xTaskGetCurrentTaskHandle':ret(0x1234)
  elif n=='a733_smp_counter':ret(state['time_us']*24)
  elif n=='vTaskSuspend':
   assert state['scheduler']==(2 if fail_step==0 else 1) and u.reg_read(UC_ARM64_REG_X0)==0x4567;state['suspended']=True;ret()
  elif n=='eTaskGetState':state['getter']+=1;ret(enums['eSuspended'] if state['suspended'] else enums[state['worker_state']] if state['worker_state'] else enums['eBlocked'] if fail_step==0 and not state['worker_ready'] else enums['eReady'])
  elif n=='uxQueueMessagesWaiting':
   state['queue_reads']+=1;queue=u.reg_read(UC_ARM64_REG_X0)
   bad={'queued-request':0x8200,'queued-reply':0x8300,'queued-record':0x8100,'peer-record':0x8400}.get(mutation)
   ret(queue==bad)
  else:
   stage=next(k for k,v in steps.items() if v==n);state['setup'].append(stage)
   if stage==fail_step:ret(0 if stage!=6 else -1);return
   if stage==5:sf(power,'a733_gpu_platform','initialized',1)
   elif stage==10:
    sf(component,'pvr_compute_queue','owner',session,8);sf(component,'pvr_compute_queue','service',service,8)
    sf(component,'pvr_compute_queue','phase',enums['PVR_COMPUTE_QUEUE_BOUND']);sf(component,'pvr_compute_queue','slot_state',enums['PVR_COMPUTE_QUEUE_CLOSED']);sf(component,'pvr_compute_queue','completions',0x8100,8)
   elif stage==11:
    sf(backend,'a733_gpu_backend','initialized',1);sf(backend,'a733_gpu_backend','phase',enums['A733_GPU_BACKEND_BOUND'])
    for key,val in [('service',service),('power',power),('memory',field(field(gpu,'gpu_private','firmware'),'a733_gpu_reserved','memory'))]:sf(backend,'a733_gpu_backend',key,val,8)
   elif stage==12:
    sf(service,'pvr_gpu_service','initialized',1);sf(service,'pvr_gpu_service','worker',0x4567,8);sf(service,'pvr_gpu_service','requests',0x8200,8)
    sf(service,'pvr_gpu_service','owner',client_count);sf(service,'pvr_gpu_service','irq_masked',1)
    clients=field(service,'pvr_gpu_service','clients')
    for i in range(client_count):sf(clients+i*client_size,'pvr_gpu_service_client','replies',0x8300+i*16,8)
   elif stage==13:sf(field(service,'pvr_gpu_service','clients'),'pvr_gpu_service_client','task',0x1234,8)
   ret(field(field(gpu,'gpu_private','firmware'),'a733_gpu_reserved','config') if stage==9 else component if stage==10 else enums['A733_PMIC_READY'] if stage==6 else 1)
 u.hook_add(UC_HOOK_CODE,hook)
 def call(fn,arg=0,arg1=0):
  u.reg_write(UC_ARM64_REG_SP,0x8001f000);u.reg_write(UC_ARM64_REG_LR,0x80000000);u.reg_write(UC_ARM64_REG_X0,arg);u.reg_write(UC_ARM64_REG_X1,arg1)
  u.emu_start(im.address(fn,'return-thermal.c' if fn=='record_reading' else None),0x80000000,count=2000000);assert u.reg_read(UC_ARM64_REG_PC)==0x80000000,fn
  return u.reg_read(UC_ARM64_REG_X0)
 client_size=sizes['pvr_gpu_service_client']
 # Constructor ownership and refusal branches execute with the actual image.
 if fail_step==2:
  sf(bs,'a733_board_session','pmic_owner',0x4f53475055464d31,8)
 assert call('os_gpu_queue_start',6)==1,(name,state)
 assert get(field(gpu,'gpu_private','constructor_refusal'))==fail_step,(name,state,get(field(gpu,'gpu_private','constructor_refusal')))
 assert call('os_gpu_queue_start',6)==0 # immutable reason, no second construction
 state['scheduler']=2
 if fail_step==0:
  # The COLD worker samples the counter before it blocks; no enable runs.
  sampled=0 if name=='cold-ready-no-create' else state['time_us']
  sf(backend,'a733_gpu_backend','last_now',sampled,8)
  sf(service,'pvr_gpu_service','last_now',sampled,8)
  state['worker_ready']=name.startswith('cold-ready-')
  if 'refused-create' in name:
   sf(component,'pvr_compute_queue','create_done',2)
   sf(component,'pvr_compute_queue','create_pending',2 if name.endswith('pending2') else 1)
   sf(field(service,'pvr_gpu_service','clients'),'pvr_gpu_service_client','next_ticket',1,8)
   if name.endswith('failed'):sf(component,'pvr_compute_queue','phase',enums['PVR_COMPUTE_QUEUE_FAILED'])
 if mutation:
  direct={'started':'started','opened':'opened','ticket':'accepted_ticket','epoch':'epoch','runtime-fault':'failed'}
  if mutation=='not-started':sf(gpu,'gpu_private','started',0)
  elif mutation in direct:put(field(gpu,'gpu_private',direct[mutation]),1,8 if mutation in ('ticket','epoch') else 4)
  elif mutation=='worker-running':state['worker_state']='eRunning'
  elif mutation=='lost':sf(component,'pvr_compute_queue','lost',1)
  elif mutation=='service-error':sf(service,'pvr_gpu_service','error',1)
  elif mutation=='backend-error':sf(backend,'a733_gpu_backend','error',1)
  elif mutation=='client-task':sf(field(service,'pvr_gpu_service','clients'),'pvr_gpu_service_client','task',0x9999,8)
  elif mutation=='close-ticket':sf(component,'pvr_compute_queue','close_ticket',1,8)
  elif mutation=='published-epoch':sf(service,'pvr_gpu_service','published_epoch',1,8)
  elif mutation=='service-owner':sf(service,'pvr_gpu_service','owner',0)
  elif mutation=='component-owner':sf(component,'pvr_compute_queue','owner',0,8)
  elif mutation=='backend-clock':sf(backend,'a733_gpu_backend','last_now',state['time_us']+1,8)
  elif mutation=='refused-create-live-ticket':sf(component,'pvr_compute_queue','create_ticket',1,8)
  elif mutation=='not-suspended':state['suspended']=False
  elif mutation=='power-token':sf(power,'a733_gpu_platform','token',1,8)
  elif mutation=='pmic-request':sf(field(pmic,'a733_pmic_consumer','request'),'a733_pmic_request','id',1,8)
  elif mutation=='rail':sf(field(pmic,'a733_pmic_consumer','rail'),'a733_rail_token','id',1,8)
  elif mutation.startswith('row-'):sf(row,'a733_power_domain_row',{'row-request':'request','row-state':'state','row-count':'counted'}[mutation],1)
  elif mutation=='backend-deadline':sf(backend,'a733_gpu_backend','deadline',1,8)
  elif mutation=='service-epoch':sf(service,'pvr_gpu_service','epoch',1,8)
  elif mutation=='reply-pending':sf(field(service,'pvr_gpu_service','clients'),'pvr_gpu_service_client','pending',1,8)
  elif mutation=='manual-closed':sf(gpu,'gpu_private','manual_closed',1)
  elif mutation in ('backend-recovering','backend-recovery_write','backend-recovery_reset'):
   sf(backend,'a733_gpu_backend',mutation[len('backend-'):],1)
  elif mutation.startswith('service-recovery_'):
   member=mutation[len('service-'):]
   sf(service,'pvr_gpu_service',member,1,8 if member in ('recovery_detected','recovery_stop_deadline','recovery_ready_deadline') else 4)
  elif mutation in ('application_sequence','timeline_serial','timeline_previous','completed_jobs',
                    'reset_pending','recovering','close_pending','final_close'):
   sf(component,'pvr_compute_queue',mutation,1)
  elif mutation.startswith('peer-'):
   peer=field(component,'pvr_compute_queue','peer')
   sf(peer,'pvr_compute_queue_client','completions',0x8400,8)
   member=mutation[5:]
   if member in ('reserved','submitted'):
    sf(peer,'pvr_compute_queue_client','slot_state',enums['PVR_COMPUTE_QUEUE_'+member.upper()])
   elif member!='record':sf(peer,'pvr_compute_queue_client',member,1,8 if member=='not_after' else 4)
  elif mutation in ('owed','context-generation','create-ticket','create-pending','unconsumed-record','crumb-pending'):
   member={'owed':'owed','context-generation':'generation','create-ticket':'create_ticket','create-pending':'create_pending','unconsumed-record':'client_owed','crumb-pending':'crumb_pending'}[mutation]
   sf(component,'pvr_compute_queue',member,1,8 if member=='create_ticket' else 4)
 result=call('os_gpu_retire') if fail_step==0 else call('os_gpu_queue_return_untouched')
 assert bool(result) is (mutation is None),(name,result,state)
 if result:
  raw=bytes(u.mem_read(im.address('os_gpu_return_report'),64));decoded=decoder.decode(raw,epoch,generation,require,allow_untouched=True)
  diagnostic_cold=fail_step==0 and diagnostic is not None and diagnostic[1]==1
  if diagnostic_cold:
   stage,limit=diagnostic[1:]
   assert struct.unpack('<8Q',raw)==(decoder.MAGIC,3,64,epoch,generation,0,0,(stage<<32)|limit)
   expected={'jobs':0,'gpu_bisect':{'stage':decoder.BISECT_STAGES[stage],'limit':limit},
             'gpu_off':False,'gpu_never_powered':True,'worker_parked':False,'qualification_passed':False}
  else:
   expected={'jobs':0,'gpu_untouched':True,'qualification_passed':False}
   expected.update({'device_never_opened':True} if fail_step==0 else {'constructor_refusal':fail_step,'constructor_stage':decoder.CONSTRUCTOR_STAGES[fail_step]})
  assert decoded==expected
  assert get(field(gpu,'gpu_private','constructor_refusal'))==fail_step
  refused(lambda:decoder.decode(raw,epoch,generation,require),
          'GPU bisect retirement' if diagnostic_cold else 'not admitted')
  assert call('os_gpu_return_ready')==1 and call('os_gpu_queue_return_untouched')==0
  (a.output/(name+'-report.bin')).write_bytes(raw)
  text=b'MODEL ONLY CPU return with GPU never opened\n';u.mem_write(0x80001000,text)
  result_address=im.address('ss_result_record')
  sf(result_address,'ss_result','magic',0x31544c5553455241,8)
  sf(result_address,'ss_result','status',1,8)
  sf(result_address,'ss_result','payload_bytes',len(text),8)
  call('os_cpu_return_begin',0x80001000,len(text))
  assert call('os_cpu_return_requested')==1
  (a.output/(name+'-result.bin')).write_bytes(u.mem_read(result_address,56))
  if thermal_layout is not None:
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
 save(a.output/'results.json',{'passed':True,'physical_access':False,'image_sha256':hashlib.sha256(a.image.read_bytes()).hexdigest(),'scope':__doc__,'thermal_window_exercised':thermal_layout is not None,'cases':rows})
save(a.output/'results.json',{'passed':True,'physical_access':False,'image_sha256':hashlib.sha256(a.image.read_bytes()).hexdigest(),'scope':__doc__,'thermal_window_exercised':thermal_layout is not None,'cases':rows})
print('PASS untouched GPU constructor/return:',len(rows),'cases')
