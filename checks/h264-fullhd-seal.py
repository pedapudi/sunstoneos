#!/usr/bin/env python3
"""Execute private full-HD EL3 admission/sealing; stop before shared cached startup."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
from capstone import Cs,CS_ARCH_ARM64,CS_MODE_ARM
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR
ROOT=Path(__file__).resolve().parents[1]
SESSION,CONTRACT,STOP=0x41f00000,0x42004000,0x50000000

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('image','entry','output'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();a.output.mkdir(exist_ok=False)
    with a.image.open('rb') as f:
        elf=ELFFile(f);symbols={}
        for s in elf.get_section_by_name('.symtab').iter_symbols():
            if s['st_value']:symbols.setdefault(s.name,set()).add(int(s['st_value']))
        required=['a733_smp_boot','h264_admission','h264_fullhd_board_prepare','h264_fullhd_boot_record',
            'h264_session_prepare','h264_admitted_session','board_session_snapshot','a733_device_memory_boot_admit',
            'a733_cpu_coherence_publish','a733_cached_prepare','a733_smp_stop','__a733_smp_boot_stack_top']
        assert all(len(symbols.get(n,set()))==1 for n in required)
        syms={n:next(iter(symbols[n])) for n in required}
        segments=[(int(s['p_vaddr']),bytes(s.data())) for s in elf.iter_segments() if s['p_type']=='PT_LOAD' and s['p_filesz']]
    entry=json.loads((a.entry/'result.json').read_text());assert entry['passed'] and entry['image_sha256']==hashlib.sha256(a.image.read_bytes()).hexdigest()
    observed=next(c for c in entry['cases'] if c['case']=='full-HD-version3');assert observed['reason']==12
    source=a.entry/'full-HD-version3';session=(source/'session.bin').read_bytes();published=bytearray((source/'contract.bin').read_bytes())
    # Every reconstructed byte was asserted against the actual launcher output:
    # unchanged bytes plus its observed counter values and published magic.
    start=observed['counter_reads'][0];struct.pack_into('<Q',published,0,0x4153373348324331)
    struct.pack_into('<4Q',published,80,start,start+24000000,start+1440000000,start+1440000000)
    (a.output/'published-contract.bin').write_bytes(published)
    reports=[]
    for case in ('nominal','wrong-session-hash','short-record','wrong-WB-range','live-entry-change-before-seal','session-change-between-copies'):
        cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);cpu.mem_map(0x41000000,0x1200000);cpu.mem_map(STOP,4096)
        cpu.mem_map(0x02050000,4096);cpu.mem_map(0x07050000,4096)
        for address,data in segments:cpu.mem_write(address,data)
        raw=bytearray(session);contract=bytearray(published)
        if case=='wrong-session-hash':struct.pack_into('<Q',raw,616,99)
        if case=='short-record':struct.pack_into('<Q',contract,16,224)
        if case=='wrong-WB-range':struct.pack_into('<Q',contract,200,0x77fff000)
        cpu.mem_write(SESSION,bytes(raw));cpu.mem_write(CONTRACT,bytes(contract))
        decoder=Cs(CS_ARCH_ARM64,CS_MODE_ARM);decoded={};effects=[];publications=[];memory_calls=[];stopped=[]
        facts={'currentel':12,'mpidr_el1':0x81000000,'sctlr_el3':0,'sctlr_el1':0,'cntpct_el0':start+100}
        mmio={0x02050014:0,0x02050010:0,0x070501c0:0,0x070501d4:0}
        def returned(value):cpu.reg_write(UC_ARM64_REG_X0,value);cpu.reg_write(UC_ARM64_REG_PC,cpu.reg_read(UC_ARM64_REG_LR))
        def instruction(u,address,size,unused):
            if address==STOP:stopped.append('return');u.emu_stop();return
            if address==syms['a733_smp_stop']:
                stopped.append(('refused',u.reg_read(UC_ARM64_REG_X0),u.reg_read(UC_ARM64_REG_X1)));u.emu_stop();return
            if address==syms['a733_cached_prepare']:
                stopped.append('before-shared-cached-prepare');u.emu_stop();return
            if address==syms['a733_device_memory_boot_admit']:
                pointer=u.reg_read(UC_ARM64_REG_X0);assert pointer==syms['h264_admission']
                assert bytes(u.mem_read(pointer,288))==bytes(published[:288])
                memory_calls.append(hex(pointer));returned(1);return
            if address==syms['a733_cpu_coherence_publish']:
                pointer,n,deadline=[u.reg_read(r) for r in (UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2)]
                assert deadline==start+24000000 and (pointer==SESSION and n==4096 or 0x41200000<=pointer<pointer+n<=0x41f00000)
                publications.append((pointer,n))
                if case=='session-change-between-copies' and len(publications)==1:u.mem_write(SESSION+616,struct.pack('<Q',99))
                returned(1);return
            if address==syms['h264_fullhd_board_prepare'] and case=='live-entry-change-before-seal':u.mem_write(CONTRACT+256,bytes(8))
            if address not in decoded:decoded[address]=next(decoder.disasm(bytes(u.mem_read(address,4)),address))
            ins=decoded[address]
            if ins.mnemonic=='mrs':
                register,name=ins.op_str.split(', ');assert name in facts,(case,name)
                index=int(register[1:]);u.reg_write(UC_ARM64_REG_X0+index,facts[name]);u.reg_write(UC_ARM64_REG_PC,address+4)
        def read(u,access,address,size,value,unused):
            if address>=0x41000000:return
            assert address in mmio and size==4,(case,hex(address),size)
            u.mem_write(address,struct.pack('<I',mmio[address]))
        def write(u,access,address,size,value,unused):
            if address<0x41000000:raise AssertionError(('unexpected peripheral write',case,hex(address)))
            effects.append((address,size))
        cpu.hook_add(UC_HOOK_CODE,instruction);cpu.hook_add(UC_HOOK_MEM_READ,read);cpu.hook_add(UC_HOOK_MEM_WRITE,write)
        cpu.reg_write(UC_ARM64_REG_SP,syms['__a733_smp_boot_stack_top']);cpu.reg_write(UC_ARM64_REG_LR,STOP)
        cpu.emu_start(syms['a733_smp_boot'],0,count=3000000);assert stopped,(case,'instruction budget')
        assert bytes(cpu.mem_read(syms['h264_admission'],288))==bytes(contract[:288])
        if case=='nominal':
            assert stopped==['before-shared-cached-prepare'] and len(publications)==3 and len(memory_calls)==1
            for name,length in [('h264_admitted_session',856),('board_session_snapshot',856)]:
                cpu.mem_write(SESSION+616,struct.pack('<Q',99));stopped.clear()
                cpu.reg_write(UC_ARM64_REG_LR,STOP);cpu.reg_write(UC_ARM64_REG_SP,syms['__a733_smp_boot_stack_top'])
                cpu.emu_start(next(iter(symbols[name])),0,count=1000)
                pointer=cpu.reg_read(UC_ARM64_REG_X0);assert stopped==['return'] and bytes(cpu.mem_read(pointer,length))==session
            stopped[:]=['before-shared-cached-prepare']
        elif case=='live-entry-change-before-seal':assert stopped==[('refused',3,0x30c)] and not publications and not memory_calls
        elif case=='session-change-between-copies':assert stopped==[('refused',3,0x30b)] and len(publications)==1 and len(memory_calls)==1
        else:assert stopped==[('refused',1,1)] and not publications and not memory_calls
        reports.append({'case':case,'stop':stopped,'publications':publications,'memory_admission_arguments':memory_calls,'peripheral_writes':0})
    result={'passed':True,'physical_access':False,'image_sha256':hashlib.sha256(a.image.read_bytes()).hexdigest(),'cases':reports,
        'scope':'Actual private linked288-byte boot copy, full-session/board validators and sealers. Initial BSS clearing, EL3 system-register/counter facts, cache publication and extended-memory admission boundary are modeled. Stops before a733_cached_prepare; no shared cached preparation, GIC, secondary CPU, allocator or device execution.'}
    (a.output/'result.json').write_text(json.dumps(result,indent=2)+'\n');print('PASS private full-HD 288-byte boot admission/sealers, stable copies and changed-record/session refusal')
if __name__=='__main__':main()
