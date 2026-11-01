#!/usr/bin/env python3
"""Execute linked two-application preparation, queue binding and peer submission.

Session admission, RTOS task/queue allocation, readiness and device construction
are modeled. The actual application admission, task grants, compute/client bind,
peer registration, context join, submit and strong native readiness hook execute.
No GPU command execution, task scheduling or physical qualification is claimed.
"""
import argparse
import hashlib
from pathlib import Path
import re
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_PC, UC_ARM64_REG_LR, UC_ARM64_REG_SP
from harness import CACHE, Image, hook_addresses, save, skip_crumbs

# Linked image sources whose paths are not named by the boundary model.
CHECK_INPUTS = ('apps/gpu-queue-peer/main.c',)

ROOT=Path(__file__).resolve().parents[1]
TYPES={'ss_application','ss_manifest','ss_grant','ss_gpu_share','gpu_queue_job','gpu_queue_context',
       'gpu_queue_record','gpu_queue_lease_info','board_application','a733_board_session',
       'a733_gpu_queue','a733_gpu_reserved','pvr_compute_queue','pvr_compute_queue_client',
       'pvr_gpu_service','pvr_gpu_service_client'}


def layouts(path):
    image=Image(path,TYPES)
    sizes={};enums={};private=[]
    with path.open('rb') as stream:
        for cu in ELFFile(stream).get_dwarf_info().iter_CUs():
            unit=cu.get_top_DIE().attributes['DW_AT_name'].value.decode()
            for die in cu.iter_DIEs():
                attr=die.attributes.get('DW_AT_name')
                if attr is None:continue
                name=attr.value.decode()
                if die.tag=='DW_TAG_structure_type' and 'DW_AT_byte_size' in die.attributes:
                    sizes[name]=die.attributes['DW_AT_byte_size'].value
                elif die.tag=='DW_TAG_enumerator':enums[name]=die.attributes['DW_AT_const_value'].value
                elif die.tag=='DW_TAG_variable' and name=='gpu' and unit.endswith('/apps/os/gpu.c'):
                    typ=die.get_DIE_from_attribute('DW_AT_type')
                    private.append({m.attributes['DW_AT_name'].value.decode():m.attributes['DW_AT_data_member_location'].value
                                    for m in typ.iter_children() if m.tag=='DW_TAG_member'})
    assert len(private)==1,'Native GPU private state is ambiguous'
    image.layouts['gpu_private']=private[0]
    return image,sizes,enums


class Machine:
    def __init__(self,image,sizes,enums,scheduler):
        self.image=image;self.sizes=sizes;self.enums=enums;self.scheduler=scheduler
        self.u=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
        self.u.mem_map(0x41000000,0x1200000);self.u.mem_map(0x80000000,0x40000)
        for address,data in image.parts:
            if data:self.u.mem_write(address,data)
        skip_crumbs(self.u,image)
        self.current=0x80001000;self.clock=1000000;self.throttled=0
        self.tasks=[];self.queues=[];self.wakes=[];self.strong_entries=0;self.forbidden=[]
        self.gpu=image.address('gpu','/apps/os/gpu.c')
        self.service=self.field(self.gpu,'gpu_private','service')
        self.session=self.field(self.gpu,'gpu_private','session')
        self.component=self.field(self.session,'a733_gpu_queue','component')
        self.config=self.field(self.field(self.gpu,'gpu_private','firmware'),'a733_gpu_reserved','config')
        board=image.address('board')
        self.put(self.field(self.field(board,'board_application','session'),'a733_board_session','deadline_us'),60000000,8)
        self.hooks={}
        names=['xTaskGetSchedulerState','xTaskGetCurrentTaskHandle','os_session_admitted','os_session_devices',
               'os_session_inputs','xTaskCreateStaticAffinitySet','xQueueGenericCreateStatic','xQueueGenericSend',
               'runtime_init','runtime_smp_init','board_live','os_thermal_throttled','a733_smp_counter',
               'pvr_compute_queue_runtime_sync','os_gpu_queue_thermal','pvr_gpu_service_send_epoch','board_fail_at']
        for name in names:
            if name in image.symbols and image.symbols[name]-{0}:self.hooks[image.address(name)]=name
        hook_addresses(self.u,self.hooks,self.hook)

    def field(self,base,typ,member):return base+self.image.layouts[typ][member]
    def put(self,address,value,size=4):self.u.mem_write(address,int(value).to_bytes(size,'little'))
    def get(self,address,size=4):return int.from_bytes(self.u.mem_read(address,size),'little')
    def set(self,base,typ,member,value,size=4):self.put(self.field(base,typ,member),value,size)
    def read(self,base,typ,member,size=4):return self.get(self.field(base,typ,member),size)
    def ret(self,value=0):
        self.u.reg_write(UC_ARM64_REG_X0,value)
        self.u.reg_write(UC_ARM64_REG_PC,self.u.reg_read(UC_ARM64_REG_LR))

    def hook(self,u,address,size,unused):
        name=self.hooks[address];args=[u.reg_read(UC_ARM64_REG_X0+n) for n in range(8)]
        if name=='xTaskGetSchedulerState':self.ret(self.scheduler)
        elif name=='xTaskGetCurrentTaskHandle':self.ret(self.current)
        elif name=='os_session_admitted':self.ret(0x80002000)
        elif name=='os_session_devices':self.ret(self.enums['SS_DEVICE_GPU'])
        elif name=='os_session_inputs':self.put(args[0],0);self.ret()
        elif name=='xTaskCreateStaticAffinitySet':
            self.tasks.append(dict(entry=args[0],stack_words=args[2],priority=args[4],stack=args[5],task=args[6],affinity=args[7]))
            self.ret(args[6])
        elif name=='xQueueGenericCreateStatic':
            assert args[3] and args[2] and all(q['handle']!=args[3] for q in self.queues),'Queue storage aliases another queue'
            self.queues.append(dict(length=args[0],item_bytes=args[1],storage=args[2],handle=args[3]))
            self.ret(args[3])
        elif name=='xQueueGenericSend':
            self.wakes.append(dict(queue=args[0],bytes=bytes(u.mem_read(args[1],16)).hex()))
            self.ret(1)
        elif name in ('runtime_init','runtime_smp_init'):self.ret()
        elif name=='board_live':self.ret(1)
        elif name=='os_thermal_throttled':self.ret(self.throttled)
        elif name=='a733_smp_counter':self.ret(self.clock*24)
        elif name=='pvr_compute_queue_runtime_sync':self.strong_entries+=1
        else:
            self.forbidden.append(name)
            raise AssertionError('Unexpected native lifecycle/device action: '+name)

    def call(self,name,*args,unit=None):
        self.u.reg_write(UC_ARM64_REG_SP,0x8003f000)
        self.u.reg_write(UC_ARM64_REG_LR,0x80000000)
        for n in range(8):self.u.reg_write(UC_ARM64_REG_X0+n,args[n] if n<len(args) else 0)
        self.u.emu_start(self.image.address(name,unit),0x80000000,count=2000000)
        assert self.u.reg_read(UC_ARM64_REG_PC)==0x80000000,(name,'instruction budget exhausted')
        return self.u.reg_read(UC_ARM64_REG_X0)&0xffffffff


def run(path,task_header):
    image,sizes,enums=layouts(path)
    text=task_header.read_text()
    def scheduler(name):return int(re.search(r'#define\s+'+name+r'\s+\(\s*\(\s*BaseType_t\s*\)\s*(\d+)\s*\)',text)[1])
    machine=Machine(image,sizes,enums,scheduler('taskSCHEDULER_NOT_STARTED'))
    assert machine.call('os_application_prepare')==1,'Actual application preparation refused'
    assert len(machine.tasks)==2,'Composition must create exactly two application tasks'
    owner_task=image.address('main_task','/apps/os/application.c')
    peer_task=image.address('peer_task','/apps/os/application.c')
    owner=machine.get(owner_task,8);peer=machine.get(peer_task,8)
    assert owner and peer and owner!=peer
    owner_grant=image.address('grant','/apps/os/application.c')
    peer_grant=image.address('peer_grant','/apps/os/application.c')
    owner_core=machine.read(owner_grant,'ss_grant','main_core')
    peer_core=machine.read(peer_grant,'ss_grant','main_core')
    assert owner_core!=peer_core
    assert machine.read(owner_grant,'ss_grant','worker_cores')&(1<<peer_core)==0
    assert machine.read(peer_grant,'ss_grant','devices')==enums['SS_DEVICE_GPU'] and machine.read(peer_grant,'ss_grant','worker_cores')==0
    by_task={t['task']:t for t in machine.tasks}
    assert by_task[owner]['affinity']==1<<owner_core and by_task[peer]['affinity']==1<<peer_core
    assert machine.call('pvr_compute_queue_runtime_program',machine.get(machine.field(image.address('ss_gpu_share'),'ss_gpu_share','program'),8))==1
    assert machine.call('pvr_compute_queue_runtime_program',0x80003000)==0
    # Model reserved payload ownership, then execute the actual queue constructor.
    machine.set(machine.component,'pvr_compute_queue','owner',machine.session,8)
    assert machine.call('pvr_compute_queue_bind',machine.component,machine.service,machine.config)==1
    assert len(machine.queues)==1 and machine.queues[0]['item_bytes']==sizes['gpu_queue_record']
    # Device/service startup is outside this check. Its immutable identity and
    # initialized metadata suffice to execute the actual service task bindings.
    machine.set(machine.service,'pvr_gpu_service','initialized',1)
    machine.set(machine.service,'pvr_gpu_service','worker',0x80003100,8)
    machine.set(machine.service,'pvr_gpu_service','requests',0x80003200,8)
    machine.put(image.address('instance','/drivers/gpu/pvr_gpu_service.c'),machine.service,8)
    assert machine.call('pvr_gpu_service_bind',machine.service,0,owner)==1
    assert machine.call('pvr_compute_queue_client_bind',machine.component)==1
    assert machine.call('os_application_gpu_share_bind',machine.component)==1
    assert len(machine.queues)==2,'Peer registration must create its completion queue'
    client=machine.field(machine.component,'pvr_compute_queue','peer')
    owner_queue=machine.read(machine.component,'pvr_compute_queue','completions',8)
    peer_queue=machine.read(client,'pvr_compute_queue_client','completions',8)
    assert owner_queue and peer_queue and owner_queue!=peer_queue
    assert {owner_queue,peer_queue}=={q['handle'] for q in machine.queues}
    assert all(q['length']==1 and q['item_bytes']==sizes['gpu_queue_record'] for q in machine.queues)
    assert machine.read(client,'pvr_compute_queue_client','task',8)==peer
    machine.scheduler=scheduler('taskSCHEDULER_RUNNING')
    machine.current=peer
    assert machine.call('ss_grant')==peer_grant
    machine.current=owner
    assert machine.call('ss_grant')==owner_grant
    # Model the worker's successful CREATE/OPEN publication coherently. The
    # device map and firmware execution remain outside this admission check.
    machine.set(machine.service,'pvr_gpu_service','phase',enums['PVR_GPU_SERVICE_READY'])
    machine.set(machine.service,'pvr_gpu_service','owner',0)
    machine.set(machine.service,'pvr_gpu_service','epoch',1,8)
    machine.set(machine.service,'pvr_gpu_service','published_epoch',1,8)
    machine.set(machine.component,'pvr_compute_queue','phase',enums['PVR_COMPUTE_QUEUE_READY'])
    machine.set(machine.component,'pvr_compute_queue','epoch',1,8)
    machine.set(machine.component,'pvr_compute_queue','slot_state',enums['PVR_COMPUTE_QUEUE_IDLE'])
    machine.set(machine.gpu,'gpu_private','epoch',1,8)
    machine.set(machine.component,'pvr_compute_queue','create_done',1)
    machine.set(machine.component,'pvr_compute_queue','generation',9)
    machine.set(machine.component,'pvr_compute_queue','lease_count',1)
    lease=machine.field(machine.component,'pvr_compute_queue','lease')
    machine.set(lease,'gpu_queue_lease_info','address',0x44000000,8)
    machine.set(lease,'gpu_queue_lease_info','bytes',4096,8)
    machine.set(client,'pvr_compute_queue_client','slot_state',enums['PVR_COMPUTE_QUEUE_IDLE'])
    machine.set(machine.gpu,'gpu_private','component',machine.component,8)
    machine.set(machine.gpu,'gpu_private','started',1)
    machine.set(machine.gpu,'gpu_private','opened',1)
    context=0x80006000;info=0x80006100;job=0x80006200
    machine.current=0x80003300
    assert machine.call('gpu_queue_context_join',context,info)==enums['GPU_QUEUE_REFUSED']
    machine.current=peer
    assert machine.call('gpu_queue_context_join',context,info)==enums['GPU_QUEUE_OK']
    assert machine.read(context,'gpu_queue_context','generation')==9 and machine.read(context,'gpu_queue_context','client')==1
    handle=machine.get(context,8)
    machine.set(job,'gpu_queue_job','sequence',1)
    machine.set(job,'gpu_queue_job','bindings',1,2)
    for n in range(3):machine.put(machine.field(job,'gpu_queue_job','groups')+4*n,1)
    machine.throttled=1
    assert machine.call('gpu_queue_submit',handle,job)==enums['GPU_QUEUE_PARKED']
    assert not machine.read(client,'pvr_compute_queue_client','client_owed')
    machine.throttled=0
    assert machine.call('gpu_queue_submit',handle,job)==enums['GPU_QUEUE_OK']
    assert machine.read(client,'pvr_compute_queue_client','client_owed')==1
    assert machine.read(client,'pvr_compute_queue_client','slot_state')==enums['PVR_COMPUTE_QUEUE_SUBMITTED']
    assert machine.strong_entries==2 and len(machine.wakes)==1 and not machine.forbidden
    return dict(passed=True,image_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                task_header_sha256=hashlib.sha256(task_header.read_bytes()).hexdigest(),
                application_tasks=machine.tasks,completion_queues=machine.queues,
                owner_core=owner_core,peer_core=peer_core,strong_runtime_entries=machine.strong_entries,
                refused=['unregistered-join','unapproved-program','thermally-parked-submit'],
                peer_submitted=True,lifecycle_actions=machine.forbidden,physical_access=False,scope=__doc__)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--task-header',type=Path,default=CACHE/'sources/FreeRTOS-Kernel-97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2/include/task.h')
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    result=run(a.image,a.task_header);save(a.output/'result.json',result)
    print('PASS actual native GPU peer admission, two constructed completion queues, task grants, join and strong readiness submission')


if __name__=='__main__':main()
