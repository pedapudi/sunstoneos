#!/usr/bin/env python3
"""Execute the linked finite client with modeled service and stop replies."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR
p=argparse.ArgumentParser(description=__doc__)
for name in ('elf','evidence'):p.add_argument('--'+name,type=Path,required=True)
a=p.parse_args();r=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('layout',r/'checks/storage-read-native.py');layout=importlib.util.module_from_spec(spec);spec.loader.exec_module(layout)
image=layout.Image(a.elf,layout_names=('a733_observe_identity','npu_service_reply'));cases=[]
for variant in ('matched','mismatch','close-fault','stop-never-ready','quiesce-failed','quiescence-lost'):
    u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0x40000000,0x04000000)
    for at,data in image.parts:
        if data:u.mem_write(at,data)
    def put(at,value,n=8):u.mem_write(at,int(value).to_bytes(n,'little'))
    def get(at,n=8):return int.from_bytes(u.mem_read(at,n),'little')
    def ret(value=0):u.reg_write(UC_ARM64_REG_X0,value);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
    state={'ticks':24000000,'ticket':0,'job':0,'operation':None,'submits':0,'reaps':0,'copied':0,'closed':False,'stop_queries':0,'exit':None,'retained':False,'quiesced':False,'critical':False,'operations':[]}
    c=0x43000000
    put(image.address('a733_observe_identity')+image.layouts['a733_observe_identity']['deadline'],480000000)
    names=['a733_npu_application_ready','a733_npu_application_client','npu_convolution_send','npu_convolution_collect','npu_convolution_output','a733_npu_application_stopped','a733_npu_application_quiesce','a733_npu_application_quiescent','vPortEnterCritical','platform_exit','vTaskSuspend','vTaskDelay','a733_npu_application_fail']
    hooks={image.address(name):name for name in names}
    reply=image.layouts['npu_service_reply']
    def code(cpu,at,size,cookie):
        word=int.from_bytes(u.mem_read(at,4),'little')
        if word&0xffffffe0==0xd53be020:
            u.reg_write(UC_ARM64_REG_X0+(word&31),state['ticks']);state['ticks']+=24
            u.reg_write(UC_ARM64_REG_PC,at+4);return
        name=hooks.get(at)
        if name is None:return
        args=[u.reg_read(UC_ARM64_REG_X0+i) for i in range(7)]
        if name=='a733_npu_application_ready':ret(1)
        elif name=='a733_npu_application_client':ret(c)
        elif name=='npu_convolution_send':
            assert args[0]==c
            op=args[1];state['operation']=op;state['operations'].append(op);state['ticket']+=1;put(args[6],state['ticket'])
            if op==2:
                assert not state['closed'];state['submits']+=1;state['job']=state['ticket']
            if op==4:assert state['copied']==state['submits'];state['reaps']+=1
            if op==5:assert state['reaps']==2 and state['copied']==2
            ret(1)
        elif name=='npu_convolution_collect':
            op=state['operation'];out=args[2];u.mem_write(out,bytes(40));put(out+reply['ticket'],args[1])
            result=4 if op==5 and variant=='close-fault' else 0
            st=9 if op==5 else 3 if op==2 else 6 if op==6 else 2
            for key,value in [('job_ticket',0 if op in (1,5) else state['job']),('result',result),('state',st),('owns_session',0 if op==5 and not result else 1)]:put(out+reply[key],value,8 if key=='job_ticket' else 4)
            if op==5 and not result:state['closed']=True
            ret(1)
        elif name=='npu_convolution_output':
            assert args[0]==c and state['job'];state['copied']+=1
            data=bytes(u.mem_read(image.address('a733_npu_application_reference'),args[2]))
            if variant=='mismatch' and state['copied']==1:data=bytes([data[0]^1])+data[1:]
            u.mem_write(args[1],data);ret(1)
        elif name=='a733_npu_application_stopped':
            assert state['closed'];state['stop_queries']+=1
            ret(int(variant!='stop-never-ready' and state['stop_queries']>=3))
        elif name=='a733_npu_application_quiesce':
            assert state['closed'] and state['stop_queries']>=3
            state['quiesced']=variant!='quiesce-failed';ret(int(state['quiesced']))
        elif name=='a733_npu_application_quiescent':ret(int(state['quiesced'] and variant!='quiescence-lost'))
        elif name=='vPortEnterCritical':
            assert state['quiesced'] and variant!='quiescence-lost';state['critical']=True;ret()
        elif name=='vTaskDelay':
            state['ticks']+=240000000 if variant=='stop-never-ready' and state['closed'] else 24000;ret()
        elif name=='a733_npu_application_fail':put(image.address('a733_npu_application_failed'),args[0],4);ret()
        elif name=='vTaskSuspend':state['retained']=True;u.emu_stop()
        elif name=='platform_exit':
            assert state['closed'] and state['stop_queries']>=3 and state['quiesced'] and state['critical']
            state['exit']=args[0];u.emu_stop()
    u.hook_add(UC_HOOK_CODE,code);u.reg_write(UC_ARM64_REG_SP,0x43ffe000);u.reg_write(UC_ARM64_REG_LR,0x43fff000)
    u.emu_start(image.address('client',unit='/apps/npu-convolution/main.c'),0,count=3000000)
    if variant in ('matched','mismatch'):assert state['exit']==(variant=='mismatch') and not state['retained'],state
    else:assert state['retained'] and state['exit'] is None and not state['critical'],state
    assert state['submits']==2 and state['copied']==2 and state['reaps']==2,state
    assert state['operations']==[1,2,6,4,2,6,4,5],state
    cases.append({'name':variant,'passed':True,**state})
a.evidence.mkdir(parents=True,exist_ok=True)
(a.evidence/'client-result.json').write_text(json.dumps({'elf_sha256':hashlib.sha256(a.elf.read_bytes()).hexdigest(),'cases':cases,'scope':'Actual finite client/request/output comparison/logging instructions. Service, scheduler, counter and bootstrap stop replies modeled. No native provider, hardware inference or ROM-return qualification.'},indent=2)+'\n')
print('Six finite NPU client cases passed')
