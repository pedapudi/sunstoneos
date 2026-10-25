#!/usr/bin/env python3
"""Build and execute the cached eight-core image with modeled architectural I/O.

The model has no A733 interconnect/cache implementation and proves no physical
coherence, interrupt delivery timing, watchdog recovery or board admission.

With --count, the scheduler run also counts, for each call of the kernel and
port functions in COUNTED, the instructions, SVC, DSB, DMB and ISB that the
calling core executes until the call returns, and the lock calls it makes.
Instructions of other tasks and of interrupts taken during the call are not
counted. The result gives the median of each count over all calls; sgi_sends
counts the calls of vInterruptCore and woken the entries of prvYieldForTask
(a task made ready that must run now) during the counted call. tick_by_wakes
separates the tick handler's calls that woke nothing, those taken while the
scheduler was suspended, which only pend the tick, and those that woke a
task, whose SGIs are required preemptions. irq_round_trip counts each IRQ
from its vector entry to its exception return on the taking core, grouped by
interrupt ID and by whether the exit path called vTaskSwitchContext; the tick
group also reports how many tick handler calls the IRQs made.

With --late-ticks N, once the kernel tick count reaches 16 and core 0 has no
IRQ in progress, the model counter jumps to N tick periods past the pending
tick deadline, so N + 1 periods are due when the next tick IRQ arrives;
late_catch_up then gives the tick IRQs, tick handler calls and instructions,
counted from that IRQ, that the kernel needs to account for them.
"""
import argparse
import ctypes
import hashlib
import json
import pickle
from pathlib import Path
import shutil
import statistics
import struct
import subprocess
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import *
ROOT=Path(__file__).resolve().parents[1]
GPR=[UC_ARM64_REG_X0+i for i in range(29)]+[UC_ARM64_REG_X29,UC_ARM64_REG_LR]
MASK=(1<<64)-1
COUNTED=('vTaskEnterCritical','vTaskExitCritical','xTaskGetCurrentTaskHandle','ucPortGetCoreID',
    'FreeRTOS_SGI_Handler','FreeRTOS_Tick_Handler','vTaskSwitchContext','vTaskEnterCriticalFromISR',
    'vTaskExitCriticalFromISR','vTaskDelay','xTaskGenericNotify','ulTaskGenericNotifyTake','vInterruptCore',
    'xTaskIncrementTick')
# TPIDRRO_EL0 is UNKNOWN at reset. The scheduler run seeds a value whose task
# field is nonzero and whose core field is 0xff; the entry code must clear it.
TPIDRRO_RESET=(0xff<<48)|0x41ff0000

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()

def build(e,freertos):
    e.mkdir(parents=True,exist_ok=False)
    source=e/'source';source.mkdir()
    paths=[*ROOT.glob('platform/a733-smp-cached/*'),*ROOT.glob('include/*.h'),
        *ROOT.glob('runtime/include/*.h'),ROOT/'runtime/memory.c',ROOT/'runtime/memmove.c',
        ROOT/'checks/fp-context.S',ROOT/'checks/a733-smp-cached.py',
        ROOT/'cmake/a733-smp-cached.cmake',ROOT/'cmake/aarch64-clang.cmake',
        *ROOT.glob('patches/freertos-smp-*.patch'),ROOT/'patches/freertos-a733-smp.patch']
    paths += [ROOT/'platform/a733'/n for n in ['cpu_start.c','cpu_entry.c','cpu_entry.S','cpu_entry_native.c',
        'cpu_continue.c','cpu_coherence.c','cpu_coherence.S']]
    for p in paths:
        d=source/p.relative_to(ROOT);d.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,d)
    dep=e/'freertos';dep.mkdir()
    for n in ['tasks.c','queue.c','list.c']:shutil.copy2(freertos/n,dep/n)
    shutil.copytree(freertos/'include',dep/'include')
    shutil.copytree(freertos/'portable/GCC/ARM_CR82',dep/'portable/GCC/ARM_CR82')
    compiler=Path(shutil.which('clang-21')).resolve()
    linker=Path.home()/'.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld'
    commands=[]
    def run(cmd,name):
        cmd=list(map(str,cmd));commands.append(cmd)
        p=subprocess.run(cmd,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
        (e/(name+'.log')).write_text(p.stdout)
        (e/'commands.json').write_text(json.dumps(commands,indent=2)+'\n');p.check_returncode()
    toolchain=e/'toolchain.cmake'
    toolchain.write_text(f'set(SUNSTONE_LLD "{linker}" CACHE FILEPATH "")\n'
        f'include("{source}/cmake/aarch64-clang.cmake")\nset(CMAKE_C_COMPILER "{compiler}")\nset(CMAKE_ASM_COMPILER "{compiler}")\n')
    b=e/'build'
    run(['cmake','-G','Unix Makefiles','-S',source/'platform/a733-smp-cached','-B',b,
        f'-DCMAKE_TOOLCHAIN_FILE={toolchain}',f'-DFREERTOS_DIR={dep}',f'-DSUNSTONE_LLD={linker}'],'configure')
    run(['cmake','--build',b,'--target','a733-smp-cached-diagnostic'],'build')
    (e/'build-inputs.json').write_text(json.dumps({str(p.relative_to(e)):sha(p)
        for p in e.rglob('*') if p.is_file()},indent=2)+'\n')
    return b/'a733-smp-cached-diagnostic.elf'


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--evidence',type=Path,required=True)
    p.add_argument('--freertos',type=Path)
    p.add_argument('--build-only',action='store_true')
    p.add_argument('--stop-clock',type=int)
    p.add_argument('--resume',type=Path)
    p.add_argument('--transition-only',action='store_true')
    p.add_argument('--cold-vectors-only',action='store_true')
    p.add_argument('--image',type=Path,help='Reuse preserved ELF without rebuilding')
    p.add_argument('--count',action='store_true',help='Count instructions per kernel and port call')
    p.add_argument('--late-ticks',type=int,default=0,help='Make one tick IRQ this many tick periods late')
    a=p.parse_args();e=a.evidence.resolve()
    if a.image:
        e.mkdir(parents=True,exist_ok=False);image=a.image.resolve()
        shutil.copy2(__file__,e/'executed-check.py')
    else:
        assert a.freertos;image=build(e,a.freertos)
    if a.build_only:print(f'BUILT {image}');return
    try:result=cold_vector_check(image) if a.cold_vectors_only else transition_check(image) if a.transition_only else execute(image,e,a.stop_clock,a.resume,a.count,a.late_ticks)
    except Exception as error:
        (e/'failure.txt').write_text(repr(error)+'\n');raise
    (e/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    (e/'manifest.json').write_text(json.dumps({'image':str(image),'image_sha256':sha(image),
        'check_sha256':sha(e/'executed-check.py' if a.image else e/'source/checks/a733-smp-cached.py'),
        'resume':{'path':str(a.resume),'sha256':sha(a.resume)} if a.resume else None,'files':{str(f.relative_to(e)):sha(f)
        for f in e.rglob('*') if f.is_file()}},indent=2)+'\n')
    print('PASS '+('stackless cold/late-failure vectors' if a.cold_vectors_only else 'cached transition and retained vectors' if a.transition_only else
        'saved diagnostic checkpoint' if a.stop_clock else 'eight-core scheduler and missing-secondary refusal'))

# Execution model is defined below; build-only preserves the exact candidate
# before any instruction run or subsequent source correction.


class Machine:
    def __init__(self,image,missing=None,count=False):
        with image.open('rb') as f:
            elf=ELFFile(f)
            self.symbols={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
            segments=[(s['p_vaddr'],s['p_memsz'],s['p_flags'],s.data()) for s in elf.iter_segments() if s['p_type']=='PT_LOAD']
        assert self.symbols['a733_smp_entry']==0x41000000
        for addr,size,flags,data in segments:
            assert 0x41000000<=addr and addr+size<=0x42200000 and flags&3!=3
        self.ram=ctypes.create_string_buffer(0x1200000)
        self.ram_pointer=ctypes.addressof(self.ram)
        for addr,size,flags,data in segments:ctypes.memmove(self.ram_pointer+addr-0x41000000,data,len(data))
        self.clock=24;self.steps=0;self.missing=missing
        self.cpus=[];self.regs=[];self.active=[False]*8;self.active[0]=True
        self.pending=[set() for _ in range(8)];self.irq_active=[None]*8
        self.versions={};self.exclusive=[None]*8
        self.decoded={};self.decoder=Cs(CS_ARCH_ARM64,CS_MODE_ARM)
        self.counting=count;self.calls=[[] for _ in range(8)];self.exceptions=[[] for _ in range(8)]
        self.irq_samples=[[] for _ in range(8)];self.irq_done=[];self.late_ticks=0;self.late_applied=None
        self.samples={name:[] for name in COUNTED}
        self.entries={self.symbols[n]:n for n in COUNTED if n in self.symbols}
        self.trace=[];self.sticky_sif=None;self.sif_cleared=set();self.el3_enabled=set();self.el1_enabled=set();self.first_task=set()
        # Zeroed active-priority copies per core, and those zeroed when the
        # core first enabled Nonsecure Group 1 at EL1.
        self.active_priorities=[set() for _ in range(8)];self.group1_priorities={}
        self.mmio={0x02050014:0x41,0x02050010:1,0x02050018:2,0x07090108:0x5aa5a55a,
            0x070501c0:0xfa50392f,0x070501c4:0x20,0x070501d4:0,0x03000024:0,
            0x03400000:0,0x03400004:1}
        self.rdist=[0x03460000+c*0x20000 for c in range(8)]
        for c,r in enumerate(self.rdist):
            self.mmio[r+8]=(c<<40)|(16 if c==7 else 0)
            for o in [0,0x14,0x24,0x10300,0x10080,0x10d00,0x10c04,0x10400,0x10100]:self.mmio[r+o]=0
            self.mmio[r+0x14]=6
            self.mmio[0x07050200+c*4]=2 if c else 0
            self.mmio[0x07050220+c*4]=0
            self.mmio[0x07052008+c*0x1000]=0
            for o in [0,4,8]:self.mmio[0x08001000+c*0x1000+o]=0
        self.clusteractlr=0
        for c in range(8):
            u=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
            u.mem_map_ptr(0x41000000,0x1200000,7,self.ram_pointer)
            u.mem_map(0x02000000,0x07000000)
            u.reg_write(UC_ARM64_REG_CPACR_EL1,3<<20)
            m={'currentel':12,'spsel':1,'daif':0x3c0,'sctlr_el3':0x30c50830,'sctlr_el1':0x30d00800,
                'tcr_el1':0,'scr_el3':0x200,'midr_el1':0x412fd050 if c<6 else 0x413fd0b0,
                'mpidr_el1':0x81000000|(c<<8),'clidr_el1':0x124,'pmcr_el0':0x41003000,'id_aa64mmfr0_el1':2,
                's3_0_c15_c3_1':0x30,'s3_0_c15_c3_0':0,'cntfrq_el0':24000000,'ctr_el0':4<<16,
                'sp_el0':0,'sp_el1':0,'sp_el3':0,'icc_pmr_el1':0,'icc_igrpen1_el1':0,'tpidrro_el0':0,
                'cntp_ctl_el0':0,'cntp_cval_el0':MASK,'vbar_el3':0,'vbar_el1':0,
                's3_0_c15_c1_0':0,'s3_0_c15_c1_1':0,'s3_0_c15_c1_2':0,'s3_0_c15_c1_4':0,
                'esr_el1':0,'far_el1':0,'esr_el3':0,'far_el3':0,'elr_el3':0}
            self.cpus.append(u);self.regs.append(m)
            u.hook_add(UC_HOOK_CODE,self.instruction,c)
            u.hook_add(UC_HOOK_MEM_READ,self.memory,c)
            u.hook_add(UC_HOOK_MEM_WRITE,self.memory,c)
        self.cpus[0].reg_write(UC_ARM64_REG_PC,self.symbols['a733_smp_entry'])
        build_id=self.read(self.symbols['a733_smp_build_id'],32)
        values=[0x4153373343414331,1,224,5,7,MASK-7]
        contract=struct.pack('<6Q',*values)+build_id+struct.pack('<18Q',
            0,24000000,72000000,96000000,9,0x41,1,2,0x5aa5a55a,0xfa50392f,0x20,0,*([0]*6))
        assert len(contract)==224 # Six header words + build identity + eighteen words.
        contract=contract[:16]+struct.pack('<Q',len(contract))+contract[24:]
        ctypes.memmove(self.ram_pointer+0x1004000,contract,len(contract))
    def read(self,a,n):return ctypes.string_at(self.ram_pointer+a-0x41000000,n)
    def active_priority(self,c,name,value):
        """Record a zeroed ICC_AP0R0_EL1 or ICC_AP1R0_EL1. At EL3 SCR_EL3.NS
        selects the ICC_AP1R0_EL1 copy; below EL3 the Nonsecure copy is
        accessed. With SCR_EL3.FIQ set, Group 0 registers trap below EL3."""
        m=self.regs[c];assert value==0,(c,name,value)
        if name=='icc_ap0r0_el1':
            assert m['currentel']==12 or not m['scr_el3']&4,('Group 0 access traps to EL3',c)
            self.active_priorities[c].add('ap0r0')
        else:self.active_priorities[c].add('ap1r0_ns' if m['currentel']<12 or m['scr_el3']&1 else 'ap1r0_s')
    def q(self,a):return struct.unpack('<Q',self.read(a,8))[0]
    def reg(self,c,s):
        if s in ('xzr','wzr'):return 0
        if s.startswith('#'):return int(s[1:],0)
        if s=='sp':return self.cpus[c].reg_read(UC_ARM64_REG_SP)
        return self.cpus[c].reg_read(GPR[int(s[1:])])&((1<<32)-1 if s[0]=='w' else MASK)
    def put(self,c,s,v):
        if s in ('xzr','wzr'):return
        self.cpus[c].reg_write(GPR[int(s[1:])],v&((1<<32)-1 if s[0]=='w' else MASK))
    def spname(self,c):
        m=self.regs[c];return 'sp_el0' if m['spsel']==0 else f"sp_el{m['currentel']//4}"
    def save_sp(self,c):self.regs[c][self.spname(c)]=self.cpus[c].reg_read(UC_ARM64_REG_SP)
    def exception(self,c,level,pc,esr=0,irq=False):
        u=self.cpus[c];m=self.regs[c];old=m['currentel']//4
        self.save_sp(c)
        m[f'elr_el{level}']=pc
        m[f'spsr_el{level}']=u.reg_read(UC_ARM64_REG_NZCV)|m['daif']|(old*4+(m['spsel'] if old else 0))
        m[f'esr_el{level}']=esr
        self.exceptions[c].append(irq)
        if irq and self.counting:
            self.irq_samples[c].append({'depth':len(self.exceptions[c]),'intid':None,'switched':0,
                'counts':dict.fromkeys(('instructions','svc','dsb','dmb','isb','tick_handlers'),0)})
        offset=(0x200 if m['spsel'] else 0) if old==level else 0x400
        if irq:offset+=0x80
        m['currentel']=level*4;m['spsel']=1;m['daif']=0x3c0
        u.reg_write(UC_ARM64_REG_SP,m[f'sp_el{level}'])
        u.reg_write(UC_ARM64_REG_PC,m[f'vbar_el{level}']+offset)
    def timer_level(self,c):
        # Core 0's timer PPI 30 is level-sensitive: it is pending while the
        # timer condition holds, and a compare or control write that ends the
        # condition also ends the pending state.
        m=self.regs[c]
        if m['cntp_ctl_el0']&3==1 and self.clock>=m['cntp_cval_el0']:self.pending[c].add(30)
        else:self.pending[c].discard(30)
    def changed(self,a,size):
        for line in range(a//64,(a+size-1)//64+1):self.versions[line]=self.versions.get(line,0)+1
    def memory(self,u,access,a,size,value,c):
        from unicorn import UC_MEM_WRITE
        write=access==UC_MEM_WRITE;m=self.regs[c]
        if a<0x41000000:
            if write:
                if 0x07050204<=a<=0x0705021c and not (a&3):
                    target=(a-0x07050200)//4
                    if value&1:
                        self.mmio[0x07052008+target*0x1000]=8
                        if target!=self.missing and not self.active[target]:
                            self.active[target]=True
                            self.cpus[target].reg_write(UC_ARM64_REG_PC,self.symbols['a733_cpu_secondary_entry'])
                for r in self.rdist:
                    if a==r+0x14:value &= ~4
                    if a==r+0x10180:self.mmio[r+0x10100]=self.mmio[r+0x10100]&~value
                    if a==r+0x10100:value |= self.mmio[a]
                self.mmio[a]=value
            else:
                assert a in self.mmio,('unknown MMIO read',c,hex(a),size)
                u.mem_write(a,int(self.mmio[a]).to_bytes(size,'little'))
            return
        assert 0x41000000<=a and a+size<=0x42200000
        if 0x421f0000<=a<0x42200000:
            # EL1 table check: Device reads of the EL1 pages and checksum line, MMU off.
            el1_check=(not write and m['currentel']==4 and not m['sctlr_el1']&1 and
                any(s<=a<s+4096 for s in (self.symbols['a733_cached_el1_l1'],self.symbols['a733_cached_el1_l2'])) or
                (not write and m['currentel']==4 and not m['sctlr_el1']&1 and
                 self.symbols['a733_cached_el1_table_sum']<=a<self.symbols['a733_cached_el1_table_sum']+64))
            assert not self.el3_enabled or el1_check,('CPU table access after first walk',c,hex(a),write)
        if 0x42000000<=a<0x421f0000:
            assert m['currentel']==12,('EL1 touched secure monitor',c,hex(a))
            assert a%size==0,('unaligned Device access',c,hex(a),size)
        if 0x41200000<=a<0x42000000 and self.el3_enabled:
            assert len(self.el3_enabled)==8,('ordinary RAM before every EL3 adoption',c,hex(a),write)
            assert m['currentel']!=4 or c in self.el1_enabled,('EL1 cold access',c,hex(a))
        if write:
            assert a>=0x41200000,('code/rodata mutation',c,hex(a))
            self.changed(a,size)
    def instruction(self,u,a,size,c):
        self.steps+=1;m=self.regs[c]
        # Deliver at an architectural instruction boundary, not only the model's
        # scheduling quantum. A periodic task can otherwise alias every quantum
        # with a masked SVC and spuriously starve an already pending interrupt.
        if m['currentel']<=4 and not m['daif']&0x80 and m['icc_igrpen1_el1']&1 and \
           m['icc_pmr_el1']>0xe0 and self.pending[c] and self.irq_active[c] is None:
            self.exception(c,1,a,irq=True);return
        if a not in self.decoded:self.decoded[a]=next(self.decoder.disasm(bytes(u.mem_read(a,4)),a))
        ins=self.decoded[a];op=ins.mnemonic;args=ins.op_str.split(', ')
        if self.counting:self.count(u,a,c,op)
        skip=False
        if op=='mrs':
            dest,name=args
            if name in ('fpcr','fpsr','nzcv'):return
            if name=='cntpct_el0':v=self.clock
            elif name=='s3_0_c15_c3_3':v=self.clusteractlr
            # Secure priority0xf0 is exposed as0xe0 through the NS CPU interface.
            elif name=='icc_rpr_el1':v=0xe0 if self.irq_active[c] is not None else 0xff
            elif name=='icc_iar1_el1':
                choices=self.pending[c]
                v=min(choices) if choices else 1023
                if v<1020:
                    choices.remove(v);self.irq_active[c]=v
                    if self.irq_samples[c]:self.irq_samples[c][-1]['intid']=v
            elif name=='cntp_ctl_el0':
                # ISTATUS (bit 2) reports the timer condition.
                v=m[name]&~4|(4 if m[name]&1 and self.clock>=m['cntp_cval_el0'] else 0)
            else:
                assert name in m,('unknown sysreg',c,name,hex(a));v=m[name]
            self.put(c,dest,v);skip=True
        elif op=='msr':
            name,src=args;v=self.reg(c,src)
            if name in ('fpcr','fpsr','nzcv'):return
            if name=='daifset':m['daif']|=v<<6
            elif name=='daifclr':m['daif']&=~(v<<6)
            elif name=='spsel':
                self.save_sp(c);m['spsel']=v;u.reg_write(UC_ARM64_REG_SP,m[self.spname(c)])
            elif name=='s3_0_c15_c3_3':
                assert c==0 and not any(self.active[1:]);self.clusteractlr=v
            elif name in ('icc_ap0r0_el1','icc_ap1r0_el1'):self.active_priority(c,name,v)
            else:
                m[name]=v|0x200 if name=='scr_el3' and c==self.sticky_sif else v
            if name=='scr_el3' and not m[name]&0x200:self.sif_cleared.add(c)
            if name=='sctlr_el3' and v&0x1005==0x1005:
                assert not m['scr_el3']&0x200,('SIF prevents NS code fetch',c)
                self.mappings()
                self.el3_enabled.add(c);self.trace.append(('EL3 cached',c,self.clock))
                assert m['sctlr_el1']&1 and m['tcr_el1']&0x800080==0x800080
            if name=='sctlr_el1' and v&0x1005==0x1005:
                assert len(self.el3_enabled)==8
                self.el1_enabled.add(c);self.trace.append(('EL1 cached',c,self.clock))
            if name=='icc_igrpen1_el1' and v&1 and m['currentel']<12:
                self.group1_priorities.setdefault(c,sorted(self.active_priorities[c]))
            if name=='icc_sgi1r_el1':
                target=(v>>16)&255;assert target<8 and v&0xffff==1
                self.pending[target].add(0)
            if name=='icc_eoir1_el1':assert self.irq_active[c]==v;self.irq_active[c]=None
            if name in ('cntp_cval_el0','cntp_ctl_el0') and not c:self.timer_level(c)
            skip=True
        elif op in ('isb','dsb','dmb','tlbi','dc','ic','sev','sevl','yield'):skip=True
        elif op in ('wfe','wfi'):
            u.reg_write(UC_ARM64_REG_PC,a+4);u.emu_stop();return
        elif op in ('ldaxr','ldxr'):
            dest,base=args;address=self.reg(c,base.strip('[]'));n=8 if dest[0]=='x' else 4
            self.memory(u,0,address,n,0,c)
            self.put(c,dest,int.from_bytes(self.read(address,n),'little'))
            self.exclusive[c]=(address,n,self.versions.get(address//64,0));skip=True
        elif op in ('stlxr','stxr'):
            result,src,base=args;address=self.reg(c,base.strip('[]'));n=8 if src[0]=='x' else 4
            valid=self.exclusive[c]==(address,n,self.versions.get(address//64,0))
            self.put(c,result,0 if valid else 1)
            if valid:
                from unicorn import UC_MEM_WRITE
                self.memory(u,UC_MEM_WRITE,address,n,self.reg(c,src),c)
                u.mem_write(address,self.reg(c,src).to_bytes(n,'little'))
            self.exclusive[c]=None;skip=True
        elif op=='clrex':self.exclusive[c]=None;skip=True
        elif op=='svc':
            self.exception(c,1,a+4,(0x15<<26)|int(args[0][1:],0));return
        elif op=='smc':
            self.exception(c,3,a+4,0x17<<26);return
        elif op=='eret':
            level=m['currentel']//4;target=m[f'elr_el{level}'];spsr=m[f'spsr_el{level}']
            if self.exceptions[c]:self.exceptions[c].pop()
            self.save_sp(c);mode=spsr&15;m['currentel']=(mode>>2)*4;m['spsel']=mode&1
            m['daif']=spsr&0x3c0;u.reg_write(UC_ARM64_REG_NZCV,spsr&0xf0000000)
            u.reg_write(UC_ARM64_REG_SP,m[self.spname(c)]);u.reg_write(UC_ARM64_REG_PC,target)
            if not m['currentel']:self.first_task.add(c)
            return
        if skip:u.reg_write(UC_ARM64_REG_PC,a+4)
    def count(self,u,a,c,op):
        sp=u.reg_read(UC_ARM64_REG_SP);irqs=sum(self.exceptions[c])
        task=self.q(self.symbols['pxCurrentTCBs']+8*c)
        calls=self.calls[c]
        while calls and a==calls[-1]['return'] and sp==calls[-1]['sp'] and irqs==calls[-1]['irqs']:
            done=calls.pop();self.samples[done['name']].append(done['counts'])
        if a in self.entries:
            calls.append({'name':self.entries[a],'return':u.reg_read(UC_ARM64_REG_LR),'sp':sp,'irqs':irqs,
                'task':task,'counts':dict.fromkeys(('instructions','svc','dsb','dmb','isb','lock_calls','sgi_sends','woken'),0)})
            # A tick taken while the scheduler is suspended only pends the tick.
            calls[-1]['counts']['suspended']=int(self.q(self.symbols['uxSchedulerSuspended'])!=0)
        for call in calls:
            if call['irqs']!=irqs or call['task']!=task:continue
            counts=call['counts'];counts['instructions']+=1
            if op in counts:counts[op]+=1
            if a==self.symbols['vPortRecursiveLock']:counts['lock_calls']+=1
            if a==self.symbols['vInterruptCore']:counts['sgi_sends']+=1
            if a==self.symbols['prvYieldForTask']:counts['woken']+=1
        samples=self.irq_samples[c]
        if samples and samples[-1]['depth']==len(self.exceptions[c]):
            sample=samples[-1];counts=sample['counts'];counts['instructions']+=1
            if op in counts:counts[op]+=1
            if a==self.symbols['vTaskSwitchContext']:sample['switched']=1
            if a==self.symbols['FreeRTOS_Tick_Handler']:counts['tick_handlers']+=1
            if op=='eret':self.irq_done.append(samples.pop())
    def counts(self):
        result={name:{'calls':len(samples),**{key:statistics.median_low(s[key] for s in samples)
            for key in samples[0]}} for name,samples in self.samples.items() if samples}
        ticks=self.samples['FreeRTOS_Tick_Handler']
        quiet=[s for s in ticks if not s['woken'] and not s['suspended']];woke=[s for s in ticks if s['woken']]
        median=lambda rows,key:statistics.median_low(s[key] for s in rows) if rows else None
        result['tick_by_wakes']={
            'woke_nothing':{'ticks':len(quiet),'max_instructions':max((s['instructions'] for s in quiet),default=None),
                **{key:median(quiet,key) for key in ('instructions','svc','dsb','isb','sgi_sends')}},
            'pended':{'ticks':sum(s['suspended'] for s in ticks),'instructions':median([s for s in ticks if s['suspended']],'instructions')},
            'woke_tasks':{'ticks':len(woke),'sgi_sends_total':sum(s['sgi_sends'] for s in woke),
                **{key:median(woke,key) for key in ('instructions','woken','sgi_sends')}}}
        groups={}
        for sample in self.irq_done:
            name=('sgi' if sample['intid']==0 else 'tick' if sample['intid']==30 else f"intid_{sample['intid']}")
            groups.setdefault(name+('_switched' if sample['switched'] else '_returned'),[]).append(sample['counts'])
        result['irq_round_trip']={name:{'irqs':len(rows),**{key:median(rows,key) for key in
            ('instructions','svc','dsb','dmb','isb')},**({'tick_handlers':sum(r['tick_handlers'] for r in rows)}
            if name.startswith('tick') else {})} for name,rows in sorted(groups.items())}
        ticks=[s for s in self.irq_done if s['intid']==30]
        result['tick_irqs']={'irqs':len(ticks),'tick_handler_calls':sum(s['counts']['tick_handlers'] for s in ticks),
            'late_ticks_injected':self.late_ticks if self.late_applied is not None else 0}
        if self.late_applied is not None:
            irqs=calls=instructions=0
            for sample in self.irq_done[self.late_applied:]:
                if sample['intid']!=30:continue
                irqs+=1;calls+=sample['counts']['tick_handlers'];instructions+=sample['counts']['instructions']
                if calls>=self.late_ticks+1:break
            result['late_catch_up']={'periods':self.late_ticks+1,'irqs':irqs,'tick_handler_calls':calls,
                'instructions':instructions}
        return result
    def mappings(self):
        names=['a733_cached_el3_l1','a733_cached_el3_l2','a733_cached_el3_l3',
               'a733_cached_el1_l1','a733_cached_el1_l2']
        pages=[self.symbols[n] for n in names]
        assert len(set(pages))==5 and all(0x421f0000<=a<0x42200000 and not a&4095 for a in pages)
        table_pages=set(pages)
        if 'a733_supervised_publish_l1' in self.symbols:
            temporary=self.symbols['a733_supervised_publish_l1']
            table_pages.update((temporary,temporary+4096))
        def walk(root,va):
            desc=self.q(root+((va>>30)&511)*8)
            for shift in (21,12):
                if desc&3!=3:break
                desc=self.q((desc&0x0000fffffffff000)+((va>>shift)&511)*8)
            return desc
        for va in range(0x42000000,0x42200000,4096):
            d=walk(pages[0],va);assert d&3==3 and d&(1<<54) and d&(1<<10) and d&(1<<6)
            assert (d>>2)&7==(2 if va in table_pages else 0)
            assert bool(d&(1<<5))==(va in pages[3:])
            assert walk(pages[3],va)==0
        for va in (0x40000000,0x41000000,0x41200000,0x41fff000,0x42200000,0x47fff000):
            a=walk(pages[0],va);b=walk(pages[3],va)
            assert (a>>2)&7==(b>>2)&7==1
            assert (a>>8)&3==(b>>8)&3==3
            assert a&(1<<5) and a&(1<<10) and b&(1<<10)
            if va==0x41000000:assert not a&(1<<54) and not b&((1<<53)|(1<<54))
            else:assert a&(1<<54) and b&((1<<53)|(1<<54))==((1<<53)|(1<<54))
        assert not walk(pages[0],0x03400000)&(1<<5)
    def save(self,path):
        state={k:v for k,v in self.__dict__.items() if k not in
            ('cpus','ram','ram_pointer','decoder','decoded')}
        state['ram_bytes']=bytes(self.ram)
        state['contexts']=[u.context_save() for u in self.cpus]
        path.write_bytes(pickle.dumps(state))
    def restore(self,path):
        state=pickle.loads(path.read_bytes())
        ctypes.memmove(self.ram_pointer,state.pop('ram_bytes'),0x1200000)
        contexts=state.pop('contexts');self.__dict__.update(state)
        for u,c in zip(self.cpus,contexts):u.context_restore(c)
    def summary(self):
        return {'report':struct.unpack('<9Q',self.read(0x42000000,72)),
            'steps':self.steps,'clock':self.clock,'trace':self.trace,'sif_cleared':sorted(self.sif_cleared),
            'group1_active_priorities':{c:v for c,v in sorted(self.group1_priorities.items())},
            'EL3':sorted(self.el3_enabled),'EL1':sorted(self.el1_enabled),'task_cores':sorted(self.first_task),
            'pcs':[hex(u.reg_read(UC_ARM64_REG_PC)) for u in self.cpus],
            'registers':self.regs,'pending':[list(p) for p in self.pending],'active_irq':self.irq_active,
            'core_records':[struct.unpack('<16Q',self.read(self.symbols['a733_smp_core_results']+c*128,128)) for c in range(8)]}
    def run(self,stop_clock=None,transition=False):
        for quantum in range(100000):
            # Rotate the first runnable CPU and use coprime quantum lengths.
            # Identical task loops must not obtain a permanent scheduling phase
            # advantage over peers at a non-fair hardware spinlock.
            for index in range(8):
                c=(index+quantum)%8;u=self.cpus[c]
                if not self.active[c]:continue
                m=self.regs[c]
                if not c:self.timer_level(c)
                if m['currentel']<=4 and not m['daif']&0x80 and m['icc_igrpen1_el1']&1 and \
                   m['icc_pmr_el1']>0xe0 and self.pending[c] and self.irq_active[c] is None:
                    self.exception(c,1,u.reg_read(UC_ARM64_REG_PC),irq=True)
                try:u.emu_start(u.reg_read(UC_ARM64_REG_PC),0x42200000,count=(211,347,509,673,887)[(quantum+c)%5])
                except Exception as ex:
                    raise RuntimeError((c,hex(u.reg_read(UC_ARM64_REG_PC)),self.clock,self.steps,str(ex))) from ex
                if self.q(0x42000000+24)==5:
                    return self.summary()
            if transition and (len(self.el1_enabled)==8 or any(
                self.q(self.symbols['a733_cached_records']+c*128+16)==6 for c in range(8))):
                return self.summary()
            if stop_clock is not None and self.clock>=stop_clock:return self.summary()
            self.clock+=1000
            if self.late_ticks and self.late_applied is None and self.irq_active[0] is None and \
               not self.irq_samples[0] and struct.unpack('<I',self.read(self.symbols['xTickCount'],4))[0]>=16:
                self.clock=max(self.clock,self.regs[0]['cntp_cval_el0']+self.late_ticks*24000)
                self.late_applied=len(self.irq_done)
            if self.missing is not None and self.mmio[0x07052008+self.missing*0x1000]==8:
                self.clock=max(self.clock,24000001)
        raise AssertionError(('instruction budget',self.steps,[hex(u.reg_read(UC_ARM64_REG_PC)) for u in self.cpus]))


def execute(image,e,stop_clock=None,resume=None,count=False,late_ticks=0):
    machine=Machine(image,count=count);machine.late_ticks=late_ticks
    for r in machine.regs:r['tpidrro_el0']=TPIDRRO_RESET
    if resume:machine.restore(resume)
    success=machine.run(stop_clock);machine.save(e/'machine.pkl')
    (e/'state.json').write_text(json.dumps(success,indent=2)+'\n')
    if stop_clock:return {'diagnostic_checkpoint':True,'state':success}
    assert success['report'][4]==0,success
    assert success['EL3']==success['EL1']==success['task_cores']==list(range(8)),success
    assert success['report'][7]==255
    # Every core zeroed Group 0 and both Group 1 active-priority copies
    # before it enabled Group 1 (Linux gic_cpu_sys_reg_init).
    assert success['group1_active_priorities']=={c:['ap0r0','ap1r0_ns','ap1r0_s'] for c in range(8)},success['group1_active_priorities']
    for c,r in enumerate(success['core_records']):
        assert r[0]==32 and r[1]>0 and r[2]>0 and r[3]==0,(c,r)
    failed=Machine(image,missing=7).run()
    assert failed['report'][4]!=0 and failed['EL3']==failed['EL1']==[] and not failed['task_cores'],failed
    return {'passed':True,'success':success,'missing_secondary':failed,
        **({'counts':machine.counts()} if count else {}),
        'limits':'Actual image instructions and FreeRTOS tasks; modeled registers, GIC, timer, exclusive monitors and shared RAM. No A733 cache/interconnect/physical timing model or board execution.'}


def transition_check(image):
    normal=Machine(image);good=normal.run(transition=True)
    assert good['sif_cleared']==good['EL3']==good['EL1']==list(range(8)),good
    sticky=Machine(image);sticky.sticky_sif=6;bad=sticky.run(transition=True)
    assert 6 not in bad['EL3'] and not bad['EL1'],bad
    assert sticky.q(sticky.symbols['a733_cached_records']+6*128+16)==6
    assert sticky.q(sticky.symbols['a733_cached_records']+6*128+24)==8
    vectors=[]
    for state in (3,4,6):
        m=Machine(image);c=6;u=m.cpus[c];r=m.regs[c]
        address=m.symbols['a733_cached_records']+c*128
        record=struct.pack('<16Q',5,c,state,*([0]*13))
        u.mem_write(address,record)
        r['sctlr_el1']=0x30d00800;r['tcr_el1']=0
        r['esr_el3']=0x17<<26;r['far_el3']=0xabc;r['elr_el3']=0x41001234
        u.reg_write(GPR[0],4);u.reg_write(GPR[1],0x123)
        u.reg_write(UC_ARM64_REG_PC,m.symbols['a733_cached_el3_vectors']+8*128)
        u.emu_start(u.reg_read(UC_ARM64_REG_PC),0x42200000,count=1000)
        after=m.read(address,128)
        assert r['sctlr_el1']&1 and r['tcr_el1']&0x800080==0x800080
        if state==3:
            assert m.q(address+16)==6 and m.q(address+24)==0x123 and m.q(address+40)==0x17<<26
        else:assert after==record
        assert all(m.read(m.symbols['a733_cached_records']+other*128,128)==bytes(128) for other in range(8) if other!=c)
        vectors.append({'initial_state':state,'terminal_state':m.q(address+16)})
    return {'passed':True,'normal_transition':good,'sticky_sif_refusal':bad,'terminal_vectors':vectors,
        'limits':'New cache-adoption/SIF and retained-vector paths only; unchanged scheduler evidence is reused.'}


def cold_vector_check(image):
    cases=[]
    for enabled in (False,True):
        m=Machine(image);c=6;u=m.cpus[c];r=m.regs[c]
        address=m.symbols['a733_cached_records']+c*128
        u.mem_write(address,struct.pack('<16Q',5,c,3,*([0]*13)))
        r['currentel']=4;r['spsel']=1;r['sctlr_el1']=0x30d00800|(0x1005 if enabled else 0)
        r['vbar_el3']=m.symbols['a733_cached_el3_vectors']
        r['sp_el3']=m.symbols['a733_cpu_entry_stacks']+(c+1)*4096
        r['esr_el1']=0x96000011;u.reg_write(UC_ARM64_REG_SP,0)
        if enabled:m.el3_enabled=set(range(8));m.el1_enabled.add(c)
        branches=[]
        def count(cpu,pc,size,unused):
            ins=next(m.decoder.disasm(bytes(cpu.mem_read(pc,4)),pc))
            if ins.mnemonic=='subs' and ins.op_str=='x16, x16, #1':branches.append(pc)
        u.hook_add(UC_HOOK_CODE,count)
        u.emu_start(m.symbols['a733_cached_cold_vectors']+0x400,0x42200000,count=1000)
        assert m.q(address+16)==6 and m.q(address+24)==0x96000011
        assert len(branches)==48 # Twenty-four iterations at EL1 and at EL3.
        assert r['sctlr_el1']&1 and r['tcr_el1']&0x800080==0x800080
        assert all(m.read(m.symbols['a733_cached_records']+other*128,128)==bytes(128) for other in range(8) if other!=c)
        cases.append({'EL1_cache_active':enabled,'BHB_iterations':len(branches),'terminal_state':6})
    return {'passed':True,'cases':cases,'limits':'Stackless exception paths with modeled system registers; no physical execution.'}

if __name__=='__main__':main()
