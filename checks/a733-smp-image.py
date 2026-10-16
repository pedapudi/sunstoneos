#!/usr/bin/env python3
"""Build the separate image; check admission, ELF placement and native SGI calls."""
import argparse
import hashlib
import json
import shutil
import struct
import subprocess
from pathlib import Path
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_LR, UC_ARM64_REG_SP, UC_ARM64_REG_PC
ROOT=Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inspect_image(path,execute=True):
    with path.open('rb') as f:
        elf=ELFFile(f)
        segments=[(s['p_vaddr'],s['p_memsz'],s['p_flags'],s.data())
                  for s in elf.iter_segments() if s['p_type']=='PT_LOAD']
        symbols={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
        assert elf.header['e_entry']==symbols['a733_smp_entry']==0x41000000
    for address,size,flags,data in segments:
        assert 0x41000000<=address<0x42200000 and address+size<=0x42200000
        assert flags&3!=3, 'ELF segment is writable and executable'
        if flags&1: assert address+size<=0x41200000
    for name in ['a733_cpu_mailboxes','a733_cpu_entry_stacks','a733_cpu_continue_plan',
                 'a733_cpu_continue_records','a733_cpu_continue_level1','a733_cpu_continue_level2']:
        assert 0x42005000<=symbols[name]<0x42200000,(name,hex(symbols[name]))
    assert 0x41200000<=symbols['a733_cpu_el1_stacks']
    assert symbols['a733_cpu_el1_stacks']+8*32768<=0x42000000
    assert symbols['__a733_smp_result']==0x42000000
    assert symbols['__a733_smp_result_end']==0x42004000
    assert symbols['__a733_smp_contract']==0x42004000
    assert symbols['__a733_smp_contract_end']==0x42005000
    layout={'symbols':{n:hex(a) for n,a in symbols.items()
                      if n.startswith('a733_') or n.startswith('__a733_')},
            'segments':[{'address':hex(a),'memory_bytes':s,'file_bytes':len(d),'flags':f}
                        for a,s,f,d in segments]}
    if not execute:return layout
    decoder=Cs(CS_ARCH_ARM64,CS_MODE_ARM)
    outcomes=[]
    for target in range(8):
        uc=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
        uc.mem_map(0x41000000,0x1200000)
        for address,_,_,data in segments: uc.mem_write(address,data)
        events=[]
        def step(cpu,address,size,unused):
            ins=next(decoder.disasm(bytes(cpu.mem_read(address,4)),address))
            if ins.mnemonic=='svc':
                events.append((int(ins.op_str.lstrip('#'),0),cpu.reg_read(UC_ARM64_REG_X0)))
                cpu.emu_stop()
        uc.hook_add(UC_HOOK_CODE,step)
        uc.reg_write(UC_ARM64_REG_X0,0)
        uc.reg_write(UC_ARM64_REG_X1,target)
        uc.reg_write(UC_ARM64_REG_SP,0x41fff000)
        uc.reg_write(UC_ARM64_REG_LR,0x41000000)
        uc.emu_start(symbols['vInterruptCore'],0x42200000,count=10000)
        assert events==[(117,(target<<16)|1)],(target,events)
        outcomes.append({'target':target,'sgi1r':hex(events[0][1])})
    probe_results=[]
    for name,target,missing,held in [('primary',0,False,False),('secondary',7,False,False),
                                     ('missing_peer',0,True,False),('held_lock',7,False,True)]:
        uc=Uc(UC_ARCH_ARM64,UC_MODE_ARM);uc.mem_map(0x41000000,0x1200000)
        for address,_,_,data in segments: uc.mem_write(address,data)
        uc.mem_write(symbols['probe_arrived'],struct.pack('<8I',*([1]*7+[0 if missing else 1])))
        uc.mem_write(symbols['probe_finished'],struct.pack('<8I',*([1]*8)))
        uc.mem_write(symbols['probe_count'],struct.pack('<I',224))
        uc.mem_write(symbols['probe_release'],struct.pack('<I',int(target!=0)))
        uc.mem_write(symbols['probe_lock'],struct.pack('<I',int(held)))
        clock=[100];returned=[]
        def probe_step(cpu,address,size,unused):
            if address==0x41ffe000:
                returned.append(cpu.reg_read(UC_ARM64_REG_X0));cpu.emu_stop();return
            ins=next(decoder.disasm(bytes(cpu.mem_read(address,4)),address))
            if ins.mnemonic=='mrs':
                register,system=ins.op_str.split(', ')
                assert system=='cntpct_el0',system
                clock[0]+=24
                cpu.reg_write(UC_ARM64_REG_X0+int(register[1:]),clock[0])
                cpu.reg_write(UC_ARM64_REG_PC,address+4)
        uc.hook_add(UC_HOOK_CODE,probe_step)
        uc.reg_write(UC_ARM64_REG_X0,target);uc.reg_write(UC_ARM64_REG_X1,5)
        uc.reg_write(UC_ARM64_REG_X0+2,200)
        uc.reg_write(UC_ARM64_REG_SP,0x41fff000);uc.reg_write(UC_ARM64_REG_LR,0x41ffe000)
        uc.emu_start(symbols['a733_smp_pre_scheduler'],0x42200000,count=30000)
        assert returned==[0 if missing or held else 1],(name,returned)
        count=struct.unpack('<I',uc.mem_read(symbols['probe_count'],4))[0]
        assert count==(224 if missing or held else 256),(name,count)
        probe_results.append({'case':name,'returned':returned[0],'counter':count})
    return {**layout,'sgi_calls':outcomes,'shared_probe':probe_results}


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--evidence',type=Path,required=True)
    p.add_argument('--freertos',type=Path,required=True)
    p.add_argument('--continuation-root',type=Path,default=ROOT)
    p.add_argument('--clang',default='clang-21')
    p.add_argument('--lld',type=Path,default=Path.home()/'.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld')
    a=p.parse_args();e=a.evidence.resolve();e.mkdir(parents=True,exist_ok=False)
    source=e/'source';source.mkdir();commands=[]
    # Copy all image-owned source and every direct repository dependency.
    paths=[*ROOT.glob('platform/a733-smp/*'),*ROOT.glob('include/*.h'),
           *ROOT.glob('runtime/include/*.h'),ROOT/'runtime/memory.c',ROOT/'runtime/memmove.c',ROOT/'checks/fp-context.S',
           ROOT/'checks/a733-smp-image.py',ROOT/'cmake/a733-smp.cmake',ROOT/'cmake/aarch64-clang.cmake',
           *ROOT.glob('patches/freertos-smp-*.patch'),ROOT/'patches/freertos-a733-smp.patch']
    paths += [ROOT/'platform/a733'/n for n in ['cpu_start.c','cpu_entry.c','cpu_entry.S','cpu_entry_native.c']]
    for old in paths:
        rel=old.relative_to(ROOT);new=source/rel;new.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(old,new)
    for rel in ['include/a733_cpu_continue.h','platform/a733/cpu_continue.c','platform/a733/cpu_continue.S']:
        new=source/rel;new.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(a.continuation_root/rel,new)
    dep=e/'freertos';dep.mkdir()
    for rel in ['tasks.c','queue.c','list.c']:
        shutil.copy2(a.freertos/rel,dep/rel)
    shutil.copytree(a.freertos/'include',dep/'include')
    shutil.copytree(a.freertos/'portable/GCC/ARM_CR82',dep/'portable/GCC/ARM_CR82')
    def run(args,name):
        args=[str(x) for x in args];commands.append(args)
        r=subprocess.run(args,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
        (e/(name+'.log')).write_text(r.stdout)
        (e/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
        if r.returncode: raise RuntimeError(f'{name} failed: {r.stdout[-6000:]}')
    clang=Path(shutil.which(a.clang)).resolve()
    run([clang,'-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
         source/'platform/a733-smp/admission.c',source/'platform/a733-smp/admission-check.c',
         '-o',e/'admission-check'],'host-build')
    run([e/'admission-check'],'host-check')
    build=e/'build'
    toolchain=e/'toolchain.cmake'
    toolchain.write_text(f'set(SUNSTONE_LLD "{a.lld}" CACHE FILEPATH "")\n'
                         f'include("{source}/cmake/aarch64-clang.cmake")\n'
                         f'set(CMAKE_C_COMPILER "{clang}")\nset(CMAKE_ASM_COMPILER "{clang}")\n')
    run(['cmake','-G','Unix Makefiles','-S',source/'platform/a733-smp','-B',build,
         f'-DCMAKE_TOOLCHAIN_FILE={toolchain}',
         f'-DFREERTOS_DIR={dep}',f'-DSUNSTONE_LLD={a.lld}'],'configure')
    run(['cmake','--build',build,'--target','a733-smp-diagnostic'],'image-build')
    result=inspect_image(build/'a733-smp-diagnostic.elf')
    (e/'image-check.json').write_text(json.dumps(result,indent=2)+'\n')
    files={str(f.relative_to(e)):sha(f) for f in e.rglob('*') if f.is_file()}
    (e/'manifest.json').write_text(json.dumps({'files':files,'tools':{str(clang):sha(clang),str(a.lld):sha(a.lld)},
        'result':'PASS host admission, ELF placement and eight native SGI calls; no board or full scheduler execution'},indent=2)+'\n')
    print('PASS admission, ELF layout and Aff1 SGI calls for eight cores; no physical execution')


if __name__=='__main__':main()
