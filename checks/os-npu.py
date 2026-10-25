#!/usr/bin/env python3
"""Focused linked OS NPU constructor/API checks; RTOS and device effects modeled.

Runs the real catalogue sizing/preparation, fixed allocator, startup provider
binding and application APIs. Service completions below are explicit modeled
replies, not numerical execution or physical thermal/interrupt qualification.
"""
import argparse, hashlib, importlib.util, json, struct
from pathlib import Path
from harness import Image
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import *
ROOT=Path(__file__).resolve().parents[1]
NAMES=('os_npu_application','os_session','board_application','board_provider','a733_native_usb',
       'a733_counter_clock','a733_platform_locks','a733_pmic','a733_pmic_config','a733_power_domains',
       'a733_power_domain_row','a733_npu_startup','a733_npu_startup_providers','device_memory_pool',
       'device_memory_extent','device_memory_allocation','npu_runtime','npu_runtime_slot','npu_service_reply',
       'nbg_assembly_layout','npu_runtime_tensor')
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
prep=module('os_npu_preparer',ROOT/'apps/os/prepare.py')

class Machine:
    def __init__(self,image,session):
        self.image=image;self.L=image.layouts;self.u=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
        self.u.mem_map(0x02000000,0x07000000);self.u.mem_map(0x40000000,0x08000000);self.u.mem_map(0x50000000,0x100000)
        self.u.reg_write(UC_ARM64_REG_PSTATE,0x3c5)
        for address,data in image.parts:
            if data:self.u.mem_write(address,data)
        self.now=1000;self.scheduler=1;self.current=0;self.failed=False;self.done=False;self.suspended=False
        self.tasks=[];self.maps=[];self.ops=[];self.ticket=0;self.variant='';self.clock_reads=0;self.startups=0;self.stops=0
        self.session=0x50000000;self.u.mem_write(self.session,bytes(session))
        self.n=image.address('npu',unit='apps/os/npu.c');self.nL=self.L['os_npu_application']
        self.rt=self.n+self.nL['runtime'];self.slot=self.rt+self.L['npu_runtime']['slot']
        self.board=image.address('board');self.shared=image.address('providers')+self.L['board_provider']['shared']
        self.domains=image.address('providers')+self.L['board_provider']['domains'];nu=self.L['a733_native_usb']
        self.put(self.shared+nu['counter']+self.L['a733_counter_clock']['initialized'],1,4)
        self.put(self.shared+nu['locks']+self.L['a733_platform_locks']['initialized'],1,4)
        self.put(self.domains+self.L['a733_power_domains']['self'],self.domains)
        self.put(self.domains+self.L['a733_power_domains']['pmic'],self.shared+nu['pmic'])
        self.put(self.shared+nu['pmic']+self.L['a733_pmic']['config']+self.L['a733_pmic_config']['startup_deadline'],60000000)
        self.u.mem_write(self.board,bytes(session[:592]));self.put(self.board+160,60000000)
        self.put(0x03400460,0xd8<<8,4);self.put(0x03406308,0x100)
        names=('os_session_admitted','os_session','board_live','board_fail','vTaskEnterCritical','vTaskExitCritical',
          'aarch64_device_cache','aarch64_device_cache_stride','aarch64_device_translate','a733_counter_clock_now',
          'xTaskCreateStaticAffinitySet','xTaskGetSchedulerState','xTaskGetCurrentTaskHandle','xQueueGenericCreateStatic',
          'xQueueCreateMutexStatic','xTaskGenericNotify','ulTaskGenericNotifyTake','vTaskSuspend','vTaskDelay',
          'npu_service_send','npu_service_collect','npu_runtime_startup_poll','npu_runtime_stop_poll','npu_service_activate')
        self.hooks={image.address(n):n for n in names if n in image.symbols}
        special={}
        for address,data in image.parts:
            for off in range(0,len(data)-3,4):
                word=int.from_bytes(data[off:off+4],'little')
                if word&0xffffffe0 in (0xd5384240,0xd53b00e0,0xd53be020,0xd53be000,0xd53800a0):special[address+off]=word
        self.special=special
        for at in set(self.hooks)|set(special)|{0x500ff000}:self.u.hook_add(UC_HOOK_CODE,self.hook,begin=at,end=at)
    def put(self,a,v,n=8):self.u.mem_write(a,int(v).to_bytes(n,'little'))
    def get(self,a,n=8):return int.from_bytes(self.u.mem_read(a,n),'little')
    def ret(self,v=0):self.u.reg_write(UC_ARM64_REG_X0,v&((1<<64)-1));self.u.reg_write(UC_ARM64_REG_PC,self.u.reg_read(UC_ARM64_REG_LR))
    def gate(self):return self.get(self.n+self.nL['gate'])
    def stop(self):self.put(self.n+self.nL['gate'],((self.now+5000000)<<3)|(self.gate()&2)|4)
    def hook(self,c,at,size,unused):
        if at==0x500ff000:self.done=True;c.emu_stop();return
        if at not in self.hooks:
            w=self.special[at];v={0xd5384240:4,0xd53b00e0:4,0xd53be020:self.now*24,0xd53be000:24000000,0xd53800a0:0x100}[w&0xffffffe0]
            c.reg_write(UC_ARM64_REG_X0+(w&31),v);c.reg_write(UC_ARM64_REG_PC,at+4);return
        name=self.hooks[at];r=[c.reg_read(UC_ARM64_REG_X0+i) for i in range(8)]
        if name in ('os_session','os_session_admitted'):self.ret(self.session)
        elif name=='board_live':self.ret(not self.failed)
        elif name=='board_fail':self.failed=True;self.ret()
        elif name in ('vTaskEnterCritical','vTaskExitCritical','xTaskGenericNotify'):self.ret(1)
        elif name=='a733_counter_clock_now':
            self.clock_reads+=1
            if self.variant=='preempted-claim' and self.clock_reads==2:self.now+=2000000
            self.put(r[1],self.now);self.ret(1)
        elif name=='aarch64_device_cache':self.ret(1)
        elif name=='aarch64_device_cache_stride':self.ret(64)
        elif name=='aarch64_device_translate':
            self.maps.append(r[0]);attr,share=(0xff,3) if r[0]>=0x40000000 else (0,2)
            if self.variant=='bad-mapping' and r[0]==0x47800000:attr=0x44
            self.u.mem_write(r[2],struct.pack('<QII',r[0],attr,share));self.ret(1)
        elif name=='xTaskGetSchedulerState':self.ret(self.scheduler)
        elif name=='xTaskGetCurrentTaskHandle':self.ret(self.current)
        elif name=='xTaskCreateStaticAffinitySet':
            self.tasks.append((bytes(c.mem_read(r[1],20)).split(b'\0')[0].decode(),r[7],r[6]));self.ret(r[6])
        elif name=='xQueueGenericCreateStatic':self.ret(r[3])
        elif name=='xQueueCreateMutexStatic':self.ret(r[1])
        elif name=='npu_service_send':
            op,model,timeout=struct.unpack('<IIQ',c.mem_read(r[1],16));self.ticket+=1;self.operation=op
            self.ops.append((op,model,timeout));self.put(r[3],self.ticket)
            if self.variant=='stop-after-claim':self.stop()
            self.ret(1)
        elif name=='npu_service_collect':
            ticket=r[1]+(1 if self.variant=='wrong-ticket' else 0)
            state=9 if self.operation==5 else 6 if self.operation==2 else 2
            if self.variant=='wrong-state':state=10
            reply=bytearray(40);ll=self.L['npu_service_reply']
            for k,v,n in [('ticket',ticket,8),('state',state,4)]:struct.pack_into('<Q' if n==8 else '<I',reply,ll[k],v)
            self.u.mem_write(r[2],bytes(reply))
            if self.variant=='late-reply':self.now=60000000
            self.ret(1)
        elif name=='npu_runtime_startup_poll':self.startups+=1;self.ret(0)
        elif name=='npu_service_activate':self.ret(0)
        elif name=='npu_runtime_stop_poll':self.stops+=1;self.ret(0)
        elif name in ('ulTaskGenericNotifyTake','vTaskDelay'):
            self.now+=25000 if name=='ulTaskGenericNotifyTake' else 1000;self.ret()
        elif name=='vTaskSuspend':self.suspended=True;c.emu_stop()
        else:raise AssertionError(name)
    def call(self,name,*args,unit=None,budget=400000000):
        self.done=False;self.suspended=False
        self.u.reg_write(UC_ARM64_REG_SP,0x500fef00);self.u.reg_write(UC_ARM64_REG_LR,0x500ff000)
        for i,v in enumerate(args):self.u.reg_write(UC_ARM64_REG_X0+i,v)
        self.u.emu_start(self.image.address(name,unit=unit),0,count=budget)
        assert self.done or self.suspended,(name,hex(self.u.reg_read(UC_ARM64_REG_PC)))
        return self.u.reg_read(UC_ARM64_REG_X0)&0xffffffff
    def ready_client(self):
        self.scheduler=2;self.current=self.get(self.n+self.nL['client'])
        self.put(self.n+self.nL['active'],1,4)
        self.call('os_npu_thermal',1,0,self.now+1000000,self.now)
        assert self.call('os_npu_open')==1
        self.put(self.slot+self.L['npu_runtime_slot']['state'],2,4)


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--image',type=Path,required=True)
    p.add_argument('--session-base',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    image=Image(a.image,layout_names=NAMES);base=a.session_base.read_bytes();assert len(base)==808
    raw=bytearray(base+bytes(272));struct.pack_into('<Q',raw,608,len(raw))
    # NPU-capable synthetic fabric uses the pinned platform decoder's actual rates.
    struct.pack_into('<3I',raw,488,0x03000002,0x83000000,0x84000000)
    struct.pack_into('<3I',raw,500,200000000,600000000,400000000)
    # Model inputs from the actual earlier OS producer; rebinding is synthetic.
    models=json.loads((ROOT/'tools/npu-session-models.json').read_text())['models']
    catalogue=hashlib.sha256(b''.join(bytes.fromhex(x['model']['sha256']) for x in models)).digest()
    struct.pack_into('<6Q4I2Q',raw,808,prep.NPU_MAGIC,1,272,31,41,51,4,0,2,0,0x47400000,0x800000)
    raw[888:920]=catalogue;raw[920:1080]=bytes([1])*160
    (a.output/'MODEL-session.bin').write_bytes(raw)
    prep.session_check(raw,False)
    m=Machine(image,raw);assert m.call('os_session_valid',m.session)==1
    cases={'actual-session-validator':True}
    for name,off,value,fmt in [('owner-collision',832,struct.unpack_from('<Q',raw,456)[0],'<Q'),
       ('pck-collision',856,struct.unpack_from('<I',raw,540)[0],'<I'),('wrong-range',872,0x47000000,'<Q'),
       ('wrong-count',864,9,'<I')]:
        changed=bytearray(raw);struct.pack_into(fmt,changed,off,value);m.u.mem_write(m.session,bytes(changed))
        assert m.call('os_session_valid',m.session)==0
        try:prep.session_check(changed,False)
        except ValueError:pass
        else:raise AssertionError(name)
        cases[name]=True
    m.u.mem_write(m.session,bytes(raw))
    assert m.call('a733_clock_leases_init',m.shared+m.L['a733_native_usb']['clocks'],image.address('a733_counter_clock_now'),m.shared+m.L['a733_native_usb']['counter'])==1
    started=m.call('os_npu_start')
    (a.output/'constructor-state.json').write_text(json.dumps({'return':started,'state':{k:m.get(m.n+m.nL[k],4 if k in ('attempted','started') else 8) for k in ('attempted','started','end')},'tasks':m.tasks,'maps':len(m.maps)},indent=2)+'\n')
    assert started==1
    nL=m.nL;L=m.L;startup=m.n+nL['startup'];providers=startup+L['a733_npu_startup']['providers']
    expected={k:m.shared+L['a733_native_usb'][v] for k,v in [('counter','counter'),('clocks','clocks'),('locks','locks'),('pck','gpu'),('rails','rails')]}
    expected['domains']=m.domains
    assert {k:m.get(providers+L['a733_npu_startup_providers'][k]) for k in expected}==expected
    extent=m.n+nL['extent'];assert [m.get(extent+L['device_memory_extent'][k]) for k in ('cpu','physical','bytes')]==[0x47400000,0x47400000,0x800000]
    assert m.get(m.n+nL['pool']+L['device_memory_pool']['memory_class'],4)==1
    assert all(at in m.maps for at in range(0x47400000,0x47c00000,0x200000))
    assert {name for name,_,_ in m.tasks}=={'os-npu-boot','os-npu-app','npu-service'} and all(affinity==2 for _,affinity,_ in m.tasks)
    assert m.get(m.domains+L['a733_power_domains']['rows'])==startup+L['a733_npu_startup']['row']
    assert m.call('os_npu_start')==0
    cases['actual-catalogue-sizing-typed-pool-provider-row-tasks']=True
    snapshot=[(at,bytes(m.u.mem_read(at,n))) for at,n in [(0x41200000,0x1000000),(0x47400000,0x800000)]]
    def reset():
        for at,data in snapshot:m.u.mem_write(at,data)
        m.now=1000;m.failed=False;m.variant='';m.clock_reads=0;m.ops=[];m.startups=0;m.stops=0;m.scheduler=2;m.current=m.get(m.n+nL['client'])
    reset();m.ready_client();m.current=0x50012340;assert m.call('os_npu_submit',1000)==0 and len(m.ops)==1;cases['unregistered-caller']=True
    for variant in ('wrong-ticket','wrong-state','late-reply'):
        reset();m.ready_client();m.variant=variant;assert m.call('os_npu_submit',1000)==0xffffffff and m.failed;cases[variant]=True
    reset();m.ready_client();m.clock_reads=0;m.variant='preempted-claim';assert m.call('os_npu_submit',1000)==0 and len(m.ops)==1 and not m.gate()&2;cases['preempted-claim-expired']=True
    reset();m.ready_client();m.variant='stop-after-claim';assert m.call('os_npu_submit',1000)==1 and m.gate()&4 and not m.gate()&2;cases['stop-survives-accepted-completion']=True
    reset();m.ready_client();m.stop();end=m.gate()>>3;assert m.call('os_npu_submit',1000)==0
    assert m.call('os_npu_close')==1 and m.gate()>>3==end;cases['terminal-stop-bounded-close']=True
    reset();m.call('os_npu_thermal',0,1,0,m.now);m.current=m.get(m.n+nL['bootstrap']);m.call('bootstrap',unit='apps/os/npu.c');assert m.startups==0 and m.call('os_npu_stopped')==1;cases['stop-before-startup']=True
    reset();m.call('os_npu_thermal',1,0,m.now+1000000,m.now);m.current=m.get(m.n+nL['bootstrap']);m.call('bootstrap',unit='apps/os/npu.c');assert m.startups==1 and m.failed and m.now==10001000 and not m.stops;cases['frozen-monitor-held-window-retains-at-deadline']=True
    result={'passed':True,'physical_access':False,'image_sha256':hashlib.sha256(a.image.read_bytes()).hexdigest(),
      'source_sha256':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in [ROOT/'apps/os/npu.c',ROOT/'apps/os/npu.h',ROOT/'apps/os/admission.c',ROOT/'apps/os/prepare.py',Path(__file__)]},
      'cases':cases,'parent':{'cpu':0x47400000,'physical':0x47400000,'bytes':0x800000,'class':'DMA32'},
      'scope':'Actual native constructor, catalogue sizing/preparation, allocator, startup binding and caller/gate instructions. RTOS constructors, memory attributes/cache effects, service replies and startup/stop effects explicitly modeled. No physical inference, interrupt delivery, thermal qualification or launch authority.'}
    (a.output/'result.json').write_text(json.dumps(result,indent=2)+'\n');print('PASS OS NPU:',len(cases),'focused cases')
if __name__=='__main__':main()
