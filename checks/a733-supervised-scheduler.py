#!/usr/bin/env python3
"""Execute the complete supervised scheduler with Arm GIC priority semantics.

Actual FreeRTOS tasks, context handlers and snapshot publication execute. USB
controller/TinyUSB calls are stubbed; caches, power and timing remain modeled.
"""
import argparse,ctypes,importlib.util,json,struct
from pathlib import Path
from unicorn import UC_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_PC,UC_ARM64_REG_LR
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('cached_scheduler',ROOT/'checks/a733-smp-cached.py')
model=importlib.util.module_from_spec(spec);spec.loader.exec_module(model)


class Scheduler(model.Machine):
    def __init__(self,image,refuse_secure_mask=False):
        super().__init__(image)
        self.secure_pmr=[0]*8;self.refuse_secure_mask=refuse_secure_mask
        self.mask_refused=False;self.completed=False;self.usb_calls={}
        self.secure_mask_initialized=set();self.priority_probed=False
        self.el0_wait_cores=set()
        self.regs[0].update(tcr_el3=0,ttbr0_el3=0,mair_el3=0)
        self.mmio.update({0x02050014:0,0x02050010:0,0x070501c0:0,0x070501d4:0})
        for core in (6,7):self.regs[core]['midr_el1']=0x414fd0b1
        # Use the producer's version-three ABI, not an armed-watchdog fixture.
        build=self.read(self.symbols['a733_smp_build_id'],32)
        contract=struct.pack('<6Q',0x4153373353554333,3,224,5,7,model.MASK-7)+build
        contract+=struct.pack('<18Q',0,24000000,480000000,480000000,9,*([0]*7),1,0,1,2,3,4)
        ctypes.memmove(self.ram_pointer+0x1004000,contract,len(contract))
        with image.open('rb') as stream:
            elf=model.ELFFile(stream)
            for name in ('ready','snapshot'):
                definitions=[s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()
                             if s.name==name and s['st_shndx']!='SHN_UNDEF']
                assert definitions==[self.symbols[name]],'Ambiguous observer symbol: '+name
        self.usb_stubs={self.symbols[n]:(n,value) for n,value in (
            ('a733_usb_prepare',0),('a733_usb_poll',0),('tusb_rhport_init',1),
            ('tud_task_ext',0),('tud_mounted',0))}

    def nonsecure(self,core):
        r=self.regs[core];return r['currentel']<12 and bool(r['scr_el3']&1)

    def priority(self,core,interrupt):
        address=self.rdist[core]+0x10400+interrupt
        raw=(self.mmio.get(address&~3,0)>>(8*(address&3)))&255
        return (raw<<1)&255

    def memory(self,u,access,address,size,value,core):
        write=access==UC_MEM_WRITE
        for frame,r in enumerate(self.rdist):
            if r+0x10400<=address and address+size<=r+0x10420:
                assert size in (1,4) and (size==1 or address%4==0)
                nonsecure=self.nonsecure(core);result=0
                for byte in range(size):
                    a=address+byte;word=a&~3;shift=(a&3)*8;irq=a-r-0x10400
                    group1=bool(self.mmio[r+0x10080]&(1<<irq))
                    original=self.mmio.get(word,0);raw=(original>>shift)&255
                    if write:
                        v=(value>>(8*byte))&255
                        if not nonsecure or group1:
                            raw=(((v>>1)|128) if nonsecure else v)&0xf8
                            self.mmio[word]=(original&~(255<<shift))|(raw<<shift)
                        if nonsecure and v==255:self.priority_probed=True
                    else:result|=(((raw<<1)&255 if group1 else 0) if nonsecure else raw)<<(8*byte)
                if not write:u.mem_write(address,result.to_bytes(size,'little'))
                return
        if write and address in (r+0x10080 for r in self.rdist):value&=0xfff0ffff
        if write and address==self.symbols['ready'] and value:
            assert core==0 and size==4 and value==1
            self.completed=True
        return super().memory(u,access,address,size,value,core)

    def instruction(self,u,address,size,core):
        if self.refuse_secure_mask and address==self.symbols['a733_cached_el3_fault']:
            self.mask_refused=True;u.emu_stop();return
        if address in self.usb_stubs:
            name,value=self.usb_stubs[address]
            assert core==0 and self.regs[core]['currentel']==4
            self.usb_calls[name]=self.usb_calls.get(name,0)+1
            self.put(core,'x0',value);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR));return
        if address not in self.decoded:self.decoded[address]=next(self.decoder.disasm(bytes(u.mem_read(address,4)),address))
        ins=self.decoded[address];op,args=ins.mnemonic,ins.op_str.split(', ');r=self.regs[core]
        if op=='wfe' and r['currentel']==0:
            # DDI0616A.a E3.2 SCTLR_EL1.nTWE: a blocking EL0 WFE may
            # trap with EC=1. The FreeRTOS synchronous handler accepts SVC
            # only. Require permission independently of modeled wake events.
            assert r['sctlr_el1']&(1<<18),('EL0 lock wait can trap',core,hex(address))
            self.el0_wait_cores.add(core)
        if op=='msr' and args[0]=='icc_pmr_el1':
            value=self.reg(core,args[1]);ns=self.nonsecure(core) and bool(r['scr_el3']&4)
            if ns:
                # IHI0069G4.8.1: Secure mask below0x80 makes NS writes ignored.
                if self.secure_pmr[core]&128:self.secure_pmr[core]=((value>>1)|128)&0xf8
            elif not self.refuse_secure_mask:
                self.secure_pmr[core]=value&0xf8
                if value==255:self.secure_mask_initialized.add(core)
            r['icc_pmr_el1']=(self.secure_pmr[core]<<1)&255 if self.secure_pmr[core]&128 else 0
            u.reg_write(UC_ARM64_REG_PC,address+4);return
        if op=='mrs' and args[1]=='icc_pmr_el1':
            mask=self.secure_pmr[core]
            ns=self.nonsecure(core) and bool(r['scr_el3']&4)
            value=((mask<<1)&255 if mask&128 else 0) if ns else mask
            self.put(core,args[0],value);u.reg_write(UC_ARM64_REG_PC,address+4);return
        if op=='mrs' and args[1]=='icc_rpr_el1':
            irq=self.irq_active[core];value=255 if irq is None else self.priority(core,irq)
            self.put(core,args[0],value);u.reg_write(UC_ARM64_REG_PC,address+4);return
        if op=='mrs' and args[1]=='icc_iar1_el1' and self.pending[core]:
            # This image uses only equally prioritized SGI0 and timerPPI30.
            for irq in self.pending[core]:
                assert irq in (0,30) and self.priority(core,irq)==0xe0
                assert self.mmio[self.rdist[core]+0x10100]&(1<<irq)
                assert self.mmio[self.rdist[core]+0x10080]&(1<<irq)
            assert r['icc_pmr_el1']>0xe0
        return super().instruction(u,address,size,core)

    def execute(self):
        announced=False;last_rounds=None;last_progress_tick=0
        for quantum in range(100000):
            for index in range(8):
                c=(index+quantum)%8
                if not self.active[c]:continue
                u=self.cpus[c];r=self.regs[c]
                if c==0 and r['cntp_ctl_el0']&3==1 and self.clock>=r['cntp_cval_el0']:self.pending[c].add(30)
                u.emu_start(u.reg_read(UC_ARM64_REG_PC),0x42200000,count=(211,347,509,673,887)[(quantum+c)%5])
                if self.completed:return self.summary()
                if self.q(0x42000000+24)==5:raise AssertionError(('Native terminal refusal',self.summary()))
            if len(self.first_task)==8 and not announced:
                print('All eight cores executing FreeRTOS tasks',flush=True);announced=True
            if announced:
                base=self.symbols['a733_smp_core_results']
                rounds=tuple(self.q(base+c*128) for c in range(8));ticks=self.q(base+32)
                if rounds!=last_rounds:
                    last_rounds=rounds;last_progress_tick=ticks
                elif ticks-last_progress_tick>=256:
                    raise AssertionError(('No FP-workload progress for 256 scheduler ticks',rounds,ticks))
            self.clock+=1000
        raise AssertionError(('Scheduler instruction budget',self.summary()))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image',type=Path,required=True);parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--case',choices=('secure-mask-refusal','eight-core-scheduler'),action='append')
    a=parser.parse_args();a.output.mkdir(parents=True,exist_ok=bool(a.case))
    if not a.case or 'secure-mask-refusal' in a.case:
        failure=Scheduler(a.image,True);c=6;u=failure.cpus[c]
        failure.put(c,'x0',c);failure.put(c,'x1',5);failure.put(c,'x2',24000000)
        failure.put(c,'x3',480000000);failure.put(c,'x4',failure.rdist[c])
        u.emu_start(failure.symbols['a733_cached_enter_el1'],0x42200000,count=1000)
        assert failure.mask_refused and not failure.el1_enabled and not failure.first_task
        (a.output/'secure-mask-refusal.json').write_text(json.dumps({'pass':True,'image_sha256':model.sha(a.image),
            'secure_pmr':failure.secure_pmr[c],'EL1':sorted(failure.el1_enabled)},indent=2)+'\n')
        print('PASS Secure PMR readback refusal before EL1',flush=True)
    if a.case==['secure-mask-refusal']:return
    prior=json.loads((a.output/'secure-mask-refusal.json').read_text())
    assert prior['pass'] and prior['image_sha256']==model.sha(a.image)
    machine=Scheduler(a.image)
    try:state=machine.execute()
    except BaseException:
        machine.save(a.output/'failure-machine.pkl')
        (a.output/'failure-state.json').write_text(json.dumps(machine.summary(),indent=2)+'\n');raise
    assert state['EL3']==state['EL1']==state['task_cores']==list(range(8))
    assert machine.secure_mask_initialized==set(range(8)) and machine.priority_probed
    # a733_smp_boot made the EL3 security setup once, at EL3 on CPU0.
    assert machine.security==model.el3_security_writes(),machine.security
    assert machine.q(machine.symbols['ready'])&0xffffffff==1
    snapshot=struct.unpack('<42Q',machine.read(machine.symbols['snapshot'],336))
    assert snapshot[1]>snapshot[0]
    for c in range(8):
        rounds,background,sgis,failed,ticks=snapshot[2+c*5:7+c*5]
        assert rounds==32 and background>0 and sgis>0 and failed==0
        if c==0:assert ticks>=32
    (a.output/'state.json').write_text(json.dumps(state,indent=2)+'\n')
    (a.output/'software.json').write_text(json.dumps({'image_sha256':model.sha(a.image),'physical_access':False,
        'sources':{n:model.sha(ROOT/n) for n in ('checks/a733-supervised-scheduler.py','checks/a733-smp-cached.py')},
        'secure_pmr_initialized':sorted(machine.secure_mask_initialized),'priority_probe_observed':machine.priority_probed,
        'snapshot':snapshot,'usb_stubs':machine.usb_calls,'cases':{'secure-mask-refusal':True,'eight-core-scheduler':True},
        'el0_wfe_permitted_cores':sorted(machine.el0_wait_cores),'el3_security_writes':len(machine.security),
        'limits':__doc__},indent=2)+'\n')
    print('PASS eight-core FreeRTOS FP/background/SGI/tick workload and copied snapshot',flush=True)
if __name__=='__main__':main()
