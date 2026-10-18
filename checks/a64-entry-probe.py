#!/usr/bin/env python3
"""One stackless A64 snapshot/forced-FEL-return instruction case; no hardware."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import struct
import subprocess
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC,UC_ARM64_REG_SP
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
for name in ('build','output','objdump'):p.add_argument('--'+name,type=Path,required=True)
a=p.parse_args();binary=(a.build/'probe.bin').read_bytes()
spec=importlib.util.spec_from_file_location('entry_probe',ROOT/'tools/run-a64-entry-probe.py');host=importlib.util.module_from_spec(spec);spec.loader.exec_module(host)
disassembly=subprocess.run([str(a.objdump),'-d','--no-show-raw-insn',str(a.build/'probe.elf')],capture_output=True,text=True,check=True).stdout
(a.output.parent/'probe-disassembly.txt').write_text(disassembly)
ops={int(m[1],16):m[2].strip() for line in disassembly.splitlines() if (m:=re.match(r'^\s*([0-9a-f]+):\s+(.+)$',line))}
cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);cpu.mem_map(0x41000000,0x20000);cpu.mem_map(0x42000000,0x10000);cpu.mem_map(0x08001000,0x1000)
cpu.mem_write(0x41000000,binary);pre=100;nonce=bytes(range(1,17));initial=bytearray(256);struct.pack_into('<Q',initial,104,pre);initial[240:]=nonce;cpu.mem_write(0x42000000,bytes(initial))
cpu.reg_write(UC_ARM64_REG_SP,0x41414100)
# Inherited EL1 bits deliberately differ from the cached predicate. Capturing
# them must still return; this test establishes no physical bank initialization.
registers={'currentel':12,'midr_el1':0x412fd050,'mpidr_el1':0x81000000,'cntfrq_el0':0,
 'rvbar_el3':0x41000000,'sctlr_el3':0xc50838,'sctlr_el1':0x30d01805,'scr_el3':0x10}
writes=[];reads=[];controls=[];ticks=[];stopped=[]
def instruction(uc,address,size,unused):
    text=ops.get(address,'');read=re.match(r'mrs\s+x(\d+),\s*(\S+)',text,re.I)
    if read:
        name=read[2].lower()
        if name=='cntpct_el0':value=200+len(ticks);ticks.append(value)
        else:assert name in registers,name;value=registers[name]
        reads.append(name);uc.reg_write(UC_ARM64_REG_X0+int(read[1]),value);uc.reg_write(UC_ARM64_REG_PC,address+4)
    elif text.lower().startswith('msr'):
        if 'DAIFSet' in text:uc.reg_write(UC_ARM64_REG_PC,address+4);return
        write=re.match(r'msr\s+(\S+),\s*x(\d+)',text,re.I);assert write,text
        name=write[1].rstrip(',').lower();value=uc.reg_read(UC_ARM64_REG_X0+int(write[2]));controls.append((name,value))
        assert name in ('vbar_el3','rmr_el3'),name
        if name=='rmr_el3':
            assert value==2 and writes[-2:]==[(0x08001004,4,0x20),(0x08001008,4,0)]
            stopped.append('forced-fel');uc.emu_stop()
        else:assert value%2048==0;uc.reg_write(UC_ARM64_REG_PC,address+4)
def write(uc,access,address,size,value,unused):
    assert 0x42000000<=address and address+size<=0x42000100 or address in (0x08001004,0x08001008),hex(address)
    writes.append((address,size,value))
cpu.hook_add(UC_HOOK_CODE,instruction);cpu.hook_add(UC_HOOK_MEM_WRITE,write);cpu.emu_start(0x41000000,0x41020000,count=1000)
assert stopped==['forced-fel'] and cpu.reg_read(UC_ARM64_REG_SP)==0x41414100
raw=bytes(cpu.mem_read(0x42000000,256));result=host.decode(raw,nonce,pre)
assert result['entry_invariants']==dict.fromkeys(('el3','primary','sctlr3','counter_monotonic'),True)
assert result['cached_initial_sctlr1_predicate'] is False and result['SCTLR_EL1']==registers['sctlr_el1']
assert result['layout']==dict(zip(host.SYMBOLS,host.LAYOUT)) and len(ticks)==2
changed=bytearray(raw);struct.pack_into('<Q',changed,104,300)
discontinuous=host.decode(bytes(changed),nonce,300)
assert discontinuous['entry_invariants']['counter_monotonic'] is False and discontinuous['counter_post']==200
(a.output.parent/'modeled-report.bin').write_bytes(raw)
a.output.write_text(json.dumps({'schema':'a733-a64-entry-probe-software-v1','physical_access':False,'return_case':True,
 'binary_sha256':hashlib.sha256(binary).hexdigest(),'check_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
 'reads':reads,'controls':controls,'result':result,'limits':'Modeled architectural reads and RMR interception only. Prior actual forced-FEL return evidence reused; no new physical return, entry-state or memory qualification.'},indent=2)+'\n')
print('PASS one A64 report/forced-FEL-return case; inherited EL1 state captured without adopting it.')
