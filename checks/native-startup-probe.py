#!/usr/bin/env python3
"""Execute the fixed native startup checkpoints; system registers/MMIO are modeled."""
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
for name in ('build','bundle','output','objdump'):p.add_argument('--'+name,type=Path,required=True)
a=p.parse_args();spec=importlib.util.spec_from_file_location('startup_host',ROOT/'tools/run-native-startup-probe.py');host=importlib.util.module_from_spec(spec);spec.loader.exec_module(host)
m=json.loads((a.build/'manifest.json').read_bytes());bundle=json.loads((a.bundle/'prepared.json').read_bytes())
assert bundle['image_sha256']==host.NATIVE and bundle['launcher_sha256']==host.LAUNCH
ops={}
for path in (a.bundle/'image.elf',a.build/'overlay.elf'):
    result=subprocess.run([str(a.objdump),'-d','--no-show-raw-insn',str(path)],capture_output=True,text=True,check=True).stdout
    for line in result.splitlines():
        match=re.match(r'^\s*([0-9a-f]+):\s+(.+)$',line)
        if match:ops[int(match[1],16)]=match[2].strip()
# Patch branches need actual decoding, rather than the old ELF's disassembly.
for address,old,new in m['patches']:ops[address]=''
nonce=bytes(range(1,17));reports=[]
def check(name,*,mutate=False,watchdog=False,fault=False,sctlr=0xc50838,el=12):
    cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
    for address,size in [(0x41000000,0x1200000),(0x02050000,0x1000),(0x07050000,0x1000),(0x08001000,0x1000)]:cpu.mem_map(address,size)
    for item in m['payloads']:cpu.mem_write(item['address'],(a.build/item['name']).read_bytes())
    cpu.mem_write(0x41020000,(a.build/'overlay.bin').read_bytes())
    for address,size in [(0x41201000,0x8fd10),(0x42005000,0xa590)]:cpu.mem_write(address,b'\xa5'*size)
    contract=bytearray((a.bundle/'contract.bin').read_bytes());start=100
    struct.pack_into('<Q',contract,0,0x4153373353554333)
    struct.pack_into('<4Q',contract,80,start,start+24000000,start+480000000,start+480000000)
    if mutate:struct.pack_into('<Q',contract,40,0)
    cpu.mem_write(0x42004000,bytes(contract));cpu.mem_write(host.REPORT+240,nonce)
    if watchdog:cpu.mem_write(0x02050014,struct.pack('<I',1))
    cpu.reg_write(UC_ARM64_REG_SP,0x41414100)
    registers={'currentel':el,'mpidr_el1':0x81000000,'sctlr_el3':sctlr,'sctlr_el1':0xc50838,
               'cntfrq_el0':0,'esr_el3':0x96000004,'elr_el3':0x41000078,'far_el3':0x41201000}
    writes=[];controls=[];ticks=[];stopped=[];entered=[];injected=[]
    def instruction(uc,address,size,unused):
        if fault and address==0x41000078 and not injected:
            injected.append(True);uc.reg_write(UC_ARM64_REG_PC,0x41008a00);return
        if address==m['hooks']['boot_checkpoint']:
            assert bytes(uc.mem_read(0x41201000,0x8fd10))==bytes(0x8fd10)
            assert bytes(uc.mem_read(0x42005000,0xa590))==bytes(0xa590)
            assert uc.reg_read(UC_ARM64_REG_SP)==0x42014000;entered.append(True)
        text=ops.get(address,'');read=re.match(r'mrs\s+x(\d+),\s*(\S+)',text,re.I)
        if read:
            key=read[2].lower()
            if key=='cntpct_el0':value=200+len(ticks);ticks.append(value)
            else:assert key in registers,(name,key,hex(address));value=registers[key]
            uc.reg_write(UC_ARM64_REG_X0+int(read[1]),value);uc.reg_write(UC_ARM64_REG_PC,address+4)
        elif text.lower().startswith('msr'):
            if 'DAIFSet' in text or 'SPSel' in text:uc.reg_write(UC_ARM64_REG_PC,address+4);return
            write=re.match(r'msr\s+(\S+),\s*(x\d+|xzr)',text,re.I);assert write,(name,text)
            key=write[1].rstrip(',').lower();value=0 if write[2]=='xzr' else uc.reg_read(UC_ARM64_REG_X0+int(write[2][1:]));controls.append([key,value])
            assert key in ('vbar_el3','cntfrq_el0','cntps_ctl_el1','cntp_ctl_el0','cntv_ctl_el0','rmr_el3'),(name,key)
            if key=='rmr_el3':
                assert value==2 and writes[-2:]==[(0x08001004,4,0x20),(0x08001008,4,0)];stopped.append('return');uc.emu_stop()
            else:
                registers[key]=value;uc.reg_write(UC_ARM64_REG_PC,address+4)
        elif text=='wfi':stopped.append('park');uc.emu_stop()
    def write(uc,access,address,size,value,unused):
        assert any(begin<=address and address+size<=begin+length for _,begin,length in host.DRAM) or address in (0x08001004,0x08001008),(name,hex(address),size)
        if address<0x41000000:writes.append((address,size,value))
    cpu.hook_add(UC_HOOK_CODE,instruction);cpu.hook_add(UC_HOOK_MEM_WRITE,write)
    cpu.emu_start(0x41000000,0x41021000,count=1500000)
    if el!=12:
        assert stopped==['park'] and not controls and not entered
        reports.append({'name':name,'wrong_el_park':True});return
    assert stopped==['return'],(name,stopped,hex(cpu.reg_read(UC_ARM64_REG_PC)))
    raw=bytes(cpu.mem_read(host.REPORT,256));result=host.decode(raw,nonce)
    assert result['SCTLR_EL3']==sctlr and result['SCTLR_EL1']==0xc50838
    if fault:assert result['checkpoint']==5 and result['vector']==4 and result['elr']==0x41000078 and not entered
    elif sctlr&0x02001005:assert result['checkpoint']==4 and not entered
    elif mutate or watchdog:
        assert entered and result['checkpoint']==3 and result['status']==1 and result['detail']==(2 if watchdog else 1)
    else:
        assert entered and result['checkpoint']==2 and result['entered_c']==1 and result['entry_sp']==0x42014000
        assert result['CNTFRQ_EL0']==24000000
        assert struct.unpack('<Q',cpu.mem_read(0x42000020,8))[0]==0xffffffffffffffff
    reports.append({'name':name,'result':result,'system_writes':controls,'mmio_writes':writes})
check('actual-stack-bss-and-admission')
check('invalid-contract',mutate=True)
check('changed-watchdog-state',watchdog=True)
check('bss-write-exception',fault=True)
check('initial-cache-rejection',sctlr=0xc51838)
check('wrong-exception-level',el=4)
# The host rejects overlaps before communication; failure details remain data.
valid={'complete':True,'sram_restored':True,'rom_usable':True,'result':{'sp':0xf2200}}
assert host.caller_interval(valid)==(0xf21c4,60)
for sp in (0,0x53010,0x100004,0xf2201):
    try:host.caller_interval({**valid,'result':{'sp':sp}})
    except ValueError:pass
    else:raise AssertionError(sp)
a.output.write_text(json.dumps({'schema':'a733-native-startup-software-v1','physical_access':False,'instruction_cases':True,
    'manifest_sha256':host.digest(a.build/'manifest.json'),'check_sha256':host.digest(Path(__file__)),'cases':reports,
    'limits':'Actual patched native startup instructions with modeled system registers, counter and read-only admission MMIO. No physical cache, return, memory or board qualification.'},indent=2)+'\n')
print('PASS six focused native startup instruction cases and caller-interval validation.')
