#!/usr/bin/env python3
"""Run actual application finish and CPU return with an unconstructed GPU.

Admission globals, the clock and current task are modeled. The linked GPU
startup/retirement, result writer, receipt producer and CPU-return gate execute.
Both an absent GPU grant and a granted GPU whose constructor never ran must
return an untouched receipt. Wrong ownership, missing admission, expired time
or nonzero GPU state must refuse before device or worker callbacks. Firmware,
scheduling and physical recovery remain outside this check.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
from elftools.elf.elffile import ELFFile
from harness import machine as harness_machine, Image, hook_addresses, refused, save, skip_crumbs
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_PC, UC_ARM64_REG_LR, UC_ARM64_REG_SP

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('gpu_return_rules',ROOT/'apps/os/gpu-return.py')
decoder=importlib.util.module_from_spec(spec);spec.loader.exec_module(decoder)


def require(ok,reason):
    if not ok:raise ValueError(reason)


def run_image(path,output):
    image=Image(path,{'ss_application','ss_grant','ss_result','board_application','a733_board_session',
                      'a733_gpu_platform','a733_perf_control'})
    private=[]
    with path.open('rb') as stream:
        for cu in ELFFile(stream).get_dwarf_info().iter_CUs():
            unit=cu.get_top_DIE().attributes['DW_AT_name'].value.decode()
            if not unit.endswith('/apps/os/gpu.c'):continue
            for die in cu.iter_DIEs():
                if die.tag!='DW_TAG_variable' or die.attributes.get('DW_AT_name') is None or die.attributes['DW_AT_name'].value!=b'gpu':continue
                typ=die.get_DIE_from_attribute('DW_AT_type')
                private.append((typ.attributes['DW_AT_byte_size'].value,
                    {m.attributes['DW_AT_name'].value.decode():m.attributes['DW_AT_data_member_location'].value
                     for m in typ.iter_children() if m.tag=='DW_TAG_member'}))
    assert len(private)==1,'Private GPU state must be unambiguous'
    gpu_bytes,gpu_fields=private[0]
    cases=['no-gpu-grant','granted-never-constructed','wrong-task','missing-admission','null-application-task',
           'bound-gpu-client','expired-cutoff','nonzero-platform-performance']
    cases += ['nonzero-'+field for field in gpu_fields]
    rows=[]
    for case in cases:
        u=harness_machine();u.mem_map(0x41000000,0x1200000);u.mem_map(0x80000000,0x20000)
        for address,data in image.parts:
            if data:u.mem_write(address,data)
        skip_crumbs(u,image)
        def put(address,value,size=4):u.mem_write(address,int(value).to_bytes(size,'little'))
        def get(address,size=4):return int.from_bytes(u.mem_read(address,size),'little')
        def field(base,typ,name):return base+image.layouts[typ][name]
        gpu=image.address('gpu','/apps/os/gpu.c')
        app=image.address('ss_application');app_task_address=field(app,'ss_application','task');task=get(app_task_address,8)
        assert task,'Image must register an application task'
        grant=image.address('grant','/apps/os/application.c')
        prepared=image.address('prepared','/apps/os/application.c')
        put(image.address('main_task','/apps/os/application.c'),task,8)
        board=image.address('board');session=field(board,'board_application','session')
        put(field(session,'a733_board_session','deadline_us'),60000000,8)
        put(field(session,'a733_board_session','epoch'),123,8);put(field(session,'a733_board_session','generation'),456,8)
        put(field(board,'board_application','live_until'),60000000,8)
        put(prepared,1);put(field(grant,'ss_grant','devices'),16 if case=='granted-never-constructed' else 0)
        assert bytes(u.mem_read(gpu,gpu_bytes))==bytes(gpu_bytes),'GPU state must start wholly zero'
        state={'clock_us':1000000,'task':task,'terminal':None,'cpu_return_entries':0,'retire_entries':0,'quiesce_entries':0,'worker_callbacks':[]}
        if case=='wrong-task':state['task']=task+16
        elif case=='missing-admission':put(prepared,0)
        elif case=='null-application-task':put(app_task_address,0,8)
        elif case=='bound-gpu-client':put(image.address('os_gpu_client_task','/apps/os/gpu.c'),task,8)
        elif case=='expired-cutoff':state['clock_us']=55000000
        elif case=='nonzero-platform-performance':put(gpu+gpu_fields['power']+image.layouts['a733_gpu_platform']['perf'],1,1)
        elif case.startswith('nonzero-'):put(gpu+gpu_fields[case.removeprefix('nonzero-')],1,1)
        callbacks=('eTaskGetState','uxQueueMessagesWaiting','pvr_gpu_service_send_epoch',
                   'pvr_gpu_service_collect','a733_gpu_backend_stopped')
        names=['a733_smp_counter','ucPortGetCoreID','xTaskGetCurrentTaskHandle','board_live','os_gpu_queue_quiesce','board_fail','platform_exit',
               'vTaskSuspend','os_gpu_retire','os_cpu_return_begin',*callbacks]
        hooks={image.address(name):name for name in names}
        def ret(value=0):u.reg_write(UC_ARM64_REG_X0,value);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
        def hook(machine,address,size,unused):
            name=hooks[address];arg=machine.reg_read(UC_ARM64_REG_X0)
            if name=='a733_smp_counter':ret(state['clock_us']*24)
            elif name=='ucPortGetCoreID':ret(6)         # the application's core: the scheduler port reads it from a user-visible register
            elif name=='xTaskGetCurrentTaskHandle':ret(state['task'])
            elif name=='os_cpu_return_begin':state['cpu_return_entries']+=1
            elif name=='os_gpu_retire':state['retire_entries']+=1
            elif name=='os_gpu_queue_quiesce':state['quiesce_entries']+=1
            elif name=='board_live':ret(1)
            elif name=='board_fail':state['terminal']='board-fail';machine.emu_stop()
            elif name=='platform_exit':state['terminal']='exit-'+hex(arg);machine.emu_stop()
            elif name=='vTaskSuspend':
                if arg:state['worker_callbacks'].append(name)
                state['terminal']='application-suspended' if not arg else 'worker-suspended';machine.emu_stop()
            else:state['worker_callbacks'].append(name);state['terminal']='unexpected-worker-callback';machine.emu_stop()
        hook_addresses(u,hooks,hook)
        def call(name,*args,terminal=False):
            u.reg_write(UC_ARM64_REG_SP,0x8001f000);u.reg_write(UC_ARM64_REG_LR,0x80000000)
            for i,arg in enumerate(args):u.reg_write(UC_ARM64_REG_X0+i,arg)
            # Finish now checks wholly zero GPU state in both generic
            # quiescence and untouched retirement. Bound those byte walks
            # by the exact linked state size as well as fixed call overhead.
            u.emu_start(image.address(name),0x80000000,count=1000000+24*gpu_bytes)
            assert (state['terminal'] is not None) if terminal else u.reg_read(UC_ARM64_REG_PC)==0x80000000,(case,name,'execution budget or unexpected terminal')
            return u.reg_read(UC_ARM64_REG_X0)&0xffffffff
        if case=='no-gpu-grant':assert call('os_gpu_queue_start_entry')==1
        capacity=get(image.address('ss_result_capacity'),8)
        payload=b'GPU-free application result\n' if capacity>=28 else b'GPU-free'
        assert 0<len(payload)<=capacity,(str(path),'fixture payload exceeds registered result capacity')
        u.mem_write(0x80001000,payload)
        call('ss_finish',0x80001000,len(payload),terminal=True)
        success=case in ('no-gpu-grant','granted-never-constructed')
        result_address=image.address('ss_result_record');receipt=bytes(u.mem_read(image.address('os_gpu_return_report'),64))
        finished=get(image.address('finished','/apps/os/application.c'))
        closing=get(image.address('closing','/apps/os/cpu-return.c'))
        passed=not state['worker_callbacks'] and state['retire_entries']<=1 and state['quiesce_entries']<=1
        if success:
            passed=passed and state['retire_entries']==1 and state['quiesce_entries']==1 and state['terminal']=='application-suspended' and state['cpu_return_entries']==1 and finished==1 and closing==1
            if passed:
                result=bytes(u.mem_read(result_address,56+len(payload)))
                assert struct.unpack_from('<7Q',result)==(0x31544c5553455241,1,
                    56+get(image.address('ss_result_capacity'),8),123,456,1,len(payload))
                assert result[56:]==payload
                decoded=decoder.decode(receipt,123,456,require,allow_untouched=True)
                assert decoded=={'jobs':0,'gpu_untouched':True,'device_never_opened':True,'qualification_passed':False}
                refused(lambda:decoder.decode(receipt,123,456,require),'not admitted')
                (output/(path.stem+'-'+case+'-receipt.bin')).write_bytes(receipt)
                (output/(path.stem+'-'+case+'-result.bin')).write_bytes(result)
        else:
            passed=passed and state['terminal']=='board-fail' and state['cpu_return_entries']==0 and finished==0 and closing==0 and receipt==bytes(64)
        rows.append(dict(case=case,passed=passed,expected_success=success,payload_bytes=len(payload),result_capacity=capacity,finished=finished,closing=closing,model=state))
    return dict(image=str(path),image_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),cases=rows,passed=all(r['passed'] for r in rows))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image',type=Path,nargs='+',required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    results=[]
    for path in args.image:
        results.append(run_image(path,args.output))
        save(args.output/'results.json',dict(physical_access=False,scope=__doc__,images=results,passed=all(r['passed'] for r in results)))
    assert all(r['passed'] for r in results),[(r['image'],[v['case'] for v in r['cases'] if not v['passed']]) for r in results]
    print('PASS unconstructed GPU finish:',sum(len(r['cases']) for r in results),'cases over',len(results),'images')


if __name__=='__main__':main()
