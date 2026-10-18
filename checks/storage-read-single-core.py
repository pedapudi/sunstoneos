#!/usr/bin/env python3
"""Execute admitted single-core storage startup; hardware effects remain modeled."""
import argparse, hashlib, importlib.util, json, struct
from pathlib import Path
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import *
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    result=importlib.util.module_from_spec(spec);spec.loader.exec_module(result);return result
class Machine:
    def __init__(self,image,session,variant='nominal'):
        self.e=image;self.cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
        self.cpu.mem_map(0x02000000,0x07000000)
        self.cpu.mem_map(0x40000000,0x04000000)
        self.cpu.mem_map(0x50000000,0x01000000)
        for address,data in image.parts:self.cpu.mem_write(address,data)
        self.raw=bytes(session);self.variant=variant;self.finished=False;self.parked=False
        self.publications=[];self.writes=[];self.time=24000000
        self.cpu.reg_write(UC_ARM64_REG_PSTATE,0x3cd)
        self.cpu.mem_write(0x41f00000,self.raw)
        self.write(0x03400000,0x37);self.write(0x03400004,7)
        self.write(0x03460008,0,8)
        prep=module('single_core_producer',ROOT/'apps/storage-read/prepare.py')
        prep.session_check(self.raw);prep.embedded_reference(image.path.read_bytes(),self.raw)
        epoch,generation,lease=struct.unpack_from('<3Q',self.raw,24)
        address=image.address('a733_smp_build_id')
        _,contract=prep.request_bytes(address,self.raw[48:80],self.raw,epoch,generation,lease)
        contract=bytearray(contract);struct.pack_into('<Q',contract,0,0x4153373352444331)
        struct.pack_into('<4Q',contract,80,self.time-100,self.time-100+24000000,self.time-100+1440000000,self.time-100+1440000000)
        self.contract=bytes(contract);self.cpu.mem_write(0x42004000,self.contract)
        if variant=='tampered-suffix':self.cpu.mem_write(0x41f00000+760,bytes([self.raw[760]^1]))
        if variant=='watchdog-active':self.write(0x02050014,1)
        if variant=='secondary-active':self.write(0x07053008,8)
        if variant=='secondary-control':self.write(0x07050204,1)
        if variant=='secondary-mode':self.write(0x07050224,1)
        self.cpu.hook_add(UC_HOOK_CODE,self.hook)
    def write(self,a,v,n=4):self.cpu.mem_write(a,int(v).to_bytes(n,'little'))
    def read(self,a,n=8):return int.from_bytes(self.cpu.mem_read(a,n),'little')
    def ret(self,v=0):
        self.cpu.reg_write(UC_ARM64_REG_X0,v);self.cpu.reg_write(UC_ARM64_REG_PC,self.cpu.reg_read(UC_ARM64_REG_LR))
    def hook(self,cpu,a,size,_):
        instruction=int.from_bytes(cpu.mem_read(a,4),'little')
        if a==0x50fff000:self.finished=True;cpu.emu_stop();return
        # The architectural counter and publication are the explicit model
        # boundaries. Admission, copying, SHA-256 and GIC sequencing execute.
        if instruction&0xffffffe0==0xd53be020:
            cpu.reg_write(UC_ARM64_REG_X0+(instruction&31),self.time)
            cpu.reg_write(UC_ARM64_REG_PC,a+4);return
        if instruction&0xffffffe0==0xd5384600:
            # Physical secure GIC priority mask exposes sixteen NS priorities.
            cpu.reg_write(UC_ARM64_REG_X0+(instruction&31),0xf0)
            cpu.reg_write(UC_ARM64_REG_PC,a+4);return
        if a==self.e.address('a733_cpu_coherence_publish'):
            self.publications.append((cpu.reg_read(UC_ARM64_REG_X0),cpu.reg_read(UC_ARM64_REG_X1)))
            self.ret(0 if self.variant=='publication-failure' else 1);return
        if instruction==0xd503205f:
            self.parked=True;cpu.emu_stop();return
        if getattr(self,'stop_at',None)==a:self.finished=True;cpu.emu_stop()
    def call(self,name,*args):
        self.finished=False;self.parked=False
        self.cpu.reg_write(UC_ARM64_REG_SP,0x50ffeff0)
        self.cpu.reg_write(UC_ARM64_REG_LR,0x50fff000)
        for i,value in enumerate(args):self.cpu.reg_write(UC_ARM64_REG_X0+i,value)
        self.cpu.emu_start(self.e.address(name),0,count=3000000)
        assert self.finished or self.parked,name
        return self.cpu.reg_read(UC_ARM64_REG_X0)
def run(image,session,out):
    out.mkdir(parents=True,exist_ok=False)
    native=module('sd_single_core_image',ROOT/'checks/storage-read-native.py')
    e=native.Image(image);e.path=image;raw=session.read_bytes();results={}
    assert not any(n in e.symbols and e.symbols[n]-{0} for n in
      ('a733_cpu_start_init','a733_cpu_primary_continue','a733_smp_pre_scheduler','FreeRTOS_SGI_Handler','monitor_reset'))
    m=Machine(e,raw);m.call('sd_single_core_monitor_init');assert not m.parked
    board=m.call('board_session_snapshot');sd=m.call('storage_read_admitted_session')
    assert bytes(m.cpu.mem_read(board,592))==raw[:592] and bytes(m.cpu.mem_read(sd,880))==raw
    lo=e.address('__monitor_state_start');hi=e.address('__monitor_state_end')
    assert lo<=board<board+592<=hi and lo<=sd<sd+880<=hi and len(m.publications)==2
    before=bytes(m.cpu.mem_read(lo,hi-lo));m.cpu.mem_write(0x41f00000,bytes(880))
    # Execute the actual accepted EL1 _start, including its entire BSS clear.
    m.cpu.reg_write(UC_ARM64_REG_PSTATE,0x3c5);m.stop_at=e.address('kernel_main')
    m.finished=False;m.cpu.emu_start(e.address('_start'),0,count=3000000);assert m.finished
    assert bytes(m.cpu.mem_read(lo,hi-lo))==before
    results['sealed-session-survives-upload-mutation-and-el1-bss-clear']=True
    # Execute the real platform table construction and MMU enable, then inspect
    # its descriptors and the actual AT/PAR translator used by SD admission.
    m.stop_at=e.address('a733_smp_diagnostic_start')
    m.cpu.reg_write(UC_ARM64_REG_CP_REG,(3,0,1,0,0,0x30d00800))
    m.call('kernel_main');assert m.finished and not m.parked
    level1=e.address('level1');level2=e.address('level2')
    assert m.read(level1)&0x31f==0x201 and m.read(level1+8)==level2|3
    assert m.read(level2+16*8)==0x42000000|0x400|0x300|8|1
    assert m.call('aarch64_device_translate',0x04020000,1,0x50010000)==1
    assert struct.unpack('<QII',m.cpu.mem_read(0x50010000,16))==(0x04020000,0,2)
    assert m.call('aarch64_device_translate',e.address('storage_read'),1,0x50010000)==1
    assert struct.unpack('<QII',m.cpu.mem_read(0x50010000,16))==(e.address('storage_read'),0xff,3)
    assert m.call('a733_smp_device_preparation_admitted')==1
    results['actual-platform-mappings-and-single-core-preparation']=True
    # Run the unchanged board constructor and actual kernel static-task/queue
    # creation until scheduler entry. Shared provider constructors execute;
    # hardware polling is deferred to those intended owner tasks.
    m.stop_at=e.address('vTaskStartScheduler')
    m.call('a733_smp_diagnostic_start');assert m.finished and not m.parked
    board=e.address('board');bl=e.layouts['board_application']
    handles=[m.read(board+bl['handles']+i*8) for i in range(8)]
    assert all(handles[i] for i in (0,1,7)) and all(not handles[i] for i in range(2,7))
    shared=e.address('providers')+e.layouts['board_provider']['shared']
    nl=e.layouts['a733_native_usb']
    assert m.read(shared+nl['used'],4)==1
    assert m.read(shared+nl['pmic_task'])==handles[0] and m.read(shared+nl['usb_task'])==handles[1]
    storage=e.address('storage_read');sl=e.layouts['storage_read_application']
    consumer=e.address('storage_read_consumer');cl=e.layouts['storage_read_consumer']
    assert m.read(storage+sl['handle']) and m.read(consumer+cl['handle'])
    assert m.read(storage+sl['slots']+e.layouts['storage_read_slot']['client'])==handles[1]
    results['actual-kernel-tasks-and-shared-provider-startup']=True
    m.time+=24000000
    assert m.call('a733_smp_device_preparation_admitted')==0
    results['expired-startup-refused']=True
    # Exact failed authority stops before providers and GIC configuration.
    for variant in ('tampered-suffix','watchdog-active','publication-failure','secondary-active','secondary-control','secondary-mode'):
        f=Machine(e,raw,variant);f.call('sd_single_core_monitor_init')
        assert f.parked and f.read(0x42000000+24)==5 and f.read(0x42000000+32)==1
        assert f.read(0x03400000,4)==0x37
        results[variant+'-retained-before-gic']=True
    # Execute terminal paths themselves; they cannot call a reset or release.
    for name,args in [('a733_smp_stop',(7,0xabc)),('sd_single_core_exception',(0,0,2))]:
        f=Machine(e,raw);f.call('sd_single_core_monitor_init')
        if name=='sd_single_core_exception':
            # IRQ number and group acknowledge are modeled system-register state.
            f.cpu.reg_write(UC_ARM64_REG_CP_REG,(3,0,12,8,0,29))
        f.call(name,*args);assert f.parked and f.read(0x42000000+24)==5
        if name=='a733_smp_stop':assert f.read(0x42000000+32)==7 and f.read(0x42000000+40)==0xabc
        else:assert f.read(0x42000000+32)==2 and f.read(0x42000000+40)==29
        results[name+'-retains']=True
    (out/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,
      image_sha256=hashlib.sha256(image.read_bytes()).hexdigest(),session_sha256=hashlib.sha256(raw).hexdigest(),
      cases=results,scope='Actual linked single-core monitor admission, full-session SHA-256/publication, retained terminal paths and EL1 BSS clearing. Counter, cache publication, GIC/MMIO and interrupt state modeled; no physical boot or storage claims.'),indent=2)+'\n')
    print('PASS single-core storage session and retained lifetime')
if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('image','session','output'):parser.add_argument('--'+name,type=Path,required=True)
    a=parser.parse_args();run(a.image,a.session,a.output)
