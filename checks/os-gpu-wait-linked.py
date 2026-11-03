#!/usr/bin/env python3
"""Exercise the linked GPU queue client with modeled scheduler and service queues.

Checks finite waits, late record collection, fault ownership, stale handles and
wrong callers. The actual public queue API executes; the worker, firmware,
cache effects and GPU results are model boundaries.
"""
import argparse
import hashlib
import json
from pathlib import Path
from elftools.elf.elffile import ELFFile
from harness import machine as harness_machine, Image, hook_addresses, skip_crumbs
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_SP, UC_ARM64_REG_LR, UC_ARM64_REG_PC

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--image',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args()
image=Image(a.image,['pvr_compute_queue','pvr_gpu_service','pvr_gpu_service_client','gpu_queue_job',
                     'gpu_queue_binding','gpu_queue_lease_info','gpu_queue_record','gpu_queue_program','gpu_queue_context_config'])
enums={}
with a.image.open('rb') as stream:
    for cu in ELFFile(stream).get_dwarf_info().iter_CUs():
        for die in cu.iter_DIEs():
            if die.tag=='DW_TAG_enumerator':
                enums[die.attributes['DW_AT_name'].value.decode()]=die.attributes['DW_AT_const_value'].value

def ev(name):return enums[name]
results=[]
for case in ('timely-record','timeout-then-late-record','owed-fault-record','wrong-caller','stale-handle',
             'parked-submit','revoked-submit','invalid-bindings','create-ticket-timeout','create-ready-timeout'):
    u=harness_machine()
    u.mem_map(0x41000000,0x1200000)
    for address,data in image.parts:
        if data:u.mem_write(address,data)
    u.mem_map(0x80000000,0x100000)
    skip_crumbs(u,image)
    def put(address,value,size=4):u.mem_write(address,int(value).to_bytes(size,'little'))
    def get(address,size=4):return int.from_bytes(u.mem_read(address,size),'little')
    def field(base,typ,name):return base+image.layouts[typ][name]
    def sf(base,typ,name,value,size=4):put(field(base,typ,name),value,size)
    def ret(value=0):
        u.reg_write(UC_ARM64_REG_X0,value);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
    def call(name,*args):
        u.reg_write(UC_ARM64_REG_SP,0x800ff000);u.reg_write(UC_ARM64_REG_LR,0x80000000)
        for i,value in enumerate(args):u.reg_write(UC_ARM64_REG_X0+i,value)
        u.emu_start(image.address(name),0x80000000,count=1000000)
        assert u.reg_read(UC_ARM64_REG_PC)==0x80000000,(case,name,'instruction budget')
        return u.reg_read(UC_ARM64_REG_X0)&0xffffffff
    q=0x80010000;service=0x80030000;job=0x80050000;output=0x80051000
    state={'scheduler':1,'task':0x1234,'ticks':0,'receive':0,'sends':0,'collect':0,'available':True,'live':1}
    names=('xTaskGetSchedulerState','xTaskGetCurrentTaskHandle','xTaskGetTickCount',
           'xQueueGenericSend','xQueueReceive','pvr_compute_queue_runtime_sync',
           'pvr_gpu_service_send_epoch','pvr_gpu_service_collect','pvr_compute_queue_runtime_opened')
    hooks={image.address(n):n for n in names}
    def hook(machine,address,size,unused):
        name=hooks[address];args=[machine.reg_read(UC_ARM64_REG_X0+i) for i in range(5)]
        if name=='xTaskGetSchedulerState':ret(state['scheduler'])
        elif name=='xTaskGetCurrentTaskHandle':ret(state['task'])
        elif name=='xTaskGetTickCount':ret(state['ticks'])
        elif name=='pvr_compute_queue_runtime_sync':ret(state['live']&0xffffffff)
        elif name=='pvr_compute_queue_runtime_opened':ret()
        elif name=='xQueueGenericSend':state['sends']+=1;ret(1)
        elif name=='pvr_gpu_service_send_epoch':
            state['sends']+=1;put(args[4],77,8);ret(1)
        elif name=='pvr_gpu_service_collect':
            state['collect']+=1
            if not state['available']:state['ticks']+=args[3];ret(0)
            else:
                # Public creation checks result; epoch comes from published state.
                # Layout is from this exact linked image.
                typ='pvr_gpu_service_reply'
                sf(args[2],typ,'result',ev('PVR_GPU_SERVICE_OK'))
                ret(1)
        elif name=='xQueueReceive':
            state['receive']+=1
            if not state['available']:state['ticks']+=args[2];ret(0);return
            sequence=0 if case.startswith('create-') else 1
            result=ev('GPU_QUEUE_FAULT') if case=='owed-fault-record' else ev('GPU_QUEUE_OK')
            sf(args[1],'gpu_queue_record','sequence',sequence)
            sf(args[1],'gpu_queue_record','result',result,2)
            sf(args[1],'gpu_queue_record','detail',ev('GPU_QUEUE_RESET') if result==ev('GPU_QUEUE_FAULT') else 0,2)
            ret(1)
        else:raise AssertionError(name)
    image.layouts.update(Image(a.image,['pvr_gpu_service_reply']).layouts)
    hook_addresses(u,hooks,hook)
    sf(q,'pvr_compute_queue','service',service,8)
    clients=field(service,'pvr_gpu_service','clients')
    sf(clients,'pvr_gpu_service_client','task',0x1234,8)
    sf(q,'pvr_compute_queue','completions',0x5678,8)
    sf(service,'pvr_gpu_service','requests',0x6789,8)
    sf(service,'pvr_gpu_service','published_epoch',1,8)
    assert call('pvr_compute_queue_client_bind',q)==1
    state['scheduler']=2
    if case.startswith('create-'):
        config=0x80060000;program=0x80061000
        sf(config,'gpu_queue_context_config','program',program,8)
        sf(config,'gpu_queue_context_config','lease_count',1)
        state['available']=False
        if case=='create-ready-timeout':
            # First collect succeeds, but the ready record remains absent.
            sf(q,'pvr_compute_queue','create_pending',2)
        assert call('gpu_queue_context_create',config,10,output,output+64)==ev('GPU_QUEUE_TIMEOUT')
        ticket=get(field(q,'pvr_compute_queue','create_ticket'),8)
        assert bool(ticket)==(case=='create-ticket-timeout')
        sent=state['sends'];state['available']=True
        sf(q,'pvr_compute_queue','generation',1)
        assert call('gpu_queue_context_create',config,10,output,output+64)==ev('GPU_QUEUE_OK')
        assert state['sends']==sent and get(output)==1
    else:
        sf(q,'pvr_compute_queue','generation',1)
        sf(q,'pvr_compute_queue','create_done',1)
        sf(q,'pvr_compute_queue','lease_count',1)
        sf(field(q,'pvr_compute_queue','lease'),'gpu_queue_lease_info','bytes',2097152,8)
        sf(q,'pvr_compute_queue','slot_state',ev('PVR_COMPUTE_QUEUE_IDLE'))
        sf(job,'gpu_queue_job','sequence',1)
        sf(job,'gpu_queue_job','bindings',1,2)
        for i in range(3):put(field(job,'gpu_queue_job','groups')+4*i,1)
        if case=='wrong-caller':state['task']=0xabcd
        if case=='parked-submit':state['live']=0
        if case=='revoked-submit':state['live']=-1
        if case=='invalid-bindings':sf(job,'gpu_queue_job','bindings',5,2)
        result=call('gpu_queue_submit',2 if case=='stale-handle' else 1,job)
        expected={'wrong-caller':'GPU_QUEUE_REFUSED','stale-handle':'GPU_QUEUE_REFUSED',
                  'parked-submit':'GPU_QUEUE_PARKED','revoked-submit':'GPU_QUEUE_FAULT',
                  'invalid-bindings':'GPU_QUEUE_REFUSED'}.get(case,'GPU_QUEUE_OK')
        assert result==ev(expected),(case,result,expected)
        if expected!='GPU_QUEUE_OK':assert not state['sends'] and not state['receive']
        else:
            assert get(field(q,'pvr_compute_queue','client_owed'))==1
            assert call('gpu_queue_submit',1,job)==ev('GPU_QUEUE_BUSY')
            put(output,0xabcdef)
            if case=='timeout-then-late-record':
                state['available']=False
                assert call('gpu_queue_wait',1,output,10)==ev('GPU_QUEUE_TIMEOUT')
                assert get(output)==0xabcdef and get(field(q,'pvr_compute_queue','client_owed'))==1
                state['available']=True
            if case=='owed-fault-record':sf(q,'pvr_compute_queue','lost',1)
            assert call('gpu_queue_wait',1,output,10)==ev('GPU_QUEUE_FAULT' if case=='owed-fault-record' else 'GPU_QUEUE_OK')
            assert get(output)==1 and get(field(q,'pvr_compute_queue','client_owed'))==0
    results.append(dict(case=case,passed=True,model=state))
a.output.parent.mkdir(parents=True,exist_ok=True)
a.output.write_text(json.dumps(dict(passed=True,physical_access=False,
    image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(),cases=results,scope=__doc__),indent=2)+'\n')
print('PASS linked GPU queue client:',len(results),'cases')
