#!/usr/bin/env python3
"""Focused linked OS NPU constructor/API checks; RTOS and device effects modeled.

Runs the real catalogue sizing/preparation, fixed allocator, startup provider
binding and application APIs. Task frames come from the linked port's stack
initializer. The registered client's output fill, comparison and one report
publication run on modeled API replies, and USB0 operation 6 runs the linked
exchange over modeled TinyUSB calls. Service completions below are explicit
modeled replies, not numerical execution or physical thermal/interrupt
qualification. --default-image runs operations 5 and 6 on an image without
the NPU profile.
"""
import argparse, hashlib, importlib.util, json, struct
from pathlib import Path
from harness import Image
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
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
report=module('os_npu_report',ROOT/'apps/os/npu-report.py')
USB='apps/mimus-board/usb0.c'
TRAMPOLINE=0x500ff100
API=('os_npu_open','os_npu_load','os_npu_tensor','os_npu_submit','os_npu_close','os_npu_stopping','os_npu_stopped')

class Machine:
    def __init__(self,image,session,npu=True):
        self.image=image;self.L=image.layouts;self.u=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
        self.u.mem_map(0x02000000,0x07000000);self.u.mem_map(0x40000000,0x08000000);self.u.mem_map(0x50000000,0x100000)
        self.u.reg_write(UC_ARM64_REG_PSTATE,0x3c5)
        for address,data in image.parts:
            if data:self.u.mem_write(address,data)
        self.now=1000;self.scheduler=1;self.current=0;self.failed=False;self.done=False;self.suspended=False
        self.tasks=[];self.maps=[];self.ops=[];self.ticket=0;self.variant='';self.clock_reads=0;self.startups=0;self.stops=0
        self.session=0x50000000;self.u.mem_write(self.session,bytes(session))
        self.frames={};self.pending=None;self.api=None;self.api_calls=[];self.usb=None;self.usb_stops=0
        if npu:
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
          'npu_service_send','npu_service_collect','npu_runtime_startup_poll','npu_runtime_stop_poll','npu_service_activate',
          'tud_vendor_n_available','tud_vendor_n_read','tud_vendor_n_write','tud_vendor_n_write_flush','a733_typec_revoke')+API
        self.hooks={image.address(n):n for n in names if n in image.symbols}
        self.hooks[image.address('stop',unit=USB)]='usb-stop';self.hooks[TRAMPOLINE]='trampoline'
        self.port=image.address('pxPortInitialiseStack')
        special={}
        for address,data in image.parts:
            for off in range(0,len(data)-3,4):
                word=int.from_bytes(data[off:off+4],'little')
                if word&0xffffffe0 in (0xd5384240,0xd53b00e0,0xd53be020,0xd53be000,0xd53800a0,0xd53bd060):special[address+off]=word
        # TPIDRRO_EL0 1: the port's core-0 identity word, read while it builds a frame.
        self.special=special
        for at in set(self.hooks)|set(special)|{0x500ff000}:self.u.hook_add(UC_HOOK_CODE,self.hook,begin=at,end=at)
    def put(self,a,v,n=8):self.u.mem_write(a,int(v).to_bytes(n,'little'))
    def get(self,a,n=8):return int.from_bytes(self.u.mem_read(a,n),'little')
    def ret(self,v=0):self.u.reg_write(UC_ARM64_REG_X0,v&((1<<64)-1));self.u.reg_write(UC_ARM64_REG_PC,self.u.reg_read(UC_ARM64_REG_LR))
    def gate(self):return self.get(self.n+self.nL['gate'])
    def stop(self):self.put(self.n+self.nL['gate'],((self.now+5000000)<<3)|(self.gate()&2)|4)
    def hook(self,c,at,size,unused):
        if at==0x500ff000:self.done=True;c.emu_stop();return
        if at in self.hooks and self.hooks[at] in API and self.api is None:return
        if at not in self.hooks:
            w=self.special[at];v={0xd5384240:4,0xd53b00e0:4,0xd53be020:self.now*24,0xd53be000:24000000,0xd53800a0:0x100,0xd53bd060:1}[w&0xffffffe0]
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
            task=bytes(c.mem_read(r[1],20)).split(b'\0')[0].decode();self.tasks.append((task,r[7],r[6]))
            # The linked port builds this task's first frame, in this creation context.
            top=(r[5]+(r[2]-1)*8)&~15;self.pending=(task,r[0],r[6],c.reg_read(UC_ARM64_REG_LR),top)
            for i,v in enumerate((top,r[0],r[3])):c.reg_write(UC_ARM64_REG_X0+i,v)
            c.reg_write(UC_ARM64_REG_LR,TRAMPOLINE);c.reg_write(UC_ARM64_REG_PC,self.port)
        elif name=='trampoline':
            task,entry,handle,lr,top=self.pending;self.pending=None
            self.frames[task]=self.spsr(c.reg_read(UC_ARM64_REG_X0),top,entry)
            c.reg_write(UC_ARM64_REG_X0,handle);c.reg_write(UC_ARM64_REG_PC,lr)
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
        elif name in API:self.model_api(c,name,r)
        elif name=='tud_vendor_n_available':self.ret(len(self.usb['rx']))
        elif name=='tud_vendor_n_read':
            n=min(r[2],len(self.usb['rx']));c.mem_write(r[1],self.usb['rx'][:n]);self.usb['rx']=self.usb['rx'][n:];self.ret(n)
        elif name=='tud_vendor_n_write':self.usb['tx']+=bytes(c.mem_read(r[1],r[2]));self.ret(r[2])
        elif name=='tud_vendor_n_write_flush':self.ret(0)
        elif name=='a733_typec_revoke':self.ret()
        elif name=='usb-stop':self.usb_stops+=1;self.suspended=True;c.emu_stop()
        else:raise AssertionError(name)
    def spsr(self,frame,top,entry):
        """SPSR of an initial frame: the word above the exception return address."""
        found=[at for at in range(frame,top,8) if self.get(at)==entry and self.get(at+8) in (0,4)]
        assert len(found)==1,(hex(frame),found);return self.get(found[0]+8)
    def frame(self,entry,context):
        top=0x500f8000;self.u.mem_write(top-0x1000,bytes(0x1000))
        self.done=False;self.u.reg_write(UC_ARM64_REG_SP,0x500fef00);self.u.reg_write(UC_ARM64_REG_LR,0x500ff000)
        for i,v in enumerate((top,entry,context)):self.u.reg_write(UC_ARM64_REG_X0+i,v)
        self.u.emu_start(self.port,0,count=1000000);assert self.done
        return self.spsr(self.u.reg_read(UC_ARM64_REG_X0),top,entry)
    def model_api(self,c,name,r):
        """Modeled API replies for the client; tensors live in the private pool."""
        a=self.api;self.api_calls.append(name)
        if name=='os_npu_open':self.ret(1)
        elif name=='os_npu_load':self.ret(1)
        elif name in ('os_npu_stopping',):self.ret(0)
        elif name=='os_npu_stopped':self.ret(a['closed'])
        elif name=='os_npu_close':a['closed']=1;self.ret(1)
        elif name=='os_npu_tensor':
            data,size=a['tensors'][(r[0],r[1])];c.mem_write(r[2],struct.pack('<QQ',data,size));self.ret(1)
        elif name=='os_npu_submit':
            job=a['submits'];a['submits']+=1;outcome=a['jobs'][job]
            if outcome in ('match','mismatch'):
                for (section,index),(data,size) in a['tensors'].items():
                    if section==1:
                        value=bytearray(a['references'][index])
                        if outcome=='mismatch':value[0]^=1
                        c.mem_write(data,bytes(value))
            self.ret({'retained':-1,'refused':0}.get(outcome,1))
    def exchange(self,packet,sends=1):
        """The linked USB0 exchange: one request, then the passes that send its frame."""
        self.usb={'rx':bytes(packet),'tx':b''};stops=self.usb_stops
        self.call('exchange',unit=USB)
        for _ in range(sends):
            if self.usb_stops!=stops:break
            self.call('exchange',unit=USB)
        if self.usb_stops==stops and self.usb['tx']:self.call('tud_vendor_tx_cb',0,len(self.usb['tx']))
        return self.usb_stops!=stops,self.usb['tx']
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
    p.add_argument('--default-image',type=Path,help='an OS image without the NPU profile, for operations 5 and 6')
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
    # SPSR 4 is EL1t and 0 is EL0t (FreeRTOS portINITIAL_PSTATE_EL1/EL0).
    assert m.frames=={'os-npu-boot':4,'os-npu-app':4,'npu-service':4},m.frames
    cases['actual-task-frames-el1']=True
    client,boot=image.address('os_npu_client_task'),image.address('bootstrap',unit='apps/os/npu.c')
    creating=image.address('creating');assert m.get(creating,1)==0
    refused=[m.frame(client,0),m.frame(client,0x50012340),m.frame(boot,0),m.frame(image.address('os_npu_open'),0)]
    m.put(creating,1,1);refused+=[m.frame(client,0x50012340),m.frame(image.address('os_npu_open'),0)];m.put(creating,0,1)
    assert refused==[0]*6,refused;cases['unregistered-frames-el0']=True
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
    usb_cases(m,image,raw,cases)
    manifest_cases(raw,base,cases)
    if a.default_image:default_cases(a.default_image,base,cases)
    result={'passed':True,'physical_access':False,'image_sha256':hashlib.sha256(a.image.read_bytes()).hexdigest(),
      'source_sha256':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in [ROOT/'apps/os/npu.c',ROOT/'apps/os/npu.h',ROOT/'apps/os/npu-client.c',ROOT/'apps/os/admission.c',ROOT/'apps/os/prepare.py',ROOT/'apps/os/npu-report.py',ROOT/USB,Path(__file__)]},
      'cases':cases,'parent':{'cpu':0x47400000,'physical':0x47400000,'bytes':0x800000,'class':'DMA32'},
      'scope':'Actual native constructor, catalogue sizing/preparation, allocator, startup binding, caller/gate instructions, port task frames, client fill/compare/report publication and USB0 operations 5 and 6. RTOS constructors, memory attributes/cache effects, service and API replies, TinyUSB calls and startup/stop effects explicitly modeled. No physical inference, interrupt delivery, thermal qualification or launch authority.'}
    (a.output/'result.json').write_text(json.dumps(result,indent=2)+'\n');print('PASS OS NPU:',len(cases),'focused cases')
def packet(op,flags=0,size=0,offset=0,expected=0):return struct.pack('<BBHIQ',op,flags,size,offset,expected)
def vectors(m,image,name,count):
    base=image.address(name);return [bytes(m.u.mem_read(m.get(base+16*i),m.get(base+16*i+8))) for i in range(m.get(image.address(count),4))]
def kept(data):return sum(b==(report.PATTERN>>8*(i&3))&255 for i,b in enumerate(data))
def usb_cases(m,image,raw,cases):
    epoch,generation=struct.unpack_from('<2Q',raw,24);published=image.address('published',unit='apps/os/npu-client.c')
    frame_bytes=image.address('frame_bytes',unit=USB)
    def fetch():
        stopped,tx=m.exchange(packet(6));assert not stopped and len(tx)==report.REPORT_BYTES and m.get(frame_bytes,4)==0
        return tx
    m.failed=False
    first=fetch();r=report.decode(first,epoch,generation)
    assert report.classify(r)=='pending' and first==fetch();cases['usb-op6-pending-104-bytes']=True
    for bad in (packet(6,size=1),packet(6,flags=1),packet(6,offset=4),packet(6,expected=1),packet(7)):
        m.failed=False;stopped,tx=m.exchange(bad);assert stopped and not tx,bad
    cases['usb-op6-malformed-and-op7-stop']=True
    inputs,references=vectors(m,image,'os_npu_inputs','os_npu_input_count'),vectors(m,image,'os_npu_references','os_npu_output_count')
    assert m.get(image.address('os_npu_client_model'),4)==0 and [len(x) for x in inputs+references]==[192,72]
    expected_digest=hashlib.sha256(b''.join(references)).hexdigest()
    for name,jobs,outcome in [('match',['match','match'],'matched'),('mismatch',['match','mismatch'],'mismatch'),
            ('no-write',['match','no-write'],'mismatch'),('refused',['match','refused'],'refused'),
            ('retained',['match','retained'],'retained')]:
        m.u.mem_write(published,bytes(report.REPORT_BYTES));m.failed=False;m.current=m.get(m.n+m.L['os_npu_application']['client'])
        tensors={(0,i):(0x47400000+0x1000*i,len(x)) for i,x in enumerate(inputs)}
        tensors.update({(1,i):(0x47600000+0x1000*i,len(x)) for i,x in enumerate(references)})
        for data,size in tensors.values():m.u.mem_write(data,bytes(size))
        m.api={'closed':0,'submits':0,'jobs':jobs,'tensors':tensors,'references':references};m.api_calls=[]
        writes=[];h=m.u.hook_add(UC_HOOK_MEM_WRITE,lambda u,access,at,size,value,x:writes.append((at-published,size)),
                                 begin=published,end=published+report.REPORT_BYTES-1)
        m.call('os_npu_client_task',0,unit='apps/os/npu-client.c');m.u.hook_del(h);m.api=None
        assert m.suspended and m.api_calls.count('os_npu_submit')==len(jobs)
        assert [o for o,n in writes if o<=12<o+n]==[12] and writes[-1]==(12,4),writes
        raw_report=bytes(m.u.mem_read(published,report.REPORT_BYTES));r=report.decode(raw_report,epoch,generation)
        assert report.classify(r)==outcome,(name,r)
        assert bytes(m.u.mem_read(0x47400000,192))==inputs[0]
        completed=len(jobs)-(name in ('refused','retained'))
        assert (r['jobs'],r['jobs_selected'],r['output_bytes'])==(completed,2,72)
        assert r['mismatches']==(name in ('mismatch','no-write'))
        assert r['retained']==(name=='retained')==m.failed and r['stopped']==(name!='retained')
        last=bytes(m.u.mem_read(0x47600000,72))
        if name=='no-write':
            # The first job left the correct output; only the fill exposes the second.
            assert r['pattern_bytes']==72 and last==bytes((report.PATTERN>>8*(i&3))&255 for i in range(72))
        elif name=='mismatch':assert r['pattern_bytes']==kept(last) and last[1:]==references[0][1:] and last!=references[0]
        else:assert r['pattern_bytes']==kept(references[0]),(name,r['pattern_bytes'],kept(references[0]))
        if name in ('match','refused','retained'):assert r['last_digest']==expected_digest
        if name=='retained':
            stopped,tx=m.exchange(packet(6));assert stopped,'a failed lifetime sent a frame'
        else:assert fetch()==raw_report and fetch()==raw_report
        cases['client-report-'+name]=True
def manifest_cases(raw,base,cases):
    """Link-check records from before the NPU profile carry no 'npu' key and mean OFF."""
    def refused(action):
        try:action()
        except ValueError:return True
        return False
    profile={'range':prep.NPU_RANGE,'catalogue':bytes(raw[888:920]).hex(),'client_model':struct.unpack_from('<I',raw,860)[0]}
    prep.manifest_cpu_clock({'cpu_clock':False},base)
    prep.manifest_cpu_clock({'cpu_clock':False,'npu':True,'npu_profile':profile},raw)
    assert all(refused(lambda m=m,r=r:prep.manifest_cpu_clock(m,r)) for m,r in [
        ({'cpu_clock':False},raw),({'cpu_clock':False,'npu':False},raw),({'cpu_clock':False,'npu':1,'npu_profile':profile},raw),
        ({'cpu_clock':False,'npu':True,'npu_profile':profile},base),
        ({'cpu_clock':False,'npu':True,'npu_profile':dict(profile,client_model=1)},raw)])
    image=b'MODEL-NOT-AN-IMAGE';old={'passed':True,'target':'sunstone-a733-os','lifetime':'supervised','audio':False,
        'image_sha256':hashlib.sha256(image).hexdigest(),'applications':[{'name':'mimus'},{'name':'thermal'}],'interrupts':[{'intid':71}]}
    assert prep.image_manifest_check(old,image) is False
    npu=dict(old,npu=True,applications=old['applications']+[{'name':'npu'}],interrupts=[{'intid':71},{'intid':97}])
    assert prep.image_manifest_check(npu,image) is False
    assert refused(lambda:prep.image_manifest_check(dict(old,npu=1),image))
    assert refused(lambda:prep.image_manifest_check(dict(old,npu=True),image))
    assert refused(lambda:prep.image_manifest_check(dict(npu,npu=False),image))
    cases['manifest-without-npu-key-means-off']=True
def default_cases(path,base,cases):
    image=Image(path,layout_names=NAMES);raw=bytearray(base)
    assert 'os_npu_report_copy' not in image.symbols
    m=Machine(image,raw,npu=False)
    stopped,tx=m.exchange(packet(6));assert stopped and not tx;cases['default-image-op6-stops']=True
    m.failed=False;stopped,tx=m.exchange(packet(5),sends=3);assert not stopped and len(tx)==1160;cases['default-image-op5-1160-bytes']=True
if __name__=='__main__':main()
