"""New RTC service binding and actual native provider callbacks; no hardware."""
import hashlib
import importlib.util
import json
import struct
from elftools.elf.elffile import ELFFile
from unicorn import UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2

def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m

def setup(image,out,source):
    prior=module('rtc_service_prior',source/'checks/rtc-application-cases.py')
    symbols,segments,layouts,identity=prior.setup(image,out)
    names=['board_application_task_privileged','board_application_start','rtc_task','rtc_service_client','rtc_service_prepare','xTaskCreateStaticAffinitySet',
           'rtc_service_read','rtc_service_write','rtc_service_now','rtc_service_live','providers',
           'a733_platform_lock_enter','a733_platform_lock_leave','a733_counter_clock_now','aarch64_device_translate']
    sizes={}
    with image.open('rb') as f:
        elf=ELFFile(f)
        for name in names:
            candidates={int(s['st_value']) for s in elf.get_section_by_name('.symtab').iter_symbols() if s.name==name and s['st_value']}
            assert len(candidates)==1,(name,candidates);symbols[name]=next(iter(candidates))
        for cu in elf.get_dwarf_info().iter_CUs():
            for die in cu.iter_DIEs():
                n=die.attributes.get('DW_AT_name');size=die.attributes.get('DW_AT_byte_size')
                if die.tag=='DW_TAG_structure_type' and n and size:sizes[n.value.decode()]=int(size.value)
    assert sizes['a733_clock_token']==48 and sizes['rtc_service_result']==200 and sizes['rtc_service_output']==2416
    for name,fields in {'rtc_application':['clock','client_handles','session'],'a733_clock_token':['id','owner','shared','exclusive'],
        'a733_native_usb':['clocks','counter'],'board_provider':['shared'],'a733_clock_leases':['failed'],
        'aarch64_translation':['physical','attributes','shareability']}.items():assert all(f in layouts[name] for f in fields),name
    (out/'service-preflight.json').write_text(json.dumps({'symbols':symbols,'layouts':layouts,'sizes':sizes,'build_id':identity.hex(),'executed':False},indent=2)+'\n')
    return prior,symbols,layouts,identity

def run(out,source):
    image=out/'build/sunstone-a733-rtc.elf';prior,symbols,layouts,identity=setup(image,out,source)
    class Machine(prior.Machine):
        def __init__(self):
            self.service_ready=False
            super().__init__(image,out,source,startup=6)
            self.symbols.update(symbols);self.service_ready=True;self.time=1000;self.held=0;self.created=[];self.stores=[]
            self.word(0x07090000,0x401d);self.word(0x07090004,1)
            assert self.call('rtc_session_prepare',0x43200000)==1
            self.word(0x03400000,0x33);assert self.call('rtc_boot_route')==1
            board=symbols['board'];b=layouts['board_application']
            self.cpu.mem_write(board+b['admitted_session'],bytes(self.session[:592]));self.cpu.mem_write(board+b['session'],bytes(self.session[:592]))
            self.cpu.mem_write(board+layouts['a733_board_session']['deadline_us'],struct.pack('<Q',10000000))
            assert self.call('board_application_start')==1 and len(self.created)==3
            token=symbols['rtc']+layouts['rtc_application']['clock'];fields=layouts['a733_clock_token']
            for name,value,size in [('id',1,8),('owner',int.from_bytes(self.session[616:624],'little'),8),('shared',0x2000000004001,16),('exclusive',0,16)]:self.cpu.mem_write(token+fields[name],value.to_bytes(size,'little'))
            self.cpu.hook_add(UC_HOOK_MEM_WRITE,self.store)
        def provider(self):return symbols['providers']+layouts['board_provider']['shared']
        def hook(self,cpu,address,size,unused):
            if not self.service_ready:return super().hook(cpu,address,size,unused)
            arg=lambda n:cpu.reg_read(UC_ARM64_REG_X0+n)
            s=self.symbols
            if address==s['a733_smp_counter']:self.ret(self.time*24);return
            if address==s['xTaskCreateStaticAffinitySet']:
                index=len(self.created)
                assert arg(0)==s['rtc_service_client' if index<2 else 'rtc_task'] and arg(7)==1
                assert arg(3)==(index if index<2 else 0) and arg(4)==(3 if index<2 else 4)
                self.created.append(dict(entry=arg(0),context=arg(3),affinity=arg(7),priority=arg(4)));self.ret(0x123400+index*16);return
            if address==s['rtc_service_prepare']:
                assert bytes(cpu.mem_read(arg(0),16))==struct.pack('<2Q',0x123400,0x123410)
                expected=(*struct.unpack_from('<2I',self.session,772),struct.unpack_from('<I',self.session,760)[0])
                (out/'native-prepare-observation.json').write_text(json.dumps({'observed':[arg(1),arg(2),arg(3)],'expected':expected,'producer':'Exact preserved admitted session'})+'\n')
                assert (arg(1),arg(2),arg(3))==expected;self.ret(1);return
            if address==s['a733_platform_lock_enter']:assert not self.held and arg(1)==0;self.held=1;self.ret(1);return
            if address==s['a733_platform_lock_leave']:assert self.held;self.held=0;self.ret(0);return
            if address==s['a733_counter_clock_now']:
                assert arg(0)==self.provider()+layouts['a733_native_usb']['counter'];self.time+=10
                cpu.mem_write(arg(1),struct.pack('<Q',self.time));self.ret(1);return
            if address==s['aarch64_device_translate']:
                for name,value,n in [('physical',arg(0),8),('attributes',0,4),('shareability',2,4)]:cpu.mem_write(arg(2)+layouts['aarch64_translation'][name],value.to_bytes(n,'little'))
                self.ret(1);return
            super().hook(cpu,address,size,unused)
        def store(self,cpu,access,address,size,value,unused):
            if 0x07090000<=address<0x07090100:
                assert self.held;self.stores.append((address,value))
    m=Machine()
    assert m.call('board_application_task_privileged',symbols['rtc_service_client'],1)==1
    assert m.call('board_application_task_privileged',symbols['rtc_service_client'],2)==0
    assert m.call('board_application_task_privileged',symbols['rtc_task'],1)==0
    (out/'admitted-service.bin').write_bytes(m.session)
    before=bytes(m.cpu.mem_read(0x41f00000,784));m.word(0x41f00000+772,65535)
    assert m.call('a733_rtc_session_valid',0x41f00000)==0
    m.cpu.mem_write(0x41f00000,before)
    assert m.call('rtc_service_live')==1
    assert m.call('rtc_service_write',0,0x14,0x123438)==1
    assert m.call('rtc_service_read',0,0x14,0x43300000)==1 and m.word(0x43300000)==0x123438
    assert m.call('rtc_service_write',0,0x00,0)==0,'No runtime source writes'
    m.live=0;assert m.call('rtc_service_write',0,0x10,20744)==0
    assert m.stores==[(0x07090014,0x123438)] and not m.held
    (out/'native-service-result.json').write_text(json.dumps({'passed':True,'session_sha256':hashlib.sha256(m.session).hexdigest(),'created':m.created,'stores':m.stores,'authority':'Software model; nondeployable','scope':'Actual final native validator, sealed handoff, task creation and provider callback instructions; queue prepare modeled (real queue/scheduler covered separately).'},indent=2)+'\n')
