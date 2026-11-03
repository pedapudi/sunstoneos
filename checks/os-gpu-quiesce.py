#!/usr/bin/env python3
"""Execute the linked OS GPU quiescence owner with modeled service boundaries.

The native owner, deadlines, atomic admission and drain checks execute. Firmware,
DMA and service replies are modeled; this check does not qualify hardware sleep.
"""
import argparse
import hashlib
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_PC, UC_ARM64_REG_LR, UC_ARM64_REG_SP
from harness import machine as harness_machine, Image, save, skip_crumbs
GATE_RUNS = [['--image', '{elf:ret-gpu}', '--output', '{output}']]

CASES=('ready','owned-job','recovery','manual-closed','cold','refused-create','pending-create',
       'unconstructed','lost-isolation','deadline','missing-primary-queue',
       'pending-client','pending-request','pending-reply','irq-wake','irq-deferred',
       'thermal-peer-drain','thermal-peer-stall','thermal-late-peer')
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--image',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--case',action='append',choices=CASES)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
types={'ss_application','board_application','a733_board_session','pvr_gpu_service','pvr_compute_queue',
       'a733_gpu_queue','a733_gpu_platform','a733_gpu_backend','pvr_gpu_service_request','pvr_gpu_service_reply',
       'pvr_gpu_service_client','pvr_compute_queue_client'}
image=Image(a.image,types);enums={}
with a.image.open('rb') as f:
    dwarf=ELFFile(f).get_dwarf_info()
    for cu in dwarf.iter_CUs():
        for die in cu.iter_DIEs():
            if die.tag=='DW_TAG_enumerator':
                enums[die.attributes['DW_AT_name'].value.decode()]=die.attributes['DW_AT_const_value'].value
            if die.tag=='DW_TAG_variable' and die.attributes.get('DW_AT_name') and die.attributes['DW_AT_name'].value==b'gpu' and cu.get_top_DIE().attributes['DW_AT_name'].value.decode().endswith('/apps/os/gpu.c'):
                typ=die.get_DIE_from_attribute('DW_AT_type');gpu_bytes=typ.attributes['DW_AT_byte_size'].value
                image.layouts['gpu_private']={c.attributes['DW_AT_name'].value.decode():c.attributes['DW_AT_data_member_location'].value for c in typ.iter_children() if c.tag=='DW_TAG_member'}
rows=[]
for case in a.case or CASES:
    u=harness_machine();u.mem_map(0x41000000,0x1200000);u.mem_map(0x80000000,0x20000)
    for address,data in image.parts:
        if data:u.mem_write(address,data)
    skip_crumbs(u,image)
    def put(address,value,n=4):u.mem_write(address,int(value).to_bytes(n,'little'))
    def get(address,n=4):return int.from_bytes(u.mem_read(address,n),'little')
    def field(base,typ,name):return base+image.layouts[typ][name]
    def sf(base,typ,name,value,n=4):put(field(base,typ,name),value,n)
    gpu=image.address('gpu','/apps/os/gpu.c');service=field(gpu,'gpu_private','service')
    q=field(field(gpu,'gpu_private','session'),'a733_gpu_queue','component')
    backend=field(gpu,'gpu_private','backend');power=field(gpu,'gpu_private','power')
    board=image.address('board');session=field(board,'board_application','session')
    task=get(field(image.address('ss_application'),'ss_application','task'),8);assert task
    sf(board,'board_application','live_until',60000000,8);sf(session,'a733_board_session','deadline_us',60000000,8)
    state={'time':100,'operation':None,'ticket':0,'requests':[],'deadlines':[],'waits':0,'creates':0,'delays':0}
    if case!='unconstructed':
        put(image.address('os_gpu_client_task','/apps/os/gpu.c'),task,8)
        sf(gpu,'gpu_private','started',1);sf(gpu,'gpu_private','component',q,8)
        sf(gpu,'gpu_private','opened',1);sf(gpu,'gpu_private','epoch',3,8)
        sf(service,'pvr_gpu_service','phase',enums['PVR_GPU_SERVICE_READY'])
        sf(service,'pvr_gpu_service','epoch',3,8);sf(service,'pvr_gpu_service','published_epoch',3,8)
        sf(q,'pvr_compute_queue','create_done',1);sf(q,'pvr_compute_queue','generation',1)
        sf(q,'pvr_compute_queue','completions',0x8800,8)
        sf(q,'pvr_compute_queue','slot_state',enums['PVR_COMPUTE_QUEUE_IDLE'])
        if case in ('owned-job','deadline'):
            sf(q,'pvr_compute_queue','client_owed',1);sf(q,'pvr_compute_queue','owed',1)
            sf(q,'pvr_compute_queue','slot_state',enums['PVR_COMPUTE_QUEUE_TAKEN'])
        if case=='recovery':sf(q,'pvr_compute_queue','recovering',1)
        if case.startswith('thermal-peer-'):
            peer=field(q,'pvr_compute_queue','peer')
            sf(peer,'pvr_compute_queue_client','completions',0x8820,8)
            sf(peer,'pvr_compute_queue_client','task',task+16,8)
            sf(peer,'pvr_compute_queue_client','client_owed',1)
            sf(peer,'pvr_compute_queue_client','slot_state',enums['PVR_COMPUTE_QUEUE_TAKEN'])
        if case=='lost-isolation':sf(q,'pvr_compute_queue','lost',1)
        if case=='missing-primary-queue':sf(q,'pvr_compute_queue','completions',0,8)
        if case=='manual-closed':
            sf(gpu,'gpu_private','opened',0);sf(gpu,'gpu_private','parked',1);sf(gpu,'gpu_private','manual_closed',1)
            sf(service,'pvr_gpu_service','phase',enums['PVR_GPU_SERVICE_STOPPED'])
            sf(q,'pvr_compute_queue','slot_state',enums['PVR_COMPUTE_QUEUE_CLOSED'])
        if case in ('pending-client','pending-request','pending-reply','irq-wake','irq-deferred'):
            sf(gpu,'gpu_private','opened',0);sf(gpu,'gpu_private','parked',1);sf(gpu,'gpu_private','manual_closed',1)
            sf(q,'pvr_compute_queue','slot_state',enums['PVR_COMPUTE_QUEUE_CLOSED'])
            if case=='pending-client':sf(field(service,'pvr_gpu_service','clients'),'pvr_gpu_service_client','pending',1,8)
            if case=='pending-request':sf(service,'pvr_gpu_service','requests',0x9900,8)
            if case=='pending-reply':sf(field(service,'pvr_gpu_service','clients'),'pvr_gpu_service_client','replies',0x9910,8)
            if case=='irq-wake':sf(service,'pvr_gpu_service','irq_wake_pending',1)
            if case=='irq-deferred':sf(service,'pvr_gpu_service','irq_deferred',1)
        if case in ('cold','refused-create','pending-create'):
            sf(gpu,'gpu_private','opened',0);sf(gpu,'gpu_private','epoch',0,8)
            sf(service,'pvr_gpu_service','phase',enums['PVR_GPU_SERVICE_COLD']);sf(service,'pvr_gpu_service','epoch',0,8)
            sf(service,'pvr_gpu_service','published_epoch',0,8)
            sf(backend,'a733_gpu_backend','phase',enums['A733_GPU_BACKEND_BOUND'])
            sf(power,'a733_gpu_platform','phase',enums['A733_GPU_PLATFORM_NEW'])
            sf(q,'pvr_compute_queue','create_done',2 if case=='refused-create' else 0)
            sf(q,'pvr_compute_queue','create_pending',2 if case!='cold' else 0)
            sf(q,'pvr_compute_queue','slot_state',enums['PVR_COMPUTE_QUEUE_CLOSED'])
    else:assert bytes(u.mem_read(gpu,gpu_bytes))==bytes(gpu_bytes)
    def ret(value=0):u.reg_write(UC_ARM64_REG_X0,value);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
    names=('board_live','a733_smp_counter','xTaskGetTickCount','xTaskGetCurrentTaskHandle','uxQueueMessagesWaiting',
           'gpu_queue_wait','gpu_queue_context_create','vTaskDelay','a733_gpu_backend_stopped',
           'pvr_gpu_service_send_epoch','pvr_gpu_service_collect','os_thermal_throttled')
    hooks={image.address(name):name for name in names}
    def hook(uc,address,size,unused):
        name=hooks.get(address)
        if name is None:return
        args=[uc.reg_read(UC_ARM64_REG_X0+i) for i in range(5)]
        if name=='board_live':ret(1)
        elif name=='a733_smp_counter':ret(state['time']*24)
        elif name=='xTaskGetTickCount':ret(state['time']//1000)
        elif name=='xTaskGetCurrentTaskHandle':ret(task)
        elif name=='uxQueueMessagesWaiting':ret(args[0] in (0x9900,0x9910))
        elif name=='a733_gpu_backend_stopped':ret(1)
        elif name=='os_thermal_throttled':ret(case.startswith('thermal-'))
        elif name=='gpu_queue_wait':
            state['time']+=10000;state['waits']+=1
            if case=='deadline':ret(enums['GPU_QUEUE_TIMEOUT'])
            else:
                sf(q,'pvr_compute_queue','client_owed',0);sf(q,'pvr_compute_queue','owed',0)
                sf(q,'pvr_compute_queue','slot_state',enums['PVR_COMPUTE_QUEUE_IDLE']);ret(enums['GPU_QUEUE_OK'])
        elif name=='gpu_queue_context_create':
            state['creates']+=1;sf(q,'pvr_compute_queue','create_done',1)
            sf(gpu,'gpu_private','opened',1);sf(gpu,'gpu_private','epoch',1,8)
            sf(service,'pvr_gpu_service','phase',enums['PVR_GPU_SERVICE_READY'])
            sf(service,'pvr_gpu_service','published_epoch',1,8)
            sf(q,'pvr_compute_queue','slot_state',enums['PVR_COMPUTE_QUEUE_IDLE']);ret(enums['GPU_QUEUE_OK'])
        elif name=='vTaskDelay':
            state['time']+=1000;state['delays']+=1
            if case=='thermal-peer-drain':
                sf(peer,'pvr_compute_queue_client','client_owed',0)
                sf(peer,'pvr_compute_queue_client','slot_state',enums['PVR_COMPUTE_QUEUE_IDLE'])
            sf(q,'pvr_compute_queue','recovering',0);sf(service,'pvr_gpu_service','published_epoch',4,8);ret()
        elif name=='pvr_gpu_service_send_epoch':
            op=get(field(args[2],'pvr_gpu_service_request','operation'));state['operation']=op
            state['deadlines'].append(state['time']+get(field(args[2],'pvr_gpu_service_request','timeout_us'),8))
            state['requests'].append(op);state['ticket']+=1;put(args[4],state['ticket'],8);ret(1)
        elif name=='pvr_gpu_service_collect':
            state['time']+=1000
            sf(args[2],'pvr_gpu_service_reply','result',enums['PVR_GPU_SERVICE_BUSY'] if case=='thermal-late-peer' and state['ticket']==1 else enums['PVR_GPU_SERVICE_OK'])
            sf(args[2],'pvr_gpu_service_reply','phase',enums['PVR_GPU_SERVICE_STOPPED'])
            sf(args[2],'pvr_gpu_service_reply','epoch',get(field(gpu,'gpu_private','epoch'),8),8);ret(1)
        else:raise AssertionError(name)
    u.hook_add(UC_HOOK_CODE,hook);u.reg_write(UC_ARM64_REG_SP,0x8001f000);u.reg_write(UC_ARM64_REG_LR,0x80000000)
    u.emu_start(image.address('os_gpu_queue_thermal' if case.startswith('thermal-') else 'os_gpu_queue_quiesce'),0x80000000,count=5000000)
    assert u.reg_read(UC_ARM64_REG_PC)==0x80000000,(case,'execution budget')
    result=u.reg_read(UC_ARM64_REG_X0);success=case not in ('lost-isolation','deadline','missing-primary-queue',
        'pending-client','pending-request','pending-reply','irq-wake','irq-deferred')
    if case=='thermal-peer-drain':
        assert result==0 and state['delays']==1 and state['requests']==[enums['PVR_GPU_SERVICE_CLOSE']]
        assert not get(field(gpu,'gpu_private','failed')) and get(field(gpu,'gpu_private','parked'))==1
    elif case=='thermal-peer-stall':
        assert result&0xffffffff==0xffffffff and not state['requests'] and state['time']<=1010000
        assert get(field(peer,'pvr_compute_queue_client','client_owed'))==1
    elif case=='thermal-late-peer':
        assert result==0 and state['delays']==1 and state['requests']==[enums['PVR_GPU_SERVICE_CLOSE']]*2
        assert state['deadlines'][0]==state['deadlines'][1]
        assert not get(field(gpu,'gpu_private','failed')) and get(field(gpu,'gpu_private','parked'))==1
    else:assert bool(result)==success,(case,result,state)
    if case in ('unconstructed','cold','refused-create','manual-closed'):assert not state['requests'],(case,state)
    if case=='owned-job':assert state['waits']==1
    if case=='recovery':assert state['delays']==1 and get(field(gpu,'gpu_private','epoch'),8)==4
    if case=='pending-create':assert state['creates']==1 and state['requests']==[enums['PVR_GPU_SERVICE_CLOSE']]
    if case=='deadline':assert state['time']<=2020000 and get(field(q,'pvr_compute_queue','client_owed'))==1
    rows.append({'case':case,'passed':True,**state})
def invoke(name):
    u.reg_write(UC_ARM64_REG_SP,0x8001f000);u.reg_write(UC_ARM64_REG_LR,0x80000000)
    u.emu_start(image.address(name),0x80000000,count=100000)
    assert u.reg_read(UC_ARM64_REG_PC)==0x80000000
    return u.reg_read(UC_ARM64_REG_X0)
end=60000000
if 'os_cpu_return_cutoff' in image.symbols:end=min(end,invoke('os_cpu_return_cutoff')//24)
assert invoke('recovery_limit')==end-10000
rows.append({'case':'recovery-limit-microseconds','passed':True,'expected_limit':end-10000})
save(a.output/'results.json',{'physical_access':False,'image_sha256':hashlib.sha256(a.image.read_bytes()).hexdigest(),'cases':rows})
print('PASS native GPU quiescence:',len(rows),'cases')
