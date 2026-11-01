#!/usr/bin/env python3
"""Run the native cancellation client, worker, IRQ binding and return instructions.

Starts from the retained provider READY boundary already covered by startup
fixtures. Kernel queues, scheduling, translation, counters and hardware effects
are modeled; the client, service/controller, native memory, IRQ callbacks,
local stop, provider park, output comparison and return code remain linked.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import *

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'checks'))
import harness

def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value

class Model:
    STOP=0x43fff000
    def __init__(self,image,wrong_output=False,failure=None):
        self.image=image;self.L=image.layouts;self.u=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
        self.u.mem_map(0x02000000,0x07000000);self.u.mem_map(0x40000000,0x04000000)
        for address,data in image.parts:
            if data:self.u.mem_write(address,data)
        self.time=11000000;self.running=False;self.current='setup';self.contexts={};self.tasks={};self.queues={}
        self.handles={'worker':0x43500000,'client':0x43501000,'bootstrap':0x43502000,'pmic':0x43503000,'irq':0x43504000}
        self.wrong_output=wrong_output;self.events=[];self.writes=[];self.stages=[];self.returned=False;self.retained=False
        self.failure=failure;self.checkpoint_injected=False
        self.pending_event=None;self.cancel_release=None;self.launches=[];self.irq_due=False;self.level=4;self.svc=[];self.cache_lines=0;self.sctlr=0x1005;self.power_steps=0
        self.power_step_times=[];self.notifies=[];self.posted=[];self.completed=[];self.dsb=[]
        self.app=image.address('app',unit='apps/npu-convolution/native.c')
        with image.path.open('rb') as stream:
            for cu in ELFFile(stream).get_dwarf_info().iter_CUs():
                for die in cu.iter_DIEs():
                    if die.tag!='DW_TAG_structure_type':continue
                    fields={d.attributes['DW_AT_name'].value.decode():d.attributes['DW_AT_data_member_location'].value for d in die.iter_children()
                        if d.tag=='DW_TAG_member' and 'DW_AT_name' in d.attributes and 'DW_AT_data_member_location' in d.attributes}
                    if {'used','reserved','bootstrap_task','pmic_task','client'}<=fields.keys():self.A=fields
                    if {'npu_irq','spi_enabled','banks'}<=fields.keys():self.saved_irq=fields['npu_irq']
        self.reserved=self.app+self.A['reserved'];self.c=self.reserved+self.L['a733_npu_reserved']['convolution']
        self.service=self.c+self.L['npu_convolution']['service'];self.startup=self.app+self.A['startup']
        self.pool=0x43600000
        hooks=('xTaskGetSchedulerState','xTaskGetCurrentTaskHandle','xTaskGetTickCount','xQueueGenericCreateStatic',
            'xQueueGenericSend','xQueueReceive','uxQueueMessagesWaiting','xTaskCreateStatic','xTaskCreateStaticAffinitySet',
            'xTaskGenericNotify','ulTaskGenericNotifyTake','vTaskGenericNotifyGiveFromISR','vTaskDelay','vTaskSuspend',
            'vPortEnterCritical','vPortExitCritical','ulPortSetInterruptMask','uxPortSetInterruptMask','vPortClearInterruptMask','vPortYieldFromISR',
            'aarch64_device_translate','aarch64_device_cache_stride','a733_memory_reservations_init',
            'a733_counter_clock_now','a733_clock_token_status','a733_rail_status','a733_power_domain_check',
            'a733_platform_lock_enter','a733_platform_lock_leave','a733_platform_lock_retain',
            'xQueueGetMutexHolder','clean_data_cache')
        self.hooks={image.address(n):n for n in hooks if n in image.symbols}
        self.hooks[image.address('digest',unit='drivers/npu/npu_convolution.c')]='digest'
        self.power_step_address=image.address('a733_power_service_step');self.stop_address=image.address('a733_smp_stop')
        self.checkpoint_address=image.address('cancel_checkpoint')
        self.pmic_entries={image.address('a733_pmic_submit'),image.address('a733_pmic_poll')}
        self.u.hook_add(UC_HOOK_CODE,self.code);self.u.hook_add(UC_HOOK_MEM_READ,self.read,begin=0x02000000,end=0x08ffffff)
        self.u.hook_add(UC_HOOK_MEM_WRITE,self.write,begin=0x02000000,end=0x08ffffff)
        self.configure()

    def get(self,address,width=8):return int.from_bytes(self.u.mem_read(address,width),'little')
    def put(self,address,value,width=8):self.u.mem_write(address,int(value).to_bytes(width,'little'))
    def field(self,base,kind,name,value,width=8):self.put(base+self.L[kind][name],value,width)
    def ret(self,value=0):self.u.reg_write(UC_ARM64_REG_X0,value);self.u.reg_write(UC_ARM64_REG_PC,self.u.reg_read(UC_ARM64_REG_LR))
    def args(self,n=8):return [self.u.reg_read(UC_ARM64_REG_X0+i) for i in range(n)]
    def call(self,name,*args,unit=None):
        self.u.reg_write(UC_ARM64_REG_SP,0x43ffe000);self.u.reg_write(UC_ARM64_REG_LR,self.STOP)
        for i,value in enumerate(args):self.u.reg_write(UC_ARM64_REG_X0+i,value)
        self.u.emu_start(self.image.address(name,unit),self.STOP,count=30000000)
        assert self.u.reg_read(UC_ARM64_REG_PC)==self.STOP,(name,hex(self.u.reg_read(UC_ARM64_REG_PC)))
        return self.u.reg_read(UC_ARM64_REG_X0)

    def configure(self):
        for address,value in ((0x03400460,0xb000),(0x03400000,0x30),(0x03400004,7),(0x020022a0,0xf8104600),
            (0x02002b00,0x80000000),(0x02002b04,0xf0001),(0x02003920,0x10001),
            (0x07064000,8),(0x07064008,8),(0x03600020,0x9000),(0x03600024,0x9202),
            (0x03600030,0x1000003b),(0x03600004,0x7fffffff),(0x02050014,0xb1),(0x02050010,1),(0x02050018,2)):
            self.put(address,value,4)
        self.put(0x07090100,0x5c0d0007,4)
        ids=self.image.address('a733_observe_identity')
        for name,value in dict(epoch=7,generation=8,lease=9,armed_at=10000000,deadline=490000000).items():
            self.field(ids,'a733_observe_identity',name,value)
        start,end=self.image.address('__device_pool_start'),self.image.address('__device_pool_end')
        assert end-start==2097152
        assert self.call('device_memory_pool_init',self.pool,start,start,end-start)==1
        model=self.image.address('a733_npu_application_model');size=self.get(self.image.address('a733_npu_application_model_bytes'))
        assert self.call('a733_npu_reserved_prepare',self.reserved,31,model,size,0x03600000,
            self.image.address('fatal',unit='apps/npu-convolution/native.c'),0)==self.c
        R=self.L['a733_npu_reserved']
        for name,kind,width in (('lease','device_memory_lease',32),('extent','device_memory_extent',24)):
            self.u.mem_write(self.app+self.A[name],bytes(self.u.mem_read(self.reserved+R[name],width)))
        self.put(self.app+self.A['pool'],self.pool)
        self.u.mem_write(self.app+self.A['backing'],bytes(self.u.mem_read(self.pool+self.L['device_memory_pool']['backing'],24)))
        lease=self.reserved+R['lease'];slot=self.get(lease+self.L['device_memory_lease']['slot'],4)
        allocation=self.pool+self.L['device_memory_pool']['allocations']+slot*32
        self.u.mem_write(self.app+self.A['allocation'],bytes(self.u.mem_read(allocation,32)))
        self.put(self.app+self.A['used'],1,4)
        for name,handle in self.handles.items():
            if name in ('client','bootstrap','pmic'):self.put(self.app+self.A[name+'_task' if name!='client' else name],handle)
        S=self.L['a733_npu_startup'];cfg=self.startup+S['config'];providers=self.startup+S['providers']
        self.field(cfg,'a733_npu_startup_config','owner',31);self.field(cfg,'a733_npu_startup_config','deadline_us',490000000//24)
        for name,value in dict(counter=self.app+self.A['clock'],clocks=self.app+self.A['registry'],locks=self.app+self.A['locks'],
            pck=self.app+self.A['gpu'],rails=self.app+self.A['rails'],domains=self.app+self.A['domains']).items():
            self.field(providers,'a733_npu_startup_providers',name,value)
        for name,value,width in (('initialized',1,4),('stage',8,4),('task',self.handles['bootstrap'],8),
            ('pll_factors',0x00104600,4),('sram_routed',1,4),('window_open',1,4),('cookie',self.startup,8)):
            self.put(self.startup+S[name],value,width)
        for name in ('clock','rail','pck'):self.put(self.startup+S[name],1)
        ops=self.startup+S['ops']
        # Native startup callback layout: two register callbacks, then six operations.
        for i,name in enumerate(('native_read','native_write','native_now','native_spin_us','native_enter','native_leave','native_retain','native_wake')):
            self.put(ops+8*i,self.image.address(name,unit='drivers/npu/a733_npu_startup.c'))
        rails,pmic,domains=self.app+self.A['rails'],self.app+self.A['pmic'],self.app+self.A['domains']
        self.put(rails,15,4)
        for name,value,width in (('self',pmic,8),('rails',rails,8),('cookie',self.app+self.A['clock'],8),('active',8,4),('stage',4,4)):
            self.field(pmic,'a733_pmic',name,value,width)
        self.put(pmic+self.L['a733_pmic']['pmic_bus_ops'],self.image.address('a733_counter_clock_now'))
        row=self.startup+S['row']
        for name,value,width in (('domains',domains,8),('rail',self.startup+S['rail'],8),('owner',31,8),('domain',4,4),('counted',1,4),('request',3,4),('state',10,4)):
            self.field(row,'a733_power_domain_row',name,value,width)
        for name,value,width in (('self',domains,8),('pmic',pmic,8),('rows',row,8),('running',1,4),('supply_stage',6,4),('supply_verified',12,4)):
            self.field(domains,'a733_power_domains',name,value,width)
        power_config=domains+self.L['a733_power_domains']['config']
        for name,value,width in (('owner',41,8),('deadline',490000000//24,8),('tick_us',1000,4)):
            self.field(power_config,'a733_power_service_config',name,value,width)
        for index,domain in enumerate((1,4,5)):
            base=domains+self.L['a733_power_domains']['domain']+index*32
            self.field(base,'a733_power_domain','id',domain,4);self.field(base,'a733_power_domain','state',3 if domain==4 else 1,4)
            if domain==4:self.field(base,'a733_power_domain','count',1,4)
        assert self.call('npu_convolution_bind',self.c,self.startup,self.handles['client'],4,1)==1
        assert self.call('npu_application_irq_bind',self.service)==1
        assert self.call('npu_convolution_cancel_diagnostic_enable',self.c,self.image.address('npu_application_irq_restart'),
            self.image.address('cancel_checkpoint'),self.app)==1
        self.running=True
        for name,entry,arg in (('bootstrap',self.image.address('bootstrap',unit='apps/npu-convolution/native.c'),self.app),
            ('pmic',self.image.address('pmic_task',unit='apps/npu-convolution/native.c'),self.app),
            ('client',self.image.address('npu_startup_client'),0)):
            self.add_task(name,entry,arg)

    def add_task(self,name,entry,arg):
        previous=self.u.context_save()
        self.u.reg_write(UC_ARM64_REG_SP,0x43f00000-len(self.tasks)*0x10000);self.u.reg_write(UC_ARM64_REG_PC,entry)
        self.u.reg_write(UC_ARM64_REG_LR,self.STOP);self.u.reg_write(UC_ARM64_REG_X0,arg)
        self.contexts[name]=self.u.context_save();self.tasks[name]=dict(wake=0,notify=0,suspended=False,wait=None)
        self.u.context_restore(previous)
    def block(self,kind,wake):
        self.tasks[self.current]['wait']=kind;self.tasks[self.current]['wake']=wake;self.u.emu_stop()
    def hardware(self):
        if self.cancel_release is not None and self.time>=self.cancel_release:
            self.put(0x036003a8,self.get(0x036003a8,4)|16,4);self.put(0x03600004,0x7fffffff,4);self.cancel_release=None
        if self.pending_event is not None and self.time>=self.pending_event:
            self.put(0x03600010,1,4);self.put(0x03600004,0x7fffffff,4);self.put(0x0340020c,2,4)
            self.pending_event=None
        if self.get(0x0340020c,4)&self.get(0x0340010c,4)&2 and not self.irq_due:
            self.irq_due=True
    def read(self,cpu,access,address,size,value,unused):
        if 0x03600000<=address<0x03601000:assert not self.posted,'NPU read before completing posted writes'
        self.hardware()
        if address==0x03600664:self.put(address,0xe018a000 if self.failure=='fetch-outside' else 0xe010a008,4)
        if address==0x03600660:self.put(address,5,4)
        if address==0x03600010:self.events.append(('event-read',self.time,self.get(address,4)))
        if address==0x03600010:self.clear_event=True
    def write(self,cpu,access,address,size,value,unused):
        self.writes.append((self.time,address,size,value,self.current))
        if 0x03600000<=address<0x03601000:
            self.posted.append((address,value,self.get(address,4),self.time));return
        self.effect(address,value,self.get(address,4))
    def effect(self,address,value,previous):
        if address==0x0340018c:self.put(0x0340010c,self.get(0x0340010c,4)&~value,4)
        elif address==0x0340010c:self.put(address,self.get(address,4)|value,4)
        elif address==0x0340028c:self.put(0x0340020c,self.get(0x0340020c,4)&~value,4)
        elif address==0x036003a8:
            if value&1:self.put(0x03600388,0,4);self.put(0x03600004,0x7fffffff,4)
            if value&8:self.events.append(('assert',self.time,value))
            elif any(v[0]=='assert' for v in self.events) and previous&8:
                self.cancel_release=None if self.failure=='ack-timeout' else self.time+2400
        elif address==0x036003a4:
            device=self.get(0x03600654,4);self.launches.append(device);self.put(0x03600004,0,4)
            if device!=0xe010a000:
                self.pending_event=self.time+2400
                if device==0xe0004000:
                    output=self.get(self.c+self.L['npu_convolution']['buffers']+4*32)
                    reference=self.image.address('a733_npu_application_reference')
                    size=self.get(self.image.address('a733_npu_application_reference_bytes'))
                    raw=bytearray(self.u.mem_read(reference,size))
                    if self.wrong_output:raw[-1]^=1
                    self.u.mem_write(output,bytes(raw))

    def code(self,cpu,address,size,unused):
        if getattr(self,'clear_event',False):self.put(0x03600010,0,4);self.clear_event=False
        name=self.hooks.get(address);word=self.get(address,4);kind=word&0xffffffe0;reg=word&31
        if address==self.checkpoint_address and self.failure=='checkpoint-pll' and not self.checkpoint_injected:
            self.put(0x020022a0,0xf8104602,4);self.checkpoint_injected=True
        if address==self.power_step_address:
            assert self.current=='pmic';self.power_steps+=1;self.power_step_times.append(self.time)
        if address in self.pmic_entries:
            raise AssertionError('Idle admitted PMIC owner attempted a new request or bus operation')
        a=self.args() if name or word&0xffe0001f==0xd4000001 or address==self.stop_address else None
        if name:
            if name=='xTaskGetSchedulerState':self.ret(2 if self.running else 1)
            elif name=='digest':self.u.mem_write(a[2],hashlib.sha256(bytes(self.u.mem_read(a[0],a[1]))).digest());self.ret()
            elif name=='xTaskGetCurrentTaskHandle':self.ret(self.handles.get(self.current,0))
            elif name=='xTaskGetTickCount':self.ret(self.time//24000)
            elif name=='aarch64_device_translate':
                self.put(a[2],a[0]);self.put(a[2]+8,0 if a[0]<0x40000000 else 255,4);self.put(a[2]+12,3,4);self.ret(1)
            elif name=='aarch64_device_cache_stride':self.ret(64)
            elif name=='a733_memory_reservations_init':self.ret(self.pool)
            elif name=='a733_counter_clock_now':self.put(a[1],self.time//24);self.ret(1)
            elif name in ('a733_clock_token_status','a733_rail_status','xQueueGetMutexHolder'):self.ret(0)
            elif name=='a733_power_domain_check':
                if a[2]:self.put(a[2],8,4)
                self.ret(1)
            elif name in ('a733_platform_lock_enter','a733_power_service_ready','a733_power_service_idle'):self.ret(1)
            elif name in ('a733_platform_lock_leave','a733_platform_lock_retain','vPortEnterCritical','vPortExitCritical','vPortClearInterruptMask','vPortYieldFromISR','clean_data_cache'):self.ret()
            elif name in ('ulPortSetInterruptMask','uxPortSetInterruptMask'):self.ret(0xff)
            elif name=='a733_power_service_step':
                assert self.current=='pmic';self.ret(1000)
            elif name=='xQueueGenericCreateStatic':self.queues[a[3]]=dict(size=a[1],items=[]);self.ret(a[3])
            elif name.startswith('xTaskCreateStatic'):
                self.add_task('worker',a[0],a[3]);self.ret(self.handles['worker'])
            elif name=='xQueueGenericSend':
                q=self.queues[a[0]];q['items'].append(bytes(self.u.mem_read(a[1],q['size'])))
                for task in self.tasks.values():
                    if task['wait']==('queue',a[0]):task['wake']=0
                self.ret(1)
            elif name=='xQueueReceive':
                q=self.queues[a[0]]
                if q['items']:
                    self.u.mem_write(a[1],q['items'].pop(0));self.tasks[self.current]['wait']=None;self.ret(1)
                elif not a[2]:self.ret(0)
                else:self.block(('queue',a[0]),2**63)
            elif name=='uxQueueMessagesWaiting':self.ret(len(self.queues[a[0]]['items']))
            elif name in ('xTaskGenericNotify','vTaskGenericNotifyGiveFromISR'):
                target=next(n for n,h in self.handles.items() if h==a[0]);self.tasks[target]['notify']+=1;self.tasks[target]['wake']=0
                self.notifies.append((self.time,self.current,target))
                if name=='vTaskGenericNotifyGiveFromISR' and a[2]:self.put(a[2],1,4)
                self.ret(1)
                if name=='xTaskGenericNotify' and self.current=='client':self.u.emu_stop()
            elif name=='ulTaskGenericNotifyTake':
                task=self.tasks[self.current]
                if task['notify']:
                    count=task['notify'];task['notify']=0;task['wait']=None;self.ret(count)
                elif task['wait']=='notify' and self.time>=task['wake']:task['wait']=None;self.ret(0)
                elif not a[2]:self.ret(0)
                else:self.block('notify',2**63 if a[2]==0xffffffff else self.time+a[2]*24000)
            elif name=='vTaskDelay':self.ret();self.block('delay',self.time+a[0]*24000)
            elif name=='vTaskSuspend':
                target=self.current if not a[0] else next(n for n,h in self.handles.items() if h==a[0]);self.tasks[target]['suspended']=True
                if target==self.current:
                    if target=='client':self.retained=True
                    self.u.emu_stop()
                else:self.ret()
            return
        if address==self.STOP:
            self.u.emu_stop();return
        if address==self.stop_address:
            raise AssertionError(('native-stop',self.current,a[:2]))
        if word&0xffe0001f==0xd4000001:
            svc=word>>5&0xffff;assert svc in (0x5c0d,0x5c0e)
            if svc==0x5c0d:self.stages.append((self.time,a[0]&0xffff,a[1]&0xffff))
            self.svc.append((address+4,svc));self.u.reg_write(UC_ARM64_REG_PC,self.image.address('npu_finite_svc'));return
        if word==0xd69f03e0 and self.svc:
            self.u.reg_write(UC_ARM64_REG_PC,self.svc.pop()[0]);return
        if kind in (0xd5384240,0xd53800a0,0xd5384200,0xd53be000,0xd53be020,0xd53b0020,0xd5385200,0xd5381000,0xd53e5200,0xd53e4020,0xd53e6000):
            values={0xd5384240:self.level,0xd53800a0:0,0xd5384200:0,0xd53be000:24000000,
                0xd53b0020:0x04040004,0xd5385200:0x15<<26|(self.svc[-1][1] if self.svc else 0),
                0xd5381000:self.sctlr,0xd53e5200:0x17<<26,0xd53e4020:0,0xd53e6000:0}
            if kind==0xd53be020:self.time+=24;self.hardware();value=self.time
            else:value=values[kind]
            self.u.reg_write(UC_ARM64_REG_X0+reg,value);self.u.reg_write(UC_ARM64_REG_PC,address+4);return
        if kind in (0xd50b7a20,0xd50b7e20,0xd5087620):self.cache_lines+=1;self.u.reg_write(UC_ARM64_REG_PC,address+4);return
        if word==0xd5033f9f:
            self.dsb.append((self.time,address,self.current))
            pending=self.posted;self.posted=[]
            for at,value,previous,requested in pending:
                self.effect(at,value,previous);self.completed.append((requested,self.time,at,value,address))
        if kind==0xd5181000:self.sctlr=self.u.reg_read(UC_ARM64_REG_X0+reg);self.u.reg_write(UC_ARM64_REG_PC,address+4);return
        if kind in (0xd51fe220,0xd51be220,0xd51be320):self.u.reg_write(UC_ARM64_REG_PC,address+4);return
        if word==0xd4000003:
            self.level=12;self.current='return';self.u.reg_write(UC_ARM64_REG_X0+2,1)
            self.u.reg_write(UC_ARM64_REG_SP,0x43e00000);self.u.reg_write(UC_ARM64_REG_PC,self.image.address('sd_single_core_exception'));return
        if kind==0xd51ec040:
            assert self.u.reg_read(UC_ARM64_REG_X0+reg)==2;self.returned=True;self.u.emu_stop()

    def run(self):
        self.current='setup'
        # Monitor baseline is modeled independently of the native session.
        saved=self.image.address('saved');self.u.mem_write(saved,struct.pack('<8I',1,4,0x33,0x123,0x456,0,0,0))
        self.u.mem_write(saved+self.saved_irq,struct.pack('<Q5I',0x10000,2,0,8,0x5800,2))
        contract=self.image.address('admission')
        for name,value in dict(epoch=7,generation=8,lease=9,armed_at=10000000,diagnostic_deadline=490000000).items():
            self.field(contract,'a733_smp_contract',name,value)
        log=self.image.address('__npu_platform_log');self.put(log,0x534b4552);self.put(log+64,11000000)
        report=0x42000000;self.put(report,0x415337334e505231);self.put(report+8,7);self.put(report+16,8);self.put(report+48,11000000)
        priority={'irq':6,'bootstrap':4,'worker':4,'pmic':3,'client':2}
        for _ in range(100000):
            self.hardware()
            if self.irq_due and 'irq' not in self.contexts:
                self.put(0x0340030c,2,4);self.add_task('irq',self.image.address('npu_application_irq'),0)
            ready=[n for n,t in self.tasks.items() if not t['suspended'] and t['wake']<=self.time]
            if not ready:
                due=[t['wake'] for t in self.tasks.values() if not t['suspended']]
                if self.pending_event is not None:due.append(self.pending_event)
                if self.cancel_release is not None:due.append(self.cancel_release)
                assert due;self.time=min(due);continue
            self.current=max(ready,key=lambda n:priority[n]);self.u.context_restore(self.contexts[self.current])
            self.u.emu_start(self.u.reg_read(UC_ARM64_REG_PC),0,count=3000000)
            if self.returned or self.retained:return
            if self.current=='irq' and self.u.reg_read(UC_ARM64_REG_PC)==self.STOP:
                self.put(0x0340030c,0,4);self.put(0x0340020c,0,4);self.irq_due=False
                del self.contexts['irq'];del self.tasks['irq']
            else:self.contexts[self.current]=self.u.context_save()
        raise AssertionError('native tasks exceeded fixture budget')

    def snapshot(self,path):
        path.mkdir(parents=True,exist_ok=True)
        (path/'RAM.bin').write_bytes(bytes(self.u.mem_read(0x40000000,0x04000000)))
        registers=[UC_ARM64_REG_X0+i for i in range(29)]+[UC_ARM64_REG_FP,UC_ARM64_REG_LR,UC_ARM64_REG_SP,UC_ARM64_REG_PC,UC_ARM64_REG_PSTATE]+[UC_ARM64_REG_Q0+i for i in range(32)]
        current=self.u.context_save();contexts={}
        for name,context in {**self.contexts,self.current:current}.items():
            self.u.context_restore(context);contexts[name]={str(reg):self.u.reg_read(reg) for reg in registers}
        self.u.context_restore(current)
        metadata=dict(current=self.current,time=self.time,tasks=self.tasks,contexts=contexts,events=self.events,
            writes=self.writes,stages=self.stages,launches=self.launches,pc=hex(self.u.reg_read(UC_ARM64_REG_PC)),
            queues={str(k):dict(size=q['size'],items=[v.hex() for v in q['items']]) for k,q in self.queues.items()})
        for address,size in ((0x02002000,8192),(0x03000000,4096),(0x03400000,0x70000),(0x03600000,4096),(0x07064000,4096),(0x07090000,4096)):
            (path/(hex(address)+'.bin')).write_bytes(bytes(self.u.mem_read(address,size)))
        (path/'state.json').write_text(json.dumps(metadata,indent=2)+'\n')

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--native',type=Path,required=True);parser.add_argument('--evidence',type=Path,required=True)
    parser.add_argument('--case',action='append',choices=('nominal','wrong-output','fetch-outside','ack-timeout','checkpoint-pll','omit-dsb'))
    args=parser.parse_args();args.evidence.mkdir(parents=True,exist_ok=False)
    image=harness.Image(args.native/'image.elf',layout_names=());image.path=args.native/'image.elf'
    chosen={}
    with image.path.open('rb') as stream:
        for cu in ELFFile(stream).get_dwarf_info().iter_CUs():
            unit=cu.get_top_DIE().attributes['DW_AT_name'].value.decode()
            priority=4 if unit.endswith('/apps/npu-convolution/native.c') else 3 if unit.endswith('/drivers/npu/npu_convolution.c') else 2 if unit.endswith('/drivers/npu/npu_service.c') else 1
            for die in cu.iter_DIEs():
                if die.tag!='DW_TAG_structure_type' or 'DW_AT_name' not in die.attributes:continue
                name=die.attributes['DW_AT_name'].value.decode()
                if priority<chosen.get(name,0):continue
                fields={d.attributes['DW_AT_name'].value.decode():d.attributes['DW_AT_data_member_location'].value for d in die.iter_children()
                    if d.tag=='DW_TAG_member' and 'DW_AT_name' in d.attributes and 'DW_AT_data_member_location' in d.attributes}
                if fields:image.layouts[name]=fields;chosen[name]=priority
    host=module('active_cancel_producer_host',ROOT/'apps/npu-finite/host.py');geometry=host.image_profile(image.path.read_bytes(),{})[4]
    results=[]
    for name in args.case or ('nominal','wrong-output','fetch-outside','ack-timeout','checkpoint-pll','omit-dsb'):
        wrong=name=='wrong-output';negative=name!='nominal'
        model=Model(image,wrong,name if name not in ('nominal','wrong-output') else None)
        if name=='omit-dsb':
            entry=image.address('write32',unit='platform/a733/npu_native.c')
            with image.path.open('rb') as stream:
                sizes={int(s['st_size']) for s in ELFFile(stream).get_section_by_name('.symtab').iter_symbols()
                    if s['st_info']['type']=='STT_FUNC' and int(s['st_value'])==entry}
            assert len(sizes)==1
            sites=[address for address in range(entry,entry+next(iter(sizes)),4) if model.get(address,4)==0xd5033f9f]
            assert len(sites)==1;model.put(sites[0],0xd503201f,4)
            harness.refused(model.run,'NPU read before completing posted writes',AssertionError)
            model.snapshot(args.evidence/'omitted-dsb-stopped')
            results.append(dict(name=name,passed=True,mutated_instruction=hex(sites[0]),failure='NPU read before completing posted writes'))
            (args.evidence/'progress.json').write_text(json.dumps(results,indent=2)+'\n');continue
        try:model.run()
        except BaseException:
            model.snapshot(args.evidence/(name+'-stopped'));print('Stopped',name,model.current,hex(model.u.reg_read(UC_ARM64_REG_PC)),flush=True);raise
        results.append(dict(name=name,normal_return=model.returned,retained=model.retained,launches=model.launches,
            stages=model.stages,MMIO_writes=model.writes))
        (args.evidence/'progress.json').write_text(json.dumps(results,indent=2)+'\n')
        assert model.returned!=negative and model.retained==negative,(name,results[-1])
        assert model.launches==([0xe0089000,0xe010a000] if name in ('fetch-outside','ack-timeout','checkpoint-pll') else [0xe0089000,0xe010a000,0xe0089000,0xe0004000])
        assert model.power_steps>=1 and 'a733_pmic_submit_write' not in image.symbols
        stages={stage:time for time,stage,value in model.stages}
        begin=stages[0x0660];end=stages.get(0x066a,model.time)
        forbidden=[row for row in model.writes if begin<=row[0]<=end and
            (0x02002000<=row[1]<0x02004000 or 0x07060000<=row[1]<0x07068000 or 0x07083000<=row[1]<0x07084000)]
        assert not forbidden,forbidden
        assert not any(begin<=time<=end for time in model.power_step_times)
        assert not any(begin<=time<=end and target=='pmic' for time,caller,target in model.notifies)
        assert not model.posted
        results[-1].update(write_interval_ticks=[begin,end],forbidden_writes=forbidden,
            PMIC_owner_steps=model.power_step_times,PMIC_notifications=model.notifies,completed_NPU_writes=model.completed)
        c=model.c;service=model.service
        state=model.get(service+model.L['npu_service']['controller']+model.L['a733_npu_controller']['state'],4)
        if wrong:
            fault=[model.get(0x07090000+offset,4) for offset in (0x110,0x114,0x118,0x11c)]
            assert fault[0]>>24==0xfa and fault[1]&0x02000000 and not fault[1]&0x04000000,fault
            size=model.get(image.address('a733_npu_application_reference_bytes'))
            actual=model.get(image.address('a733_npu_application_reference')+size-1,1)^1
            assert fault[3]==((size-1)<<8)|actual,fault
            expected_site=next(i+1 for i,line in enumerate((ROOT/'apps/npu-finite/startup.c').read_text().splitlines(),1)
                if 'for(size_t i=0;i<output_bytes;++i)if(((unsigned char *)output)[i]' in line)
            assert fault[1]&0xfff==expected_site,(fault,expected_site)
            assert model.get(service+model.L['npu_service']['owner'],4)==0 and state==6
            assert model.get(c+model.L['npu_convolution']['job']) and not model.get(c+model.L['npu_convolution']['closed'],4)
            results[-1]['first_failure']=fault
        elif negative:
            fault=[model.get(0x07090000+offset,4) for offset in (0x110,0x114,0x118,0x11c)]
            assert fault[0]>>24==0xfa and fault[1]&0x04000000,fault
            assert model.get(service+model.L['npu_service']['owner'],4)==0 and state==10
            memory=model.c+model.L['npu_convolution']['native']+model.L['a733_npu_native']['memory']
            expected_memory=1 if name=='checkpoint-pll' else 2
            assert model.get(memory+model.L['a733_npu_memory']['state'],4)==expected_memory
            source='drivers/npu/npu_service.c' if name=='fetch-outside' else 'drivers/npu/a733_npu_controller.c' if name=='ack-timeout' else 'apps/npu-convolution/native.c'
            pattern='sample->fetch[j],i)' if name=='fetch-outside' else 'DIAGNOSTIC_TIMEOUT(c,elapsed)' if name=='ack-timeout' else 'CANCEL_CHECKPOINT_REFUSE(v[2])'
            expected_sites=[i for i,line in enumerate((ROOT/source).read_text().splitlines(),1) if pattern in line]
            assert fault[1]&0xfff in expected_sites,(name,fault,expected_sites)
            if name=='fetch-outside':assert fault[3]==0xe018a000 and fault[1]>>23&3==0
            elif name=='ack-timeout':assert 20000<=fault[3]<25000
            else:assert fault[3]==0xf8104602
            results[-1]['first_failure']=fault
        else:
            assert [stage for time,stage,value in model.stages if 0x0660<=stage<=0x066b]==list(range(0x0660,0x066c))
            monitor=bytes(model.u.mem_read(0x42000000,128));platform=bytes(model.u.mem_read(image.address('__npu_platform_log'),16384))
            receipt=bytes(model.u.mem_read(image.address('npu_finite_receipt'),88))
            parsed=host.validate_reports(monitor,platform,receipt,dict(execution_profile=host.ACTIVE_CANCEL,geometry=geometry,epoch=7,generation=8,lease=9))
            assert parsed['functional_success'] and parsed['active_cancel']
            for label,data in (('monitor',monitor),('platform',platform),('receipt',receipt)):(args.evidence/(label+'.bin')).write_bytes(data)
            results[-1]['validated_report']=parsed
            # Refuse a real receipt whose activity samples lie after assertion.
            text=parsed['log'];lines=text.split('NPU_CANCEL_RECEIPT=\n')[1].splitlines();bad=list(lines)
            trigger=int(bad[1],16);bad[2]=f'0x{trigger+1:016x}'
            malformed=text.replace('\n'.join(lines),'\n'.join(bad))
            harness.refused(lambda:host.active_cancel_report(malformed,0,490000000,geometry),'Active observations follow cancellation assertion')
            results[-1]['activity_after_assertion_refused']=True
        if negative:
            original=bytes(model.u.mem_read(0x07090110,16));model.svc=[(model.STOP,0x5c0e)];model.current='audit'
            model.call('npu_finite_svc',0xafff,0xdeadbeef,0x41000004)
            assert bytes(model.u.mem_read(0x07090110,16))==original
            results[-1]['first_failure_preserved']=True
        (args.evidence/'progress.json').write_text(json.dumps(results,indent=2)+'\n')
    (args.evidence/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,
        image_sha256=hashlib.sha256(image.path.read_bytes()).hexdigest(),cases=results,scope=__doc__),indent=2)+'\n')
    print('PASS actual cancellation producer, worker, IRQ, cleanup and wrong-output retention')

if __name__=='__main__':main()
