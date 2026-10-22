#!/usr/bin/env python3
"""Check cold CPU controls and cache entry; no SMP-image or physical execution."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import subprocess
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X29, UC_ARM64_REG_LR, UC_ARM64_REG_SP, UC_ARM64_REG_PC
ROOT = Path(__file__).resolve().parents[1]
SOURCES = ['include/a733_cpu_coherence.h', 'platform/a733/cpu_coherence.c', 'platform/a733/cpu_coherence.S',
           'checks/a733-cpu-coherence.c', 'checks/a733-cpu-coherence.py']
TF = Path.home() / '.cache/sunstoneos/sources/cpu-continue'
REF = Path.home() / '.cache/sunstoneos/sources/cpu-coherence'
REV = '0892ad4db8f8f4c95775cc7d0505b6c12dda1f9e'
GPR = [UC_ARM64_REG_X0+i for i in range(29)] + [UC_ARM64_REG_X29, UC_ARM64_REG_LR]
MASK = (1 << 64) - 1

def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module
TCR = load('a733_tcr', ROOT / 'tools/a733_tcr.py').EIGHT_CORE

def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()

def references(out):
    pins = json.loads((TF / 'manifest.json').read_text())
    for pin in pins:
        assert pin['revision'] == REV and sha(TF / pin['path']) == pin['sha256'], pin['path']
    texts = {}
    for cpu in ('a55', 'a76'):
        p = TF / f'lib/cpus/aarch64/cortex_{cpu}.S'
        texts[cpu] = p.read_text()
    legal = texts['a55'].split('#include')[0]
    assembly = legal + '''
#include "cortex_a55.h"
#include "cortex_a76.h"
.macro sysreg_bit_set reg, bit
mrs x1, \\reg
orr x1, x1, #\\bit
msr \\reg, x1
.endm
.macro sysreg_lazy_start reg
mrs x1, \\reg
.endm
.macro sysreg_lazy_set bit
orr x1, x1, #\\bit
.endm
.macro sysreg_lazy_commit reg
msr \\reg, x1
.endm
.text
'''
    selected = {}
    for cpu, text in texts.items():
        selected[cpu] = []
        for number, body in re.findall(r'workaround_reset_start cortex_\w+, ERRATUM\((\d+)\),[^\n]*\n(.*?)workaround_reset_end', text, re.S):
            if number in ('798953', '936184'): continue  # Shared DSU selection checked by host fixture.
            name = f'ref_{cpu}_{number}'
            assembly += f'.global {name}\n{name}:\n{body}\nret\n'
            bound = re.search(r'check_erratum_ls cortex_'+cpu+r', ERRATUM\('+number+r'\), CPU_REV\((\d+), (\d+)\)', text)
            selected[cpu].append((name, int(bound[1])*16+int(bound[2]) if bound else None))
    # The static store-bypass mitigation uses this exact reset-source statement.
    statement = re.search(r'^\s*sysreg_bit_set CORTEX_A76_CPUACTLR2_EL1, CORTEX_A76_CPUACTLR2_EL1_DISABLE_LOAD_PASS_STORE$', texts['a76'], re.M)[0]
    assembly += '.global ref_ssbd\nref_ssbd:\n'+statement+'\nisb\nret\n'
    (out / 'reference.S').write_text(assembly)
    for cpu in ('a55', 'a76'):
        shutil.copyfile(TF / f'include/lib/cpus/aarch64/cortex_{cpu}.h', out / f'cortex_{cpu}.h')
    (out / 'lib').mkdir(); (out / 'lib/utils_def.h').write_text('#define U(x) x\n#define ULL(x) x\n#define BIT(x) (1 << (x))\n')
    return selected

def emulate(path, selected):
    with path.open('rb') as f:
        elf = ELFFile(f)
        segments = [(s['p_vaddr'], s.data()) for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']
        symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
    decoder = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    decoded = {}
    results = []
    def context(core=0, rev=0x20, el=12, failure=''):
        uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM); uc.mem_map(0x41000000, 0x1200000)
        for addr, data in segments: uc.mem_write(addr, data)
        model = {'currentel': el, 'daif': 0x3c0, 'sctlr_el3': 0x30c50830, 'sctlr_el1': 0x30d00800,
            'midr_el1': (0x410fd050 if core < 6 else 0x410fd0b0)|((rev>>4)<<20)|(rev&15),
            'mpidr_el1': 0x81000000|(core<<8), 'clidr_el1': 0x124, 'id_aa64mmfr0_el1': 2,
            's3_0_c15_c3_1': 0x30, 's3_0_c15_c3_0': 0, 'cntfrq_el0': 24000000,
            'ctr_el0': 4<<16, 's3_6_c15_c8_0': 0}
        for reg in ['s3_0_c15_c1_0','s3_0_c15_c1_1','s3_0_c15_c1_2','s3_0_c15_c1_4','s3_0_c15_c3_3']: model[reg] = 1<<40
        trace, patches, clock = [], {}, [24]
        def gpr(s): return 0 if s in ('xzr','wzr') else uc.reg_read(GPR[int(s[1:])])
        def hook(cpu, addr, size, unused):
            if addr not in decoded: decoded[addr] = next(decoder.disasm(bytes(cpu.mem_read(addr,4)),addr))
            ins = decoded[addr]; args = ins.op_str.split(', ')
            if ins.mnemonic == 'mrs':
                if args[1] == 'cntpct_el0': value=clock[0];clock[0]+=24
                else:
                    assert args[1] in model, (ins.mnemonic,args)
                    value=model[args[1]]
                    if failure == 'readback' and args[1]=='sctlr_el1' and value&1: value &= ~4
                cpu.reg_write(GPR[int(args[0][1:])],value);cpu.reg_write(UC_ARM64_REG_PC,addr+4)
            elif ins.mnemonic == 'msr':
                value = gpr(args[1]);model[args[0]]=value;trace.append((ins.mnemonic,args[0],value))
                if args[0] in ['s3_6_c15_c8_1','s3_6_c15_c8_2','s3_6_c15_c8_3']:
                    patches[(model['s3_6_c15_c8_0'],args[0])]=value
                if failure=='late_enable' and args[0].startswith('sctlr_') and value&1: clock[0]=240000
                cpu.reg_write(UC_ARM64_REG_PC,addr+4)
            elif ins.mnemonic in ('isb','dsb','dmb','tlbi','dc','ic'):
                trace.append((ins.mnemonic,args[0] if args else '',gpr(args[1]) if len(args)>1 else 0))
                if failure=='late_publish' and ins.mnemonic=='dc': clock[0]=240000
                cpu.reg_write(UC_ARM64_REG_PC,addr+4)
        uc.hook_add(UC_HOOK_CODE,hook)
        uc.reg_write(UC_ARM64_REG_SP,0x411f0000)
        def call(name,*args):
            for i,v in enumerate(args):uc.reg_write(GPR[i],v)
            uc.reg_write(UC_ARM64_REG_LR,0x411ff000)
            uc.emu_start(symbols[name],0x411ff000,count=100000)
            assert uc.reg_read(UC_ARM64_REG_PC)==0x411ff000, name
            return uc.reg_read(UC_ARM64_REG_X0)&0xffffffff
        return model,trace,patches,call
    # Compare native compiled controller+adapter against isolated unchanged reset bodies.
    for cpu, core, revs in [('a55',0,[0,1,0x10,0x20]),('a76',6,[0,0x10,0x20,0x30,0x31,0x40,0x41])]:
        for rev in revs:
            native,trace,patches,call=context(core,rev)
            assert call('check_native',core)==1
            ref,rtrace,rpatches,rcall=context(core,rev)
            for name,bound in selected[cpu]:
                if bound is not None and rev>bound:continue
                if name.endswith('_778703') and ref['clidr_el1']&(7<<6):continue
                rcall(name)
            if cpu=='a76':rcall('ref_ssbd')
            keys=['s3_0_c15_c1_0','s3_0_c15_c1_1','s3_0_c15_c1_2','s3_0_c15_c1_4']
            assert {k:native[k] for k in keys}=={k:ref[k] for k in keys}, (cpu,rev)
            assert patches==rpatches,(cpu,rev,patches,rpatches)
            results.append({'kind':'reset-reference','cpu':cpu,'revision':rev,'writes':trace,'patches':str(patches)})
    for el in (1,3):
        for failure in ('','active','expired','readback','late_enable'):
            if el==3 and failure=='readback':continue
            model,trace,patches,call=context(el=el*4,failure=failure)
            if failure=='active':model[f'sctlr_el{el}']|=1
            result=call(f'a733_cpu_coherence_enable_el{el}',0x42000000,1 if failure=='expired' else 240000)
            assert result==(1 if not failure else 0xffffffff),(el,failure,result)
            tlbi=[x for x in trace if x[0]=='tlbi'];enables=[x for x in trace if x[:2]==('msr',f'sctlr_el{el}')]
            if failure in ('active','expired'):assert not tlbi and not enables
            else:
                assert len(tlbi)==2 and len(enables)==1
                assert model[f'mair_el{el}']==0x44ff00
                assert model[f'tcr_el{el}']==(TCR if el==1 else 0x80823019)
                assert model[f'ttbr0_el{el}']==0x42000000
                assert model[f'sctlr_el{el}']&0x1005==0x1005
                first=trace.index(tlbi[0]);second=trace.index(tlbi[1],first+1)
                assert ('dsb','sy',0) in trace[first+1:second]
            results.append({'kind':'cache-enable','el':el,'failure':failure,'trace':trace})
    for failure in ('','late_publish','range','expired'):
        model,trace,patches,call=context(failure=failure)
        result=call('a733_cpu_coherence_publish',0x42000003 if failure!='range' else 0x3fffffff,130,1 if failure=='expired' else 240000)
        assert result==(1 if not failure else 0xffffffff)
        clean=[x[2] for x in trace if x[0]=='dc']
        assert clean==([] if failure in ('range','expired') else [0x42000000] if failure else [0x42000000,0x42000040,0x42000080])
        if not failure:assert ('dsb','sy',0) in trace
        results.append({'kind':'publication','failure':failure,'trace':trace})
    return results

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--evidence',type=Path,required=True);a=p.parse_args()
    out=a.evidence.resolve();assert ROOT not in [out,*out.parents];out.mkdir(parents=True,exist_ok=False)
    commands=[]
    def run(cmd,name):
        commands.append(cmd);r=subprocess.run(cmd,capture_output=True,text=True);(out/name).write_text(r.stdout+r.stderr);r.check_returncode();return r.stdout
    for s in SOURCES:
        dest=out/'source'/s;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(ROOT/s,dest)
    selected=references(out)
    run(['clang-21','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
         '-I',str(ROOT/'include'),str(ROOT/SOURCES[1]),str(ROOT/SOURCES[3]),'-o',str(out/'host')],'host-build.log')
    run([str(out/'host')],'host-run.log')
    (out/'stubs.c').write_text('''#include "a733_cpu_coherence.h"
void *memcpy(void*d,const void*s,size_t n){unsigned char*a=d;const unsigned char*b=s;while(n--)*a++=*b++;return d;}
void *memset(void*d,int v,size_t n){unsigned char*a=d;while(n--)*a++=(unsigned char)v;return d;}
struct a733_cpu_coherence state;
int check_native(unsigned core){if(a733_cpu_coherence_init(&state,&a733_cpu_coherence_native_ops,0,core,1,10000)!=1)return -1;for(unsigned i=0;i<200;++i){int r=a733_cpu_coherence_poll(&state);if(r)return r;}return -1;}
''')
    flags=['clang-21','--target=aarch64-none-elf','-march=armv8-a','-mgeneral-regs-only','-O2','-ffreestanding','-fno-builtin','-fno-stack-protector','-Wall','-Wextra','-Werror','-fstack-usage','-I',str(ROOT/'include'),'-I',str(out)]
    objs=[]
    for source,name in [(ROOT/SOURCES[1],'core'),(ROOT/SOURCES[2],'assembly'),(out/'stubs.c','stubs'),(out/'reference.S','reference')]:
        obj=out/(name+'.o');run(flags+['-c',str(source),'-o',str(obj)],name+'-build.log');objs.append(str(obj))
    (out/'link.ld').write_text('ENTRY(check_native)\nSECTIONS { . = 0x41000000; .text : { *(.text*) } .rodata : { *(.rodata*) } . = 0x41200000; .data : { *(.data*) } .bss (NOLOAD) : { *(.bss*) } /DISCARD/ : { *(.comment) *(.note*) } }\n')
    run([str(Path.home()/'.cache/sunstoneos/host/usr/bin/ld.lld-21'),'-T',str(out/'link.ld'),*objs,'-o',str(out/'coherence.elf')],'link.log')
    cases=emulate(out/'coherence.elf',selected)
    result={'passed':True,'scope':'Host controller/table checks and native instruction paths with modeled system registers and cache instructions; no physical coherency or image integration.',
      'commands':commands,'inputs':{s:sha(ROOT/s) for s in SOURCES},'reference_revision':REV,
      'reference_inputs':{str(p.relative_to(TF)):sha(p) for p in TF.rglob('*') if p.is_file() and p.suffix in ('.h','.S')},
      'artifacts':{p.name:sha(p) for p in out.iterdir() if p.is_file()},'cases':cases}
    (out/'results.json').write_text(json.dumps(result,indent=2)+'\n')
    print(f'PASS host sanitizer and {len(cases)} native/reference cases')
if __name__=='__main__':main()
