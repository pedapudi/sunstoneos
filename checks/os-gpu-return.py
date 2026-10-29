#!/usr/bin/env python3
"""Linked hook and GPU finalizer over modeled service replies and stop proof.

The real request bounds, hook/output admission, queue checks, worker suspension,
return record and CPU-completion gate execute. Firmware stop and queue APIs are
explicit model boundaries, separately covered by pvr-mimus-service cases31–33.
"""
import argparse
import json
import hashlib
import struct
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_PC, UC_ARM64_REG_LR, UC_ARM64_REG_SP
from harness import Image, save

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--image',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--session',type=Path)
p.add_argument('--case',action='append')
p.add_argument('--caller-only',action='store_true')
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
types={'board_application','a733_board_session','pvr_gpu_service','pvr_mimus_service','a733_gpu_mimus',
       'a733_gpu_platform','a733_pmic_consumer','a733_pmic_request','pvr_gpu_service_reply',
       'pvr_gpu_service_request','os_dispatch_record','Call'}
image=Image(a.image,types)
with a.image.open('rb') as f:
    sym=list(ELFFile(f).get_section_by_name('.symtab').iter_symbols())
    recognize=next(s for s in sym if s.name=='_ZN12_GLOBAL__N_19recognizeEPv')
    address,size=recognize['st_value'],recognize['st_size']
body=next(data[address-start:address-start+size] for start,data in image.parts if start<=address<address+size<=start+len(data))
calls=[]
for offset in range(0,size,4):
    word=int.from_bytes(body[offset:offset+4],'little')
    if word&0xfc000000==0x94000000:
        displacement=(word&0x3ffffff)<<2
        if displacement&(1<<27):displacement-=1<<28
        calls.append((offset,address+offset+displacement))
selected=[next(o for o,d in calls if d==image.address(name)) for name in
          ('_Z19mimus_parallel_stopv','os_gpu_retire','os_cpu_return_begin')]
assert selected==sorted(selected) and len(set(selected))==3
assert int.from_bytes(body[selected[1]+4:selected[1]+8],'little')&0xff00001f==0x34000000
caller={'image_sha256':hashlib.sha256(a.image.read_bytes()).hexdigest(),
        'parallel_stop_gpu_retire_completion_offsets':selected,'gpu_refusal_branches_before_publication':True}
if a.caller_only:
    save(a.output/'caller.json',caller)
    print('PASS exact linked recognizer: worker stop ACKs, checked GPU retirement, CPU completion')
    raise SystemExit(0)
enums={}
with a.image.open('rb') as f:
    dwarf=ELFFile(f).get_dwarf_info()
    for cu in dwarf.iter_CUs():
        for die in cu.iter_DIEs():
            if die.tag=='DW_TAG_enumerator':
                enums[die.attributes['DW_AT_name'].value.decode()]=die.attributes['DW_AT_const_value'].value
        if cu.get_top_DIE().attributes['DW_AT_name'].value.decode().endswith('/apps/os/gpu.c'):
            for die in cu.iter_DIEs():
                if die.tag=='DW_TAG_variable' and die.attributes.get('DW_AT_name') and die.attributes['DW_AT_name'].value==b'gpu':
                    image.layouts['gpu_private']={d.attributes['DW_AT_name'].value.decode():d.attributes['DW_AT_data_member_location'].value
                        for d in die.get_DIE_from_attribute('DW_AT_type').iter_children() if d.tag=='DW_TAG_member'}
epoch,generation=struct.unpack_from('<2Q',a.session.read_bytes(),24) if a.session else (123,456)
rows=[]
cases=('opened','thermal-parked','borrowed-job','hook-in-flight','incomplete-output','no-validated-job',
             'stop-proof-missing','close-refused','park-failed','park-timeout','reply-epoch','queue-not-empty',
             'late-finalizer','recognition-cutoff','finalizing-window','finalizing-expired','begin-expired')
if a.case:
    assert set(a.case)<=set(cases)
    cases=a.case
for case in cases:
    u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0x41000000,0x1200000);u.mem_map(0x80000000,0x10000)
    for address,data in image.parts:
        if data:u.mem_write(address,data)
    def put(address,value,n=4):u.mem_write(address,int(value).to_bytes(n,'little'))
    def get(address,n=4):return int.from_bytes(u.mem_read(address,n),'little')
    def field(base,typ,name):return base+image.layouts[typ][name]
    def sf(base,typ,name,value,n=4):put(field(base,typ,name),value,n)
    gpu=image.address('gpu','/apps/os/gpu.c');service=field(gpu,'gpu_private','service')
    component=field(field(gpu,'gpu_private','session'),'a733_gpu_mimus','component')
    power=field(gpu,'gpu_private','power');board=image.address('board')
    session=field(board,'board_application','session');call=image.address('call','/apps/os/gpu_hook.cc')
    record=image.address('os_dispatch_record')
    sf(gpu,'gpu_private','started',1);sf(gpu,'gpu_private','opened',case!='thermal-parked')
    sf(gpu,'gpu_private','parked',case=='thermal-parked');sf(gpu,'gpu_private','epoch',3,8)
    sf(gpu,'gpu_private','component',component,8)
    sf(gpu,'gpu_private','job_pending',case=='borrowed-job')
    sf(service,'pvr_gpu_service','worker',0x4567,8)
    sf(component,'pvr_mimus_service','sequence',7)
    sf(component,'pvr_mimus_service','slot_state',enums['PVR_MIMUS_SLOT_CLOSED'])
    put(image.address('os_recognizer'),0x1234,8)
    sf(board,'board_application','live_until',60000000,8)
    sf(session,'a733_board_session','deadline_us',60000000,8)
    sf(session,'a733_board_session','epoch',epoch,8);sf(session,'a733_board_session','generation',generation,8)
    sf(record,'os_dispatch_record','opened',1)
    sf(record,'os_dispatch_record','incomplete',case=='incomplete-output')
    sf(call,'Call','in_flight',case=='hook-in-flight',1)
    put(image.address('completed','/apps/os/gpu_hook.cc'),0 if case=='no-validated-job' else 7)
    state={'time':100,'operation':None,'ticket':0,'suspended':0,'hook_removed':0,'requests':[],'exit':None}
    names=('board_live','a733_smp_counter','xTaskGetTickCount','xTaskGetCurrentTaskHandle',
           'a733_gpu_backend_stopped','pvr_gpu_service_send_epoch','pvr_gpu_service_collect',
           'uxQueueMessagesWaiting','vTaskDelay','vTaskSuspend','board_fail','platform_exit',
           '_ZN5mimus3asr15SetExactSumHookEPKNS0_12ExactSumHookE')
    hooks={image.address(name):name for name in names}
    def ret(value=0):u.reg_write(UC_ARM64_REG_X0,value);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
    def hook(uc,address,size,unused):
        name=hooks.get(address)
        if name is None:return
        args=[uc.reg_read(UC_ARM64_REG_X0+i) for i in range(5)]
        if name=='platform_exit':state['exit']=args[0];uc.emu_stop()
        elif name=='board_fail':ret()
        elif name=='board_live':ret(1)
        elif name=='a733_smp_counter':ret(state['time']*24)
        elif name=='xTaskGetTickCount':ret(state['time']//1000)
        elif name=='xTaskGetCurrentTaskHandle':ret(0x1234)
        elif name=='a733_gpu_backend_stopped':ret(case!='stop-proof-missing')
        elif name=='pvr_gpu_service_send_epoch':
            operation=get(field(args[2],'pvr_gpu_service_request','operation'))
            state['operation']=operation;state['ticket']+=1;state['requests'].append(operation)
            put(args[4],state['ticket'],8);ret(1)
        elif name=='pvr_gpu_service_collect':
            state['time']+=1000
            op=state['operation']
            if case=='park-timeout' and op==enums['PVR_GPU_SERVICE_PARK']:ret(0);return
            sf(args[2],'pvr_gpu_service_reply','result',enums['PVR_GPU_SERVICE_BUSY'] if case=='close-refused' and op==2 else
               enums['PVR_GPU_SERVICE_FAULT'] if case=='park-failed' and op==enums['PVR_GPU_SERVICE_PARK'] else 0)
            sf(args[2],'pvr_gpu_service_reply','epoch',4 if case=='reply-epoch' else 3,8)
            phase=enums['PVR_GPU_SERVICE_PARKED'] if op==enums['PVR_GPU_SERVICE_PARK'] else enums['PVR_GPU_SERVICE_STOPPED']
            sf(args[2],'pvr_gpu_service_reply','phase',phase);ret(1)
        elif name=='uxQueueMessagesWaiting':ret(case=='queue-not-empty')
        elif name=='vTaskDelay':state['time']+=1000;ret()
        elif name=='vTaskSuspend':
            assert args[0]==0x4567 and state['requests'][-1]==enums['PVR_GPU_SERVICE_PARK']
            assert state['hook_removed']==1
            state['suspended']+=1;ret()
        else:assert not args[0];state['hook_removed']+=1;ret()
    u.hook_add(UC_HOOK_CODE,hook)
    def invoke(name,*args):
        u.reg_write(UC_ARM64_REG_SP,0x8000f000);u.reg_write(UC_ARM64_REG_LR,0x80000000)
        for i,value in enumerate(args):u.reg_write(UC_ARM64_REG_X0+i,value)
        u.emu_start(image.address(name),0x80000000,count=2000000)
        assert state['exit'] is not None or u.reg_read(UC_ARM64_REG_PC)==0x80000000,(case,'budget exhausted')
        return u.reg_read(UC_ARM64_REG_X0)
    if case in ('recognition-cutoff','finalizing-window','finalizing-expired'):
        state['time']=56000000 if case=='finalizing-window' else 57000000 if case=='finalizing-expired' else 55000000
        if case!='recognition-cutoff':put(image.address('return_state','/apps/os/gpu.c'),1)
        invoke('os_cpu_return_supervise')
        assert state['exit']==(None if case=='finalizing-window' else 0x73c)
        assert not invoke('os_gpu_return_ready') and not state['requests'] and not state['suspended']
        rows.append({'name':case,'passed':True,'terminal_detail':state['exit']})
        continue
    if case=='late-finalizer':state['time']=55000000
    result=invoke('os_gpu_retire')
    success=case in ('opened','thermal-parked','begin-expired')
    assert bool(result)==success and state['suspended']==int(success),(case,result,state)
    assert bool(invoke('os_gpu_return_ready'))==success
    if success:
        raw=bytes(u.mem_read(image.address('os_gpu_return_report'),64))
        assert struct.unpack('<8Q',raw)==(0x3152475033333741,1,64,epoch,generation,7,63,960000)
        (a.output/(case+'-gpu-report.bin')).write_bytes(raw)
        text=b'MODEL ONLY GPU recorded transcript\n';u.mem_write(0x80001000,text)
        state['time']=57000000 if case=='begin-expired' else 56000000
        if case!='begin-expired':invoke('os_cpu_return_supervise')
        invoke('os_cpu_return_begin',0x80001000,len(text))
        assert state['exit']==(0x738 if case=='begin-expired' else None)
        (a.output/(case+'-output.bin')).write_bytes(u.mem_read(image.address('os_cpu_return_output'),48+16384))
        assert state['requests']==([] if case=='thermal-parked' else [2])+[enums['PVR_GPU_SERVICE_PARK']]
    else:assert get(image.address('os_gpu_return_report'),8)==0
    rows.append({'name':case,'passed':True,'requests':state['requests'],'worker_suspensions':state['suspended']})
save(a.output/'results.json',{'physical_access':False,'cases':rows,'caller':caller,'image_sha256':caller['image_sha256']})
print('PASS linked GPU hook and return:',len(rows),'cases')
