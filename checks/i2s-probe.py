#!/usr/bin/env python3
"""Exercise native audio retirement and delayed MMIO completion without USB.

Actual FreeRTOS service cases cover held leases, cancellation/drain, overrun and
serializer-owned EOF. Linked native instructions cover source setup/restore,
stale gate/RWP readbacks, late completion and MBUS FIFO drain. Common monitor
setup/restore and MMIO effects are explicit models; physical order is unproven.
Entry-origin cases require the shared diagnostic patch selected at configure
time with SUNSTONE_I2S_PROBE_ENTRY_PATCH, until that patch is integrated.
"""
import argparse
import gc
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
from harness import CLANG, CACHE, SANITIZE, environment, hook_addresses, Steps
from unicorn import UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_PC, UC_ARM64_REG_LR
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('i2s_probe_machine',ROOT/'checks/memtest-machine.py')
machine=importlib.util.module_from_spec(spec);spec.loader.exec_module(machine)
C=0x02002000;D=0x04601000;G=0x03400000;READY=0x5944414552505353

class Model(machine.Machine):
    def __init__(self,elf,mode=1,gate=0,active=False):
        super().__init__(elf,step=24)
        self.map(0x02000000,0x6000);self.map(D,0x2000)
        self.sys['currentel']=12
        self.reg={C+0x280:0xf8147f00 if active else 0x40147f00,C+0x191c:gate,
                  C+0x199c:(gate&0x60000),G:0x33,D+0x20:0xffff,D+0x34:0xffff}
        self.original=dict(self.reg);self.writes=[];self.reads_by={};self.gate_wait=0;self.target_gate=0
        self.rwp=0;self.enabled=0;self.enable_target=0;self.fifo=0;self.fifo_seen=0
        self.common_init=0;self.common_restore=0;self.fault=None;self.late=False;self.ignore=None
        self.uc.mem_write(self.address('sunstone_probe_input'),struct.pack('<4I',mode,24,3,0))
        self.uc.hook_add(UC_HOOK_MEM_READ,self.mmio_read,begin=0x02000000,end=0x04602fff)
        self.uc.hook_add(UC_HOOK_MEM_WRITE,self.mmio_write,begin=0x02000000,end=0x04602fff)
        hooks={self.address(n):n for n in ('i2s_probe_monitor_init','i2s_probe_monitor_restore','sunstone_probe_fault','sunstone_probe_fail')}
        def boundary(uc,address,size,data):
            name=hooks[address]
            if name in ('sunstone_probe_fault','sunstone_probe_fail'):
                self.fault=(uc.reg_read(UC_ARM64_REG_X0),uc.reg_read(UC_ARM64_REG_X0+1));uc.emu_stop();return
            if name=='i2s_probe_monitor_init':self.common_init+=1
            else:self.common_restore+=1
            uc.reg_write(UC_ARM64_REG_X0,READY);uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_LR))
        hook_addresses(self.uc,hooks,boundary)
    def mmio_read(self,uc,access,address,size,value,data):
        # Only modeled peripheral addresses are handled; image RAM is elsewhere.
        if not (C<=address<C+0x2000 or D<=address<D+0x2000 or G<=address<G+0x10000):return
        self.reads_by[address]=self.reads_by.get(address,0)+1
        v=self.reg.get(address,0)
        if address==G:
            if self.rwp:self.rwp-=1;v|=1<<31
            else:self.enabled=self.enable_target
        elif address==G+0x110:v=self.enabled
        elif address==C+0x199c:
            if self.gate_wait:self.gate_wait-=1
            else:v=self.target_gate;self.reg[address]=v
            if self.late:self.now+=2400001;self.late=False
        elif address==D+0x30 and self.fifo:
            v=1<<31;self.fifo_seen+=1;self.fifo-=1
        elif address==C+0x280 and not v&(1<<29):
            v|=1<<28 # LOCK can remain stale when its detector is disabled.
        self.write(address,v,size)
    def mmio_write(self,uc,access,address,size,value,data):
        if not (C<=address<C+0x2000 or D<=address<D+0x2000 or G<=address<G+0x10000):return
        self.writes.append((address,value))
        if self.ignore==address:return
        if address==G+0x110:self.enabled|=value;self.enable_target=self.enabled
        elif address==G+0x190:self.enable_target&=~value;self.rwp=3
        elif address==G+0x290:self.reg[G+0x210]=self.reg.get(G+0x210,0)&~value
        else:self.reg[address]=value
        if address==C+0x191c:
            self.target_gate=((value&6)<<16)&value;self.gate_wait=3
    def finish(self):
        self.call('sunstone_probe_restore_interrupts',count=2000000)
        assert self.fault is None,self.fault
        assert self.common_restore==1
        for address,mask in ((D+0x20,0xffff),(D+0x34,0xffff),(D+0x38,0xffff),(D+0x28,7),
                             (C+0x704,0x10001),(C+0x5e4,1),(C+0x191c,0x60006)):
            assert (self.reg.get(address,0)^self.original.get(address,0))&mask==0,hex(address)

def native_cases(elf,steps):
    rows=[]
    if not steps.case('same-image-control-skips-audio-and-DMA'):
        m=Model(elf,mode=0);m.call('monitor_init',count=100000);m.finish()
        assert m.common_init==1 and not m.writes and not m.reads_by
        steps.case('same-image-control-skips-audio-and-DMA',{'passed':True});del m;gc.collect()
    rows.append('same-image-control-skips-audio-and-DMA')
    if not steps.case('late-global-FIFO-stale-gates-and-undefined-LOCK'):
        m=Model(elf,gate=6);m.call('monitor_init',count=200000)
        assert not m.fault,m.fault
        m.reg[C+0x280]=0xf8147f00;m.reg[C+0x199c]=0x60000;m.fifo=7
        m.finish();assert m.fifo_seen==7 and m.reads_by.get(C+0x199c,0)>=4
        pll=[v for a,v in m.writes if a==C+0x280]
        assert len(pll)>=4 and not pll[0]&(1<<27) and not pll[1]&(1<<29) and not pll[2]&(1<<31)
        assert all(v&(1<<30) for v in pll),pll
        steps.case('late-global-FIFO-stale-gates-and-undefined-LOCK',{'passed':True});del m;gc.collect()
    rows.append('late-global-FIFO-stale-gates-and-undefined-LOCK')
    if not steps.case('adopted-PLL-stays-active-and-GIC-RWP-precedes-readback'):
        m=Model(elf,active=True);m.call('monitor_init',count=200000)
        assert not m.fault and m.reads_by.get(C+0x199c,0)>=4
        m.call('i2s_probe_irq_enable',count=200000);assert m.enabled&(1<<(143%32)) and not m.fault
        m.call('i2s_probe_irq_disable',count=200000);assert not m.enabled and not m.fault
        m.finish();assert not any(a==C+0x280 for a,v in m.writes)
        steps.case('adopted-PLL-stays-active-and-GIC-RWP-precedes-readback',{'passed':True});del m;gc.collect()
    rows.append('adopted-PLL-stays-active-and-GIC-RWP-precedes-readback')
    if not steps.case('late-effective-status-refused-before-success'):
        m=Model(elf,gate=6);m.call('monitor_init',count=200000);m.late=True
        m.call('sunstone_probe_restore_interrupts',count=2000000)
        assert m.fault and m.fault[0]==0x4c,m.fault
        steps.case('late-effective-status-refused-before-success',{'passed':True});del m;gc.collect()
    rows.append('late-effective-status-refused-before-success')
    if not steps.case('failed-route-restoration-records-origin'):
        m=Model(elf);m.call('monitor_init',count=200000);m.ignore=D+0x34
        m.reg[D+0x34]=3
        m.call('sunstone_probe_restore_interrupts',count=2000000)
        assert m.fault and m.fault[0]==0x4d,m.fault
        steps.case('failed-route-restoration-records-origin',{'passed':True});del m;gc.collect()
    rows.append('failed-route-restoration-records-origin')
    if not steps.case('final-quiescence-waits-global-FIFO'):
        m=Model(elf);m.write(m.address('closed'),1,4)
        m.write(m.address('worker_stopped'),1,4);m.fifo=7
        assert m.call('sunstone_probe_quiesce',count=200000)==READY and m.fifo_seen==7 and not m.fault
        steps.case('final-quiescence-waits-global-FIFO',{'passed':True});del m;gc.collect()
    rows.append('final-quiescence-waits-global-FIFO')
    name='actual-audio-constructors-before-scheduler'
    if not steps.case(name):
        m=Model(elf);m.call('monitor_init',count=200000);assert not m.fault
        m.sys.update(currentel=4,sctlr_el1=0x30d00800)
        reached=[]
        hook_addresses(m.uc,[m.address('vTaskStartScheduler')],lambda uc,*_: (reached.append(True),uc.emu_stop()))
        m.call('kernel_main',count=4000000)
        assert reached and not m.fault,(reached,m.fault,hex(m.pc()))
        service=m.address('service');layout=m.e.layouts['dma_service']
        assert m.read(service+layout['initialized'],4)==1
        cookie=m.e.layouts['dma_service_irq']['cookie']
        assert m.read(service+layout['irq']+cookie)==m.address('irq')
        steps.case(name,{'passed':True});del m;gc.collect()
    rows.append(name)
    return rows

def entry_cases(elf,out,steps):
    """Execute the entry assembly and real library before cache maintenance.

    MMIO values establish an explicit modeled baseline, not board authority.
    Bad cases change one inherited input and require its retained origin.
    """
    cases=[('baseline',None,0,0,0),('wrong-EL','currentel',8,0x11,0),
           ('wrong-core','mpidr_el1',0x81000100,0x12,0),
           ('cached-EL3','sctlr_el3',0xc51838,0x13,0),
           ('dynamic-OFF',0x07053000,0x100,0x15,0x07053000),
           ('pending-ack',0x07053008,0x100,0x15,0x07053008),
           ('wake-enabled',0x07050204,0,0x15,0x07050204)]
    rows=['actual-entry-'+case[0] for case in cases]
    if all(steps.case(name) for name in rows):return rows
    spec=importlib.util.spec_from_file_location('i2s_shared_probe',ROOT/'checks/board-probe.py')
    shared=importlib.util.module_from_spec(spec);spec.loader.exec_module(shared)
    input_path=out/'entry-input.bin';input_path.write_bytes(struct.pack('<4I',0,24,1,0))
    attempt=1
    while (out/f'entry-session-{attempt}').exists():attempt+=1
    plan=shared.probe.prepare(elf,shared.probe.inspect(elf),[input_path],out/f'entry-session-{attempt}')
    for label,field,value,site,address in cases:
        name='actual-entry-'+label
        if steps.case(name):continue
        m=shared.Model(elf,plan)
        expected_value=value
        # The optional owner preflights the same inherited inputs before its
        # policy writes and records their more specific ownership sites.
        if 'transition' in m.e.symbols:
            site={'dynamic-OFF':0x83,'pending-ack':0x83,'wake-enabled':0}.get(label,site)
            if label=='dynamic-OFF':address=0x07053008;expected_value=0
        m.sys.update(mpidr_el1=0x81000000,sctlr_el3=0xc50838)
        if isinstance(field,str):m.sys[field]=value
        elif field is not None:m.write(field,value,4)
        reached=[]
        hook_addresses(m.uc,[m.address('invalidate_data_cache')],lambda uc,*_: (reached.append(True),uc.emu_stop()))
        m.call('monitor_entry',count=300000)
        rtc=m.rtc()
        if site:
            assert not reached and rtc['fault']==0xfa030000|site,(name,reached,rtc)
            assert rtc['esr']==expected_value and rtc['far']==address,(name,rtc)
        else:
            assert reached and not rtc['fault'],(name,reached,rtc)
            assert m.read(m.address('sunstone_probe_deadline'))>m.now
        steps.case(name,{'passed':True,'retained':rtc})
        del m;gc.collect()
    return rows

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--build',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    p.add_argument('--resume',action='store_true');p.add_argument('--native-only',action='store_true')
    a=p.parse_args();a.out.mkdir(parents=True,exist_ok=a.resume);rows={};steps=Steps(a.out,'commands.json')
    def run(label,cmd):
        steps.command(label,cmd,timeout=30)
        return Path(steps.steps[label]['log']).read_text()
    sources=['checks/i2s-platform.c','drivers/audio/a733_i2s.c','drivers/audio/a733_i2s_platform.c',
             'drivers/pinctrl/a733_pins.c','drivers/pinctrl/a733_pin_tables.c','drivers/audio/a733_audio_leases.c','drivers/clock/a733_clock_leases.c','drivers/audio/a733_i2s_stream.c',
             'drivers/dma/a733_dma.c','drivers/dma/a733_dma_controller.c']
    if not a.native_only:
      run('platform-build',[CLANG,'-std=c11','-g','-O1','-Wall','-Wextra','-Werror',*SANITIZE,'-I'+str(ROOT/'include'),
                          *[ROOT/v for v in sources],'-o',a.out/'platform'])
      run('reset-ldo',[a.out/'platform','--reset-ldo']);run('platform-baseline',[a.out/'platform'])
      rows['reset-default-LDO-and-parent-baseline']=True
    for label,target,case in ([] if a.native_only else [('held-busy-drain','i2s-retire-model',19),('held-overrun','i2s-retire-model',20),('file-EOF','i2s-file-retire-model',None)]):
        cmd=[CACHE/'host/usr/bin/qemu-system-aarch64','-machine','virt,gic-version=3,secure=off,virtualization=off',
             '-cpu','cortex-a55','-smp','2','-m','128M','-nographic','-monitor','none','-semihosting-config','enable=on,target=native',
             '-kernel',a.build/(target+'.elf')]
        if case is not None:cmd+=['-device',f'loader,addr=0x47fff000,data={case},data-len=4']
        text=run(label,cmd);assert 'PASS I2S' in text,text;rows[label]=True
    rows.update({name:True for name in native_cases(a.build/'sunstone-a733-i2s-probe.elf',steps)})
    rows.update({name:True for name in entry_cases(a.build/'sunstone-a733-i2s-probe.elf',a.out,steps)})
    (a.out/'result.json').write_text(json.dumps({'passed':True,'physical_access':False,'cases':rows,
        'image_sha256':hashlib.sha256((a.build/'sunstone-a733-i2s-probe.elf').read_bytes()).hexdigest(),
        'modeled_boundaries':['common monitor setup and interrupt restore','MMIO propagation and counters','DMA/FIFO sample effects'],
        'commands_are_host_only':True},indent=2)+'\n')
    print('PASS I2S native entry and restoration' if a.native_only else
          'PASS I2S return: sanitizer parent, actual FreeRTOS retirement, native entry and restoration')
if __name__=='__main__':main()
