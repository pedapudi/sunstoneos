#!/usr/bin/env python3
"""Actual single-core GPU admission and retained startup; hardware remains modeled."""
import argparse,hashlib,importlib.util,json,re,struct,subprocess,types
from pathlib import Path
from unittest.mock import patch
from unicorn import UC_HOOK_MEM_READ
from unicorn.arm64_const import *
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
shared=module('gpu_shared_startup_checks',ROOT/'checks/storage-read-single-core.py')
prep=module('gpu_single_core_preparation',ROOT/'apps/gpu-render/prepare.py')
class Machine(shared.Machine):
    def __init__(self,image,session,variant='nominal'):
        def request(address,identity,raw,epoch,generation,lease):
            return prep.request_bytes(address,identity,epoch,generation,lease,raw,100,raw[80:112])
        compat=types.SimpleNamespace(session_check=prep.session_check,embedded_reference=lambda image,raw:None,request_bytes=request)
        self.events={a for a,op in image.ops.items() if op.split()[0].lower() in ('mrs','msr','at','wfe')}
        self.publish=image.address('a733_cpu_coherence_publish')
        self.events.update((self.publish,image.address('platform_panic')))
        with patch.object(shared,'module',lambda *a:compat):super().__init__(image,session,variant)
        self.cpu.mem_map(0x48000000,0x08000000)
        self.cpu.mem_map(0x51000000,0x28000000)
        self.cpu.hook_add(UC_HOOK_MEM_READ,self.monitor_read,begin=0x42000000,end=0x421fffff)
        contract=bytearray(self.contract);struct.pack_into('<Q',contract,0,prep.CONTRACT_MAGIC)
        self.contract=bytes(contract);self.cpu.mem_write(0x42004000,self.contract)
        self.el=12
        if variant=='tampered-session':self.cpu.mem_write(0x41f00000+216,bytes([session[216]^1]))
        if variant=='tampered-contract':self.cpu.mem_write(0x42004000+224,bytes([contract[224]^1]))
    def monitor_read(self,cpu,access,address,size,value,unused):
        if self.el==4 and self.sysregs['ttbr0_el1']:
            raise AssertionError(('EL1 attempted protected monitor data read',hex(address)))
    def hook(self,cpu,address,size,unused):
        if address not in self.events and address!=0x43fff000 and address!=getattr(self,'stop_at',None):return
        if address==self.publish:
            self.publications.append((cpu.reg_read(UC_ARM64_REG_X0),cpu.reg_read(UC_ARM64_REG_X1)))
            self.ret(0 if self.variant=='publication-failure' else 1);return
        read=re.fullmatch(r'mrs\s+x(\d+),\s*(CurrentEL|sctlr_el3)',self.e.ops.get(address,''),re.I)
        if read:
            cpu.reg_write(UC_ARM64_REG_X0+int(read[1]),self.el if read[2].lower()=='currentel' else 0);cpu.reg_write(UC_ARM64_REG_PC,address+4);return
        return super().hook(cpu,address,size,unused)
    def translate(self,va,write):
        assert self.sysregs['tcr_el1']==0x200803019
        self.sysregs['tcr_el1']=0x200803519
        try:return super().translate(va,write)
        finally:self.sysregs['tcr_el1']=0x200803019
    def call(self,name,*args):
        self.finished=False;self.parked=False
        self.cpu.reg_write(UC_ARM64_REG_SP,0x43ffeff0);self.cpu.reg_write(UC_ARM64_REG_LR,0x43fff000)
        for i,value in enumerate(args):self.cpu.reg_write(UC_ARM64_REG_X0+i,value)
        self.cpu.emu_start(self.e.address(name),0,count=100000000)
        assert self.finished or self.parked,(name,hex(self.cpu.reg_read(UC_ARM64_REG_PC)))
        return self.cpu.reg_read(UC_ARM64_REG_X0)
def profile_checks(image,bundle,out,allcore_image):
    from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
    out.mkdir(parents=True,exist_ok=False)
    native=module('gpu_profile_image',ROOT/'checks/storage-read-native.py')
    raw=(bundle/'session-template.bin').read_bytes();results=[]
    for path,target,profile,modes in [(image,'sunstone-a733-gpu-compute-single-core','gpu-single-core-supervised-v2',(1,2)),
                                     (allcore_image,'sunstone-a733-gpu-render','gpu-supervised-v2',(1,))]:
        body=path.read_bytes();manifest={'target':target,'execution_profile':profile,'workload':'fixed-arithmetic-repeat','final_elf':hashlib.sha256(body).hexdigest()}
        e=native.Image(path);cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);cpu.mem_map(0x41000000,0x1200000);cpu.mem_map(0x43000000,0x10000)
        for address,data in e.parts:
            if data:cpu.mem_write(address,data)
        def hook(cpu,address,size,unused):
            if address==0x4300ff00:cpu.emu_stop()
        cpu.hook_add(UC_HOOK_CODE,hook)
        for mode in (0,1,2,3):
            session=bytearray(raw);struct.pack_into('<I',session,532,mode)
            accepted=True
            try:prep.image_manifest_check(manifest,body,session)
            except ValueError:accepted=False
            assert accepted==(mode in modes),(target,mode,'host')
            cpu.mem_write(0x41f00000,bytes(session));cpu.reg_write(UC_ARM64_REG_SP,0x4300fef0);cpu.reg_write(UC_ARM64_REG_LR,0x4300ff00)
            cpu.reg_write(UC_ARM64_REG_X0,0x41f00000);cpu.reg_write(UC_ARM64_REG_X1,0)
            cpu.emu_start(e.address('a733_board_session_valid'),0,count=100000)
            assert bool(cpu.reg_read(UC_ARM64_REG_X0))==(mode in modes),(target,mode,'native')
            results.append({'target':target,'mode':mode,'accepted':accepted})
    (out/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,cases=results),indent=2)+'\n')
    print('PASS actual native and host USB mode/profile admission; no startup replay')

def run(image,bundle,out):
    out.mkdir(parents=True,exist_ok=False)
    native=module('gpu_single_core_image',ROOT/'checks/storage-read-native.py');e=native.Image(image);e.path=image;e.ops={}
    manifest=json.loads((image.parent/'gpu-single-core/manifest.json').read_text())
    prep.image_manifest_check(manifest,image.read_bytes());prepared=json.loads((bundle/'prepared.json').read_text())
    raw=bytearray((bundle/'session-template.bin').read_bytes())
    struct.pack_into('<3Q',raw,24,prepared['epoch'],prepared['generation'],prepared['lease']);raw[48:80]=bytes.fromhex(prepared['build_id']);raw=bytes(raw)
    for line in subprocess.check_output(['/usr/lib/llvm-21/bin/llvm-objdump','-d','--no-show-raw-insn',str(image)],text=True).splitlines():
        match=re.match(r'^\s*([0-9a-f]+):\s+(.+)$',line)
        if match:e.ops[int(match[1],16)]=match[2].strip()
    results={}
    def passed(name):
        results[name]=True;(out/'completed-cases.json').write_text(json.dumps(results,indent=2)+'\n')
    m=Machine(e,raw);m.call('sd_single_core_monitor_init');assert not m.parked
    assert m.read(0x03400088,4)&(1<<31) and not m.read(0x03400d08,4)&(1<<31)
    assert not m.read(0x03400c14,4)&(3<<30) and m.read(0x0340045c,4)>>24==0xd8 and m.read(0x034062f8)==0
    assert not m.read(0x03400108,4)&(1<<31)
    passed('full-contract-session-and-disabled-primary-routed-spi95')
    sealed=m.call('gpu_application_session');assert bytes(m.cpu.mem_read(sealed,592))==raw
    lo,hi=e.address('__gpu_retained_start'),e.address('__gpu_retained_end')
    assert lo<=sealed<sealed+592<=hi and len(m.publications)==2,(hex(lo),hex(sealed),hex(hi),m.publications)
    runtime_contract=m.call('sd_single_core_contract');assert lo<=runtime_contract<runtime_contract+224<=hi
    assert lo<=e.address('gpu_runtime_secondary_off')<hi
    before=bytes(m.cpu.mem_read(lo,hi-lo));m.cpu.mem_write(0x41f00000,bytes(592))
    m.el=4;m.cpu.reg_write(UC_ARM64_REG_PSTATE,0x3c5);m.stop_at=e.address('kernel_main');m.finished=False
    m.cpu.emu_start(e.address('_start'),0,count=3000000);assert m.finished
    assert bytes(m.cpu.mem_read(lo,hi-lo))==before
    passed('sealed-input-and-memory-binding-survive-el1-bss-clear')
    m.stop_at=e.address('a733_smp_diagnostic_start');m.sysregs['sctlr_el1']=0x30d00800
    m.call('kernel_main');assert not m.parked
    assert m.sysregs['ttbr0_el1']==e.address('a733_cached_el1_l1') and m.sysregs['tcr_el1']==0x200803019
    assert m.call('a733_smp_device_preparation_admitted')==1 and m.call('a733_device_memory_ready')==1
    for address,attr,share in [(0x48000000,255,3),(0x77fff000,255,3),(0x78000000,68,3),(0x78fff000,68,3),(0x01800000,0,2)]:
        assert m.call('aarch64_device_translate',address,1,0x43010000)==1
        assert struct.unpack('<QII',m.cpu.mem_read(0x43010000,16))==(address,attr,share)
    assert m.call('aarch64_device_translate',0x42005000,1,0x43010000)==0
    passed('strict-large-mapping-scan-publishes-both-pools-and-denies-monitor')
    m.stop_at=e.address('vTaskStartScheduler');m.call('a733_smp_diagnostic_start');assert not m.parked
    passed('actual-shared-provider-service-and-single-core-task-construction')
    for variant in ('tampered-session','tampered-contract','watchdog-active','publication-failure','secondary-active','secondary-control','secondary-mode'):
        f=Machine(e,raw,variant);f.call('sd_single_core_monitor_init')
        assert f.parked and f.read(0x42000000+24)==5 and f.read(0x03400000,4)==0x37
        passed(variant+'-retained-before-gic')
    for target,profile in [('sunstone-a733-gpu-render','gpu-supervised-v2'),('sunstone-a733-gpu-compute-single-core','gpu-supervised-v2')]:
        try:prep.image_manifest_check({**manifest,'target':target,'execution_profile':profile},image.read_bytes())
        except ValueError:pass
        else:raise AssertionError('Cross-mode manifest accepted')
    passed('cross-mode-profiles-refused')
    (out/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,image_sha256=hashlib.sha256(image.read_bytes()).hexdigest(),cases=results,
        scope='Actual native admission, session SHA-256, retained placement, EL1 clearing, table construction, full mapping scan, static task/provider/service construction. Counter, cache publication, GIC/CPU system registers and descriptor-based hardware walk modeled. No GPU arithmetic executed or injected; prior arithmetic evidence reused.'),indent=2)+'\n')
    print('PASS single-core GPU admission and retained startup; hardware modeled')
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('image','bundle','output'):p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--profiles-only',action='store_true')
    p.add_argument('--allcore-image',type=Path)
    a=p.parse_args()
    if a.profiles_only:
        if a.allcore_image is None:p.error('--allcore-image is required for profiles-only')
        profile_checks(a.image,a.bundle,a.output,a.allcore_image)
    else:run(a.image,a.bundle,a.output)
