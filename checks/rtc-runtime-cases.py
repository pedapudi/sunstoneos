"""Actual native owner-task RTC updates with explicit RTOS/MMIO boundaries."""
import hashlib
import importlib.util
import json
import struct
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X3,UC_ARM64_REG_X4,UC_ARM64_REG_X5,UC_ARM64_REG_X6,UC_ARM64_REG_X7,UC_ARM64_REG_SP,UC_ARM64_REG_LR

def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m

def setup(image,out,source):
    prior=module('rtc_runtime_prior',source/'checks/rtc-application-cases.py')
    symbols,segments,layouts,identity=prior.setup(image,out)
    names=['rtc_task','rtc_copy_runtime','providers','board_application_start','xTaskCreateStaticAffinitySet',
           'a733_clock_acquire','a733_platform_lock_enter','a733_platform_lock_leave','vTaskDelay',
           'a733_counter_clock_now','aarch64_device_translate']
    with image.open('rb') as f:
        elf=ELFFile(f);sizes={}
        for name in names:
            values={int(s['st_value']) for s in elf.get_section_by_name('.symtab').iter_symbols() if s.name==name and s['st_value']}
            assert len(values)==1,(name,values);symbols[name]=next(iter(values))
        for cu in elf.get_dwarf_info().iter_CUs():
            for die in cu.iter_DIEs():
                name=die.attributes.get('DW_AT_name');size=die.attributes.get('DW_AT_byte_size')
                if die.tag=='DW_TAG_structure_type' and name and size:sizes[name.value.decode()]=int(size.value)
    assert sizes['a733_clock_token']==48 and sizes['rtc_runtime_output']==104
    for structure,fields in {'rtc_application':['clock','setting','runtime_output','runtime_digest','runtime_active','runtime_attempted','result','cancel'],
        'a733_clock_token':['id','owner','shared','exclusive'],'a733_native_usb':['counter','clocks'],
        'board_provider':['shared'],'a733_clock_leases':['failed'],'aarch64_translation':['physical','attributes','shareability']}.items():
        assert all(name in layouts[structure] for name in fields),structure
    (out/'runtime-preflight.json').write_text(json.dumps({'symbols':symbols,'layouts':layouts,'sizes':sizes,'build_id':identity.hex(),'instructions_executed':False},indent=2)+'\n')
    return prior,symbols,layouts

def run(out,source,step):
    image=out/'build/sunstone-a733-rtc.elf';prior,symbols,layouts=setup(image,out,source)
    class Machine(prior.Machine):
        def __init__(self,variant):
            super().__init__(image,out,source,startup=6,session_data=(out/'rollover-session-template.bin').read_bytes() if variant=='report-roll' else None)
            observation={'passed':True,'session_sha256':hashlib.sha256(self.session).hexdigest(),'variant':variant,'version':3,'build_id':self.identity.hex(),'authority':'Software model; nondeployable.'}
            (out/('validator-'+variant+'.json')).write_text(json.dumps(observation,indent=2)+'\n')
            (out/('admitted-'+variant+'.bin')).write_bytes(self.session)
            if variant=='applied':(out/'native-validator-preflight.json').write_text(json.dumps(observation,indent=2)+'\n')
            self.symbols.update(symbols);self.variant=variant;self.time=1000;self.owner=int.from_bytes(self.session[616:624],'little');self.held=0;self.yields=0;self.phase='boot';self.runtime_ready=True
            self.mmio={0:0x401d,4:1,0x10:0,0x14:0,0x20:0xabcd};self.io=[];self.busy8=0;self.busy7=0;self.loss=False;self.day_reads=0
            self.cpu.hook_add(UC_HOOK_MEM_READ,self.read_mmio);self.cpu.hook_add(UC_HOOK_MEM_WRITE,self.write_mmio)
            assert self.call('rtc_session_prepare',0x43200000)==1
            self.word(0x03400000,0x33);assert self.call('rtc_boot_route')==1
            assert not self.io,'Boot runtime profile wrote calendar/source'
            board=self.symbols['board'];b=self.layouts['board_application'];raw=bytes(self.session[:592])
            self.cpu.mem_write(board+b['admitted_session'],raw);self.cpu.mem_write(board+b['session'],raw)
            assert self.call('board_application_start')==1
            self.cpu.mem_write(board+self.layouts['a733_board_session']['deadline_us'],struct.pack('<Q',10000000))
            if variant=='early-cancel':self.word(self.symbols['rtc']+self.layouts['rtc_application']['cancel'],1)
            if variant=='alarm-pending':self.mmio[0x30]=1
            if variant=='gic-active':
                self.word(0x0340031c,16);self.cpu.mem_write(board+self.layouts['a733_board_session']['deadline_us'],struct.pack('<Q',self.time+20000))
            self.task_deadline=self.time+10000 if variant=='gic-active' else 9990000
            self.phase='task';self.done=False
        def provider(self):return self.symbols['providers']+self.layouts['board_provider']['shared']
        def runtime_base(self):return self.symbols['rtc']+self.layouts['rtc_application']['runtime_output']
        def hook(self,cpu,address,size,unused):
            if not getattr(self,'runtime_ready',False):return super().hook(cpu,address,size,unused)
            s=self.symbols;arg=lambda n:cpu.reg_read(UC_ARM64_REG_X0+n)
            if address==s['a733_smp_counter']:self.ret(self.time*24);return
            if address==s['xTaskCreateStaticAffinitySet']:
                assert arg(0)==s['rtc_task'] and not arg(3) and arg(7)==1;self.ret(0x1234);return
            if address==s['a733_clock_acquire']:
                assert arg(1)==self.owner and arg(2)==0x2000000004001 and not arg(3) and not arg(4) and not arg(5)
                assert arg(0)==self.provider()+self.layouts['a733_native_usb']['clocks']
                token=self.layouts['a733_clock_token']
                for name,value,length in [('id',1,8),('owner',self.owner,8),('shared',arg(2),16),('exclusive',0,16)]:cpu.mem_write(arg(7)+token[name],value.to_bytes(length,'little'))
                self.ret(0);return
            if address==s['a733_counter_clock_now']:
                assert arg(0)==self.provider()+self.layouts['a733_native_usb']['counter']
                self.time+=10;cpu.mem_write(arg(1),struct.pack('<Q',self.time));self.ret(1);return
            if address==s['aarch64_device_translate']:
                layout=self.layouts['aarch64_translation'];pointer=arg(2)
                for name,value,length in [('physical',arg(0),8),('attributes',0,4),('shareability',2,4)]:cpu.mem_write(pointer+layout[name],value.to_bytes(length,'little'))
                self.ret(1);return
            if address==s['a733_platform_lock_enter']:
                assert not self.held and arg(1)==0;self.held=1
                if self.variant=='mutex-cancel' and self.word(self.runtime_base()+40)==1:
                    self.word(s['rtc']+self.layouts['rtc_application']['cancel'],1)
                self.ret(1);return
            if address==s['a733_platform_lock_leave']:assert self.held;self.held=0;self.ret(0);return
            if address==s['vTaskDelay']:
                assert not self.held,'RTC yielded with common register mutex held'
                result=self.word(s['rtc']+self.layouts['rtc_application']['result'])
                if result:
                    self.done=True;cpu.emu_stop();return
                self.yields+=1;self.time+=1000
                if self.variant=='clock-loss' and self.io:
                    self.loss=True;self.word(self.provider()+self.layouts['a733_native_usb']['clocks']+self.layouts['a733_clock_leases']['failed'],1)
                if self.variant=='deadline' and self.io:self.time=10000000
                self.ret(0);return
            super().hook(cpu,address,size,unused)
        def read_mmio(self,cpu,access,address,size,value,unused):
            if not 0x07090000<=address<=0x07090050:return
            assert not self.loss and self.live,'RTC read after clock/session loss'
            if self.phase=='task':assert self.held and self.time<self.task_deadline,'RTC read outside mutex or after deadline'
            offset=address-0x07090000;v=self.mmio.get(offset,0)
            if offset==0:
                v&=~0x180
                if self.time<self.busy8:v|=256
                if self.time<self.busy7:v|=128
            if self.phase=='task' and offset==0x10:
                self.day_reads+=1
                if self.variant=='report-roll' and self.day_reads==5:self.mmio[0x10]+=1;self.mmio[0x14]=0
            cpu.mem_write(address,struct.pack('<I',v))
        def write_mmio(self,cpu,access,address,size,value,unused):
            if not 0x07090000<=address<=0x07090050:return
            assert self.phase=='task' and self.held and self.live and not self.loss and self.time<self.task_deadline
            offset=address-0x07090000;assert offset in (0x10,0x14),'Unexpected runtime clock/alarm write'
            self.io.append([offset,value]);self.mmio[offset]=value
            if offset==0x14:
                self.busy8=self.time+600000
                if self.variant=='late-cancel':self.word(self.symbols['rtc']+self.layouts['rtc_application']['cancel'],1)
                if self.variant=='post-store-revocation':self.live=0
            else:self.busy7=self.time+(2000000 if self.variant=='partial-day' else 600000)
        def task(self):
            self.cpu.reg_write(UC_ARM64_REG_X0,0);self.cpu.reg_write(UC_ARM64_REG_SP,0x50ffeff0);self.cpu.reg_write(UC_ARM64_REG_LR,0x50fff000)
            self.cpu.emu_start(self.symbols['rtc_task'],0,count=4000000);assert self.done,(self.variant,'task instruction bound')
    for variant in ('applied','report-roll','early-cancel','mutex-cancel','late-cancel','partial-day','alarm-pending','post-store-revocation','clock-loss','deadline','gic-active'):
        label='runtime-task-'+variant
        if step(label):continue
        m=Machine(variant)
        assert m.call('rtc_copy_runtime',0x50000000,0x50000100)==0
        assert bytes(m.cpu.mem_read(0x50000000,104))==bytes(104)
        m.task();result=m.word(m.symbols['rtc']+m.layouts['rtc_application']['result'])
        report=bytes(m.cpu.mem_read(m.runtime_base(),104));outcome=struct.unpack_from('<I',report,48)[0]
        expected={'applied':1,'report-roll':1,'early-cancel':2,'mutex-cancel':2,'late-cancel':3,'partial-day':4,'alarm-pending':4}
        if variant in expected:
            assert result==1 and outcome==expected[variant],(variant,result,outcome)
            assert m.call('rtc_copy_runtime',0x50000000,0x50000100)==1
            assert bytes(m.cpu.mem_read(0x50000000,104))==report
            (out/(variant+'-runtime-copy.bin')).write_bytes(report)
            if variant=='report-roll':assert struct.unpack_from('<2I',report,24)==(20744,0x173b3b) and m.mmio[0x10]==20745
        else:
            assert result==2 and m.call('rtc_copy_runtime',0x50000000,0x50000100)==2
            assert bytes(m.cpu.mem_read(0x50000000,104))==bytes(104)
        if variant in ('early-cancel','mutex-cancel','alarm-pending','gic-active'):assert not m.io
        if variant in ('post-store-revocation','clock-loss','deadline'):assert len(m.io)==1
        assert m.mmio[0x20]==0xabcd and not m.held
        step(label,{'result':result,'outcome':outcome,'writes':m.io,'task_yields_without_mutex':m.yields,'no_alarm_arm':True})

    label='runtime-profile-binding'
    if not step(label):
        for version,flags in ((2,4),(3,2)):
            m=prior.Machine(image,out,source,startup=6)
            m.cpu.mem_write(0x41f00000+600,struct.pack('<Q',version));m.word(0x41f00000+768,flags)
            assert not m.call('a733_rtc_session_valid',0x41f00000)
        step(label,{'v2_runtime_flag_refused':True,'v3_cold_calendar_flag_refused':True})
    label='runtime-launch-inputs'
    if not step(label):
        prepare=module('rtc_runtime_prepare',source/'apps/rtc/prepare.py');parts,address,identity=prepare.base.image_parts(image)
        raw=bytearray((out/'session-template.bin').read_bytes());raw[48:80]=identity
        epoch,generation,lease=struct.unpack_from('<3Q',raw,24)
        request,contract=prepare.request_bytes(address,identity,raw,epoch,generation,lease)
        launch=out/'launcher'
        for name,data in [('request.bin',request),('contract.bin',contract),('session.bin',raw),('identity.bin',identity)]:(launch/name).write_bytes(data)
        (launch/'prepared.json').write_text(json.dumps({'segments':[{'name':'session.bin','address':0x41f00000},{'name':'identity.bin','address':address}]}))
        step(label,{'session_bytes':784,'version':3,'producer':'Actual calendar/session/request producers with final linked identity'})
