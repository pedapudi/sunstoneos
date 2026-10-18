"""Only new linked explicit RTC initialization, sealed setting and copied output."""
import datetime
import hashlib
import importlib.util
import json
import struct
from unicorn import UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0

def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m

def run(out,source,step):
    previous=module('rtc_calendar_previous',source/'checks/rtc-application-cases.py')
    image=out/'build/sunstone-a733-rtc.elf'
    class Machine(previous.Machine):
        def __init__(self,variant='normal'):
            super().__init__(image,out,source,startup=6)
            (out/'native-validator-preflight.json').write_text(json.dumps({'passed':True,'session_sha256':hashlib.sha256(self.session).hexdigest(),'linked_build_id':self.identity.hex(),'startup_seconds':6,'total_seconds':60,'authority':'Software model; nondeployable.'},indent=2)+'\n')
            self.variant=variant;self.mmio={0:0x800c,4:2,0x10:0,0x14:0};self.io=[];self.busy8=0;self.busy7=0
            self.cpu.hook_add(UC_HOOK_MEM_READ,self.mmio_read);self.cpu.hook_add(UC_HOOK_MEM_WRITE,self.mmio_write)
        def hook(self,cpu,address,size,unused):
            if address==self.symbols['a733_smp_counter']:
                self.counter+=2400;self.ret(self.counter);return
            super().hook(cpu,address,size,unused)
        def mmio_read(self,cpu,access,address,size,value,unused):
            if 0x07090000<=address<=0x07090050:
                offset=address-0x07090000;v=self.mmio.get(offset,0)
                if offset==0:
                    v&=~0x180
                    if self.counter//24<self.busy8:v|=256
                    if self.counter//24<self.busy7:v|=128
                cpu.mem_write(address,struct.pack('<I',v))
        def mmio_write(self,cpu,access,address,size,value,unused):
            if 0x07090000<=address<=0x07090050:
                offset=address-0x07090000;self.io.append([offset,value]);assert offset in (0,4,0x10,0x14)
                if offset==0:
                    assert value>>16==0x16aa;self.mmio[0]=value&65535
                    self.mmio[4]=(self.mmio[4]&6)|int((value&0x13)==0x11)
                    if self.variant=='source-failure':self.mmio[4]|=4
                elif offset==4:assert value==2;self.mmio[4]&=~2
                else:
                    self.mmio[offset]=value
                    if offset==0x14:self.busy8=self.counter//24+600000
                    else:self.busy7=self.counter//24+(10000000 if self.variant=='partial-day' else 600000)
    for variant in ('normal','setting-tamper','source-failure','partial-day','adoption-no-write'):
        label='new-native-initialize-'+variant
        if step(label):continue
        m=Machine(variant)
        if variant=='setting-tamper':m.word(0x41f00000+768,2)
        if variant=='adoption-no-write':
            raw=bytearray(m.session[:768]);struct.pack_into('<2Q',raw,600,1,768)
            m.cpu.mem_write(0x41f00000,bytes(raw));struct.pack_into('<Q',m.contract,184,768);m.contract[192:224]=hashlib.sha256(raw).digest();m.cpu.mem_write(0x43200000,bytes(m.contract))
        result=m.call('rtc_session_prepare',0x43200000)
        assert result==int(variant in ('normal','adoption-no-write')),(variant,result,m.io)
        if result:
            m.word(0x03400000,0x33);assert m.call('rtc_boot_route')==1
            pointer=m.call('rtc_admitted_session');assert pointer
            if variant=='normal':
                report=m.call('rtc_admitted_initialization');setting=m.call('rtc_admitted_setting')
                assert report and setting
                diagnostic=bytes(m.cpu.mem_read(report,48));assert struct.unpack_from('<I',diagnostic,36)[0]==6
                (out/'native-initialization.bin').write_bytes(diagnostic)
                requested=bytes(m.cpu.mem_read(setting,16));m.word(0x41f00000+772,1)
                assert bytes(m.cpu.mem_read(setting,16))==requested
                assert m.mmio[0x10]&65535==20744 and m.mmio[0x14]&0x1f3f3f==0x123438
                (out/'native-initialization-registers.json').write_text(json.dumps({'writes':m.io,'final':m.mmio,'counter_ticks':m.counter},indent=2)+'\n')
        if variant in ('setting-tamper','adoption-no-write'):assert not m.io
        if variant=='source-failure':assert all(o not in (0x10,0x14) for o,v in m.io)
        if variant=='partial-day':assert any(o==0x10 for o,v in m.io) and not m.call('rtc_admitted_session')
        step(label,{'result':result,'writes':m.io,'calendar_day':m.mmio[0x10],'counter_ticks':m.counter})

    label='new-native-initialized-copy'
    if not step(label):
        m=Machine();layout=m.layouts['rtc_application'];base=m.symbols['rtc']
        day=20744;date=datetime.date(1970,1,1)+datetime.timedelta(days=day-1)
        calendar=struct.pack('<22I',day,18,52,56,date.year,date.month,date.day,day,18,52,58,date.year,date.month,date.day,
                             day,0x12343a,0,0,0x401d,1,1,0)
        payload=calendar+(out/'native-initialization.bin').read_bytes();assert len(payload)==136
        m.cpu.mem_write(base,bytes(m.session[:768]));m.cpu.mem_write(base+layout['initialized_output'],payload)
        m.cpu.mem_write(base+layout['initialized_digest'],hashlib.sha256(payload).digest())
        m.cpu.mem_write(m.symbols['board']+m.layouts['a733_board_session']['deadline_us'],struct.pack('<Q',1000000))
        for result in (0,2):
            m.word(base+layout['result'],result);assert m.call('rtc_copy_initialized',0x50000000,0x50000100)==result
            assert bytes(m.cpu.mem_read(0x50000000,136))==bytes(136) and bytes(m.cpu.mem_read(0x50000100,32))==bytes(32)
        m.word(base+layout['result'],1);assert m.call('rtc_copy_initialized',0x50000000,0x50000100)==1
        assert bytes(m.cpu.mem_read(0x50000000,136))==payload
        (out/'initialized-copy.bin').write_bytes(payload)
        m.live=0;assert m.call('rtc_copy_initialized',0x50000000,0x50000100)==2
        assert bytes(m.cpu.mem_read(0x50000000,136))==bytes(136)
        step(label,{'bytes':136,'raw_boot_initialization_report':True,'calendar_alarm_fields':'Modeled from authenticated target; prior alarm operation evidence reused.'})
    label='new-setting-packaging'
    if not step(label):
        prepare=module('rtc_setting_packaging',source/'apps/rtc/prepare.py')
        parts,address,identity=prepare.base.image_parts(image)
        raw=bytearray((out/'session-template.bin').read_bytes());raw[48:80]=identity
        epoch,generation,lease=struct.unpack_from('<3Q',raw,24)
        request,contract=prepare.request_bytes(address,identity,raw,epoch,generation,lease)
        launch=out/'launcher'
        for name,data in [('request.bin',request),('contract.bin',contract),('session.bin',raw),('identity.bin',identity)]:(launch/name).write_bytes(data)
        (launch/'prepared.json').write_text(json.dumps({'segments':[{'name':'session.bin','address':0x41f00000},{'name':'identity.bin','address':address}]}))
        assert struct.unpack_from('<Q',contract,184)[0]==784
        assert contract[192:224]==hashlib.sha256(raw).digest()
        step(label,{'source':'Actual explicit calendar/session/request producers and final linked identity','session_bytes':784})
