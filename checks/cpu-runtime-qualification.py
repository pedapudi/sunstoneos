#!/usr/bin/env python3
"""Execute the linked current-image terminal/cleanup with modeled peripherals.

This tests shutdown integration, not execution of the preceding 32-round
FreeRTOS workload or physical coherence. Completed workload records are input
fixtures. The real SVC/SMC vectors, fault recorder, C cleanup and assembly
restoration run from the supplied ELF. No hardware access occurs.
"""
import argparse
import hashlib
import importlib.util
import json
import struct
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_WRITE, UC_HOOK_MEM_READ
from unicorn.arm64_const import UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_LR

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('fault_capture', ROOT/'checks/fault-capture.py')
fault = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fault)
Q = lambda n: struct.pack('<Q', n)
EPOCH = 0x73520261024
DEADLINE = 480000000

class Model:
    def __init__(self, image):
        self.image = image
        self.s = image.symbols
        self.uc = image.machine()
        for a,n in ((0x03400000,0x200000),(0x07050000,0x10000),(0x08000000,0x10000)):
            self.uc.mem_map(a,n)
        self.core = 0
        self.reg = [dict(scr_el3=0x530, mpidr_el1=0x81000000|(c<<8), currentel=12,
                         daif=0x3c0, sctlr_el3=0, icc_sre_el3=0,
                         icc_igrpen0_el1=0, icc_igrpen1_el3=0) for c in range(8)]
        self.count = 0
        self.time = 1000
        self.stop = None
        self.parked = set()
        self.cleaned = [set() for _ in range(8)]
        self.postclean = []
        self.writes = []
        self.clean_started = False
        self.corrupt_readback = False
        self.skip_off = False
        self.uc.hook_add(UC_HOOK_CODE,self.code)
        self.uc.hook_add(UC_HOOK_MEM_WRITE,self.write)
        self.uc.hook_add(UC_HOOK_MEM_READ,self.read)
        self.put32(0x03400000,0x30)
        self.put32(0x03400004,3)
        for c in range(8):
            r=0x03460000+c*0x20000
            self.put64(r+8,(c<<40)|(16 if c==7 else 0))
            self.put32(r+0x14,6 if c else 0)
            self.put32(r+0x10400,0x10203040+c)
            self.put32(r+0x1041c,0x50607080+c)
            self.reg[c]['sctlr_el1:s']=0x30d00800
            self.reg[c]['sctlr_el1:ns']=0x30d00800
            self.reg[c]['sp_el1:ns']=0x41f90000+c*4096
            self.reg[c]['sp_el0']=0x41f00000+c*4096
        self.report=self.s['a733_qualification_report']

    def put32(self,a,v): self.uc.mem_write(a,struct.pack('<I',v))
    def put64(self,a,v): self.uc.mem_write(a,Q(v))
    def u64(self,a): return struct.unpack('<Q',self.uc.mem_read(a,8))[0]
    def u32(self,a): return struct.unpack('<I',self.uc.mem_read(a,4))[0]
    def key(self,name):
        # EL1 translation/context/ICC interface registers have Secure and NS
        # banks. Identification, DAIF and top-level group controls do not.
        banked=('sctlr_el1','tcr_el1','ttbr0_el1','ttbr1_el1','mair_el1','vbar_el1',
                'cpacr_el1','cntkctl_el1','tpidr_el1','icc_sre_el1','icc_ctlr_el1',
                'icc_bpr1_el1','icc_igrpen1_el1','sp_el1','tpidrro_el0')
        return name+(':ns' if self.reg[self.core].get('scr_el3',0)&1 else ':s') if name in banked else name
    def get(self,name):
        if name=='cntpct_el0': self.time+=24; return self.time
        return self.reg[self.core].get(self.key(name),0)
    def set(self,name,v): self.reg[self.core][self.key(name)]=v
    def gpr(self,name):
        if name in ('xzr','wzr'): return 0
        return self.uc.reg_read(fault.REGS[int(name[1:])])
    def advance(self,a): self.uc.reg_write(UC_ARM64_REG_PC,a+4)
    def code(self,uc,a,size,unused):
        if a==fault.SENTINEL:
            self.stop='return';uc.emu_stop();return
        if a==self.s['a733_smp_stop']:
            self.stop='native-retained';uc.emu_stop();return
        self.count+=1
        ins=self.image.insn(uc,a);args=ins.op_str.split(', ')
        if ins.mnemonic=='mrs':
            uc.reg_write(fault.REGS[int(args[0][1:])],self.get(args[1]));self.advance(a)
        elif ins.mnemonic=='msr':
            value=int(args[1][1:],0) if args[1].startswith('#') else self.gpr(args[1])
            if args[0]!='spsel':self.set(args[0],value)
            self.advance(a)
            if args[0]=='rmr_el3':self.stop='rom';uc.emu_stop()
        elif ins.mnemonic in ('dsb','dmb','isb','sev','ic','tlbi'):
            self.advance(a)
        elif ins.mnemonic=='dc':
            assert args[0]=='civac',ins.op_str
            address=self.gpr(args[1]);self.cleaned[self.core].add(address)
            for c in range(8):
                assert self.u64(self.report+688+c*1024+8)==EPOCH,('clean before all captures',c)
                assert self.u64(self.report+688+c*1024+104)==EPOCH,('clean before all drains',c)
            self.clean_started=True;self.advance(a)
        elif ins.mnemonic=='svc':
            self.set('esr_el1',0x56000000|int(args[0][1:],0));self.set('elr_el1',a+4)
            self.set('spsr_el1',0)
            uc.reg_write(UC_ARM64_REG_PC,self.s['a733_cached_el1_vectors']+8*128)
        elif ins.mnemonic=='smc':
            self.set('esr_el3',0x5e000000);self.set('elr_el3',a+4)
            uc.reg_write(UC_ARM64_REG_PC,self.s['a733_cached_el3_vectors']+8*128)
        elif ins.mnemonic in ('wfe','wfi'):
            self.stop='park';self.parked.add(self.core);uc.emu_stop()
    def write(self,uc,access,a,size,value,unused):
        if self.clean_started and 0x41200000<=a<0x42000000:
            self.postclean.append((self.core,a,size,value))
        if 0x03460000<=a<0x03560000:
            r=0x03460000+((a-0x03460000)//0x20000)*0x20000
            offset=a-r
            if offset in (0x10180,0x10280):
                dst=a-0x80;self.put32(dst,self.u32(dst)&~value)
            if offset==0x14 and value==2:self.put32(a,6) # applied after hook in code below
        if a==self.report+688+self.core*1024+40 and value==EPOCH and not self.skip_off:
            self.put32(0x07052008+self.core*0x1000,0)
        self.writes.append((self.core,a,size,value))
    def read(self,uc,access,a,size,value,unused):
        if self.corrupt_readback and self.clean_started and a==0x03460000+3*0x20000+0x1041c:
            self.put32(a,self.u32(a)^1)
    def apply_side_effects(self):
        # Unicorn write hooks run before the store; model completion values
        # are applied between bounded instruction slices.
        for c in range(8):
            r=0x03460000+c*0x20000
            if self.u32(r+0x14)==2:self.put32(r+0x14,6)
    def call(self,c,symbol,args=(),budget=200000):
        self.core=c;self.stop=None
        for i,v in enumerate(args):self.uc.reg_write(fault.REGS[i],v)
        self.uc.reg_write(UC_ARM64_REG_SP,self.s['__a733_smp_boot_stack_top'])
        self.uc.reg_write(UC_ARM64_REG_LR,fault.SENTINEL)
        self.uc.reg_write(UC_ARM64_REG_PC,self.s[symbol])
        for _ in range(budget//64):
            self.uc.emu_start(self.uc.reg_read(UC_ARM64_REG_PC),0,count=64)
            self.apply_side_effects()
            if self.stop:return self.uc.reg_read(fault.REGS[0])
        raise AssertionError(('call budget',symbol,hex(self.uc.reg_read(UC_ARM64_REG_PC))))
    def initialize(self):
        assert self.call(0,'a733_qualification_boot',(EPOCH,DEADLINE))==1
        for c in range(8):assert self.call(c,'a733_qualification_local',(c,EPOCH))==1
        # Model the production table builder's current identity descriptors.
        for i in range(512):
            self.put64(self.s['a733_cached_el3_l2']+i*8,(0x40000000+i*0x200000)|0x725)
            self.put64(self.s['a733_cached_el3_l3']+i*8,(0x42000000+i*4096)|(1<<54)|0x443)
        self.put64(self.s['a733_cached_el3_l2']+128,self.s['a733_cached_el3_l3']|3)
        with open(self.image.path,'rb') as f:
            elf=ELFFile(f)
            arrived=next(x['st_value'] for x in elf.get_section_by_name('.symtab').iter_symbols() if x.name=='arrived')
        contexts=[]
        for c in range(8):
            self.core=c;self.reg[c].update(scr_el3=0x531,sctlr_el3=0x1005,
                ttbr0_el3=self.s['a733_cached_el3_l1'],icc_sre_el3=15,icc_igrpen1_el3=0)
            self.set('tpidr_el1',c);self.set('sctlr_el1',0x30d01805);self.set('tcr_el1',0x80803519)
            self.set('icc_sre_el1',1);self.set('icc_sre_el2',9)
            self.put64(arrived+c*8,EPOCH)
            self.uc.mem_write(self.s['a733_cached_records']+c*128,struct.pack('<3Q',EPOCH,c,3))
            self.uc.mem_write(self.s['a733_smp_core_results']+c*128,struct.pack('<5Q',32,7,2,0,40 if c==0 else 1))
            r=0x03460000+c*0x20000
            self.put32(r+0x14,0);self.put32(r+0x10100,0x40000001)
            # NS priority0xe0 is Secure0xf0. Every timer now changes PPI30,
            # while its neighboring priority bytes must survive restoration.
            self.put32(r+0x1041c,(self.u32(r+0x1041c)&~0x00ff0000)|0x00f00000)
            self.put32(r+0x10200,0x40000001)
            if c:self.put32(0x07052008+c*0x1000,8)
            self.uc.reg_write(fault.REGS[0],c)
            self.uc.reg_write(UC_ARM64_REG_SP,self.s['a733_cpu_el1_stacks']+(c+1)*32768)
            self.uc.reg_write(UC_ARM64_REG_PC,self.s['a733_qualification_terminal_el0'])
            contexts.append(self.uc.context_save())
        return contexts
    def execute(self,contexts,missing=None,budget=12000000):
        initial=self.count
        while self.count-initial<budget:
            for c in range(8):
                if c==missing or c in self.parked:continue
                self.core=c;self.stop=None;self.uc.context_restore(contexts[c])
                self.uc.emu_start(self.uc.reg_read(UC_ARM64_REG_PC),0,count=256)
                self.apply_side_effects();contexts[c]=self.uc.context_save()
                if self.stop=='rom':return 'rom'
                if self.stop=='native-retained':self.parked.add(c)
            if len(self.parked)+(missing is not None)==8:return 'retained'
        raise AssertionError(('execution budget',self.count-initial,[hex(self.uc.context_restore(x) or self.uc.reg_read(UC_ARM64_REG_PC)) for x in contexts]))

def main():
    ap=argparse.ArgumentParser();ap.add_argument('elf',type=Path);ap.add_argument('--out',type=Path,required=True)
    a=ap.parse_args();a.out.mkdir(parents=True,exist_ok=True)
    image=fault.Image(a.elf)
    assert "a733_fault_boot" not in image.symbols, "resident stage2 rotation must not be linked"
    model=Model(image);contexts=model.initialize();outcome=model.execute(contexts)
    assert outcome=='rom',outcome
    assert not model.postclean,model.postclean[:4]
    expected=set(range(0x41200000,(image.symbols['__bss_end']+63)&~63,64))
    assert all(x==expected for x in model.cleaned),[len(x) for x in model.cleaned]
    clean_end=max(expected)+64
    assert all(a+size<=clean_end for _,a,size,_ in model.writes if 0x41200000<=a<0x42000000)
    raw=bytes(model.uc.mem_read(model.report,8880));(a.out/'report.bin').write_bytes(raw)
    assert model.u64(model.report+40)==4 and model.u64(model.report+48)==0
    for c in range(8):
        p=model.report+688+c*1024
        assert all(model.u64(p+i*8)==EPOCH for i in range(4))
        assert model.u64(p+40)==(EPOCH if c else 0)
        assert model.u64(p+768)==(EPOCH if c else 0)
        assert model.u32(0x03460000+c*0x20000+0x1041c)==0x50607080+c
        slot=fault.decode.decode_slot(bytes(model.uc.mem_read(fault.SLOTS+c*128,128)))
        assert slot['state']=='valid' and slot['kind']=='deliberate exit' and slot['epoch']==EPOCH
    rows=[dict(name='all-eight-success',instructions=model.count,cleaned_lines_per_core=len(expected),normal_writes_after_first_clean=0)]
    for name in ('ordinary-exit-zero','wrong-core','missing-peer','active-interrupt',
                 'unrelated-pending','wrong-ns-alias','expired-deadline',
                 'restore-readback','ack-without-off','missing-timer','duplicate-timer'):
        m=Model(image);ctx=m.initialize();missing=None
        m.put64(m.report+32,2500000 if name in ('restore-readback','ack-without-off') else 50000)
        if name=='ordinary-exit-zero':
            m.uc.context_restore(ctx[3]);m.uc.reg_write(UC_ARM64_REG_PC,image.symbols['a733_cached_smc'])
            for i in range(3):m.uc.reg_write(fault.REGS[i],0)
            ctx[3]=m.uc.context_save()
        elif name=='wrong-core':
            m.uc.context_restore(ctx[3]);m.uc.reg_write(fault.REGS[0],2);ctx[3]=m.uc.context_save()
        elif name=='missing-peer':missing=3
        elif name in ('missing-timer','duplicate-timer'):
            m.put64(image.symbols['a733_smp_core_results']+3*128+32,0 if name=='missing-timer' else 2)
        elif name=='active-interrupt':m.put32(0x03460000+3*0x20000+0x10300,1)
        elif name=='unrelated-pending':m.put32(0x03460000+3*0x20000+0x10200,0x40000003)
        elif name=='wrong-ns-alias':
            p=image.symbols['a733_cached_el3_l2']+9*8;m.put64(p,m.u64(p)&~32)
        elif name=='expired-deadline':m.put64(m.report+32,m.time)
        elif name=='restore-readback':m.corrupt_readback=True
        elif name=='ack-without-off':m.skip_off=True
        assert m.execute(ctx,missing=missing)=='retained',name
        assert m.u64(m.report+40)!=4 and not m.postclean,name
        if name not in ('restore-readback','ack-without-off'):
            assert not any(m.cleaned),name
        if name=='ack-without-off':
            assert m.u64(m.report+688+1024+40)==EPOCH
            assert m.u64(m.report+688+1024+768)==0
        rows.append(dict(name=name,instructions=m.count,success=False,
                         cleaned_lines=sum(map(len,m.cleaned)),normal_writes_after_first_clean=0))
    # Real initialized native records and an invalid interrupted SP exercise
    # the unchanged stackless fault path in this newly linked image.
    uc=image.machine();epoch=fault.prepare(image,uc,3)
    regs=fault.el3_model(3);regs.update(esr_el1=0x92000045,spsr_el1=0)
    result=fault.run(image,uc,image.symbols['a733_cached_el1_vectors']+8*128,
                     {UC_ARM64_REG_SP:0x40000003},regs,['a733_smp_stop'])
    slot=fault.decode.decode_slot(bytes(uc.mem_read(fault.SLOTS+3*128,128)))
    assert slot['state']=='valid' and slot['kind']=='vector fault' and slot['epoch']==epoch
    assert not [w for w in result['writes'] if w[4]==1]
    assert not [w for w in result['writes'] if model.report<=w[1]<model.report+8880]
    rows.append(dict(name='current-native-bad-stack-retains',instructions=len(result['trace']),el1_stores=0,success_publications=0))
    hashes={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted((ROOT/'apps/cpu-runtime-qualification').glob('*')) if p.is_file() and p.suffix in ('.c','.h','.S','.patch','.txt')}
    for p in (Path(__file__), ROOT/'checks/fault-capture.py', ROOT/'checks/fault-decode.py'):
        hashes[str(p.relative_to(ROOT))]=hashlib.sha256(p.read_bytes()).hexdigest()
    result=dict(schema=1,scope='modeled native shutdown; completed workload is an input fixture, not physical or scheduler qualification',epoch=EPOCH,elf_sha256=hashlib.sha256(a.elf.read_bytes()).hexdigest(),sources=hashes,resident_stage2_rotation_linked=False,manifest_sha256=hashlib.sha256((a.elf.parent/'qualification/manifest.json').read_bytes()).hexdigest(),cases=rows,report_sha256=hashlib.sha256(raw).hexdigest())
    (a.out/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
if __name__=='__main__':main()
