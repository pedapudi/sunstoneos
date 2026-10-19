#!/usr/bin/env python3
"""Execute decoder fault scans through the actual lowest H264 native read policy."""
import argparse
import importlib.util
import json
from pathlib import Path
import struct
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_READ
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X3,UC_ARM64_REG_X4,UC_ARM64_REG_SP,UC_ARM64_REG_LR,UC_ARM64_REG_PC
ROOT=Path(__file__).resolve().parents[1]
STOP=0x50fff000;IOMMU=0x03900000
REGS=[UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X3,UC_ARM64_REG_X4]
def main():
    p=argparse.ArgumentParser(description=__doc__)
    for n in ('image','inputs','native','output'):p.add_argument('--'+n,type=Path,required=True)
    a=p.parse_args();a.output.mkdir(exist_ok=False)
    spec=importlib.util.spec_from_file_location('cases',ROOT/'checks/h264-application-cases.py');cases=importlib.util.module_from_spec(spec);spec.loader.exec_module(cases)
    syms,segments,types,_,_=cases.setup(a.image,a.output,session_optional=True,fixture_optional=True)
    names={'h264_now','h264_iommu_admit','h264_iommu_read','h264_iommu_write','h264_publish'};native={}
    with a.image.open('rb') as file:
        elf=ELFFile(file)
        for cu in elf.get_dwarf_info().iter_CUs():
            name=cu.get_top_DIE().attributes.get('DW_AT_name')
            if not name or not name.value.decode().endswith('/apps/h264/native.c'):continue
            for d in cu.iter_DIEs():
                name=d.attributes.get('DW_AT_name')
                if name is None and 'DW_AT_abstract_origin' in d.attributes:
                    name=d.get_DIE_from_attribute('DW_AT_abstract_origin').attributes.get('DW_AT_name')
                if d.tag=='DW_TAG_subprogram' and name and name.value.decode() in names and 'DW_AT_low_pc' in d.attributes:
                    key=name.value.decode();assert key not in native;native[key]=d.attributes['DW_AT_low_pc'].value
    assert set(native)==names
    cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);cpu.mem_map(0x40000000,0x4000000);cpu.mem_map(0x50000000,0x1000000);cpu.mem_map(IOMMU,0x20000)
    for address,data in segments:cpu.mem_write(address,data)
    base=syms['h264'];app=types['h264_application'];io=types['a733_iommu'];state=base+app['iommu'];tables=base+app['tables']
    session=(a.inputs/'qualification-session.bin').read_bytes();cpu.mem_write(base,session);cpu.mem_write(syms['board']+160,struct.pack('<Q',1000000))
    cpu.mem_write(base+app['table'],struct.pack('<Q',0x50000000))
    declared=json.loads((a.native/'result.json').read_text())['cases'][0]['spans'];spanbase=0x50f20000
    for i,(va,pa,n,access) in enumerate(declared):cpu.mem_write(spanbase+i*32,struct.pack('<3QI4x',va,pa,n,access))
    operations=0x50f10000;cpu.mem_write(operations,struct.pack('<5Q',*[native[n] for n in ('h264_now','h264_iommu_admit','h264_iommu_read','h264_iommu_write','h264_publish')]))
    time=[1000];reads=[];translations=[];bad_mapping=[False];stopped=[False]
    def returned(value):cpu.reg_write(REGS[0],value);cpu.reg_write(UC_ARM64_REG_PC,cpu.reg_read(UC_ARM64_REG_LR))
    def hook(c,address,size,unused):
        if address==STOP:stopped[0]=True;c.emu_stop()
        elif address==syms['board_live']:returned(1)
        elif address==syms['a733_clock_token_status']:returned(0) # Held physical clock authority is modeled.
        elif address==syms['a733_counter_clock_now']:time[0]+=1;c.mem_write(c.reg_read(REGS[1]),struct.pack('<Q',time[0]));returned(1)
        elif address==syms['aarch64_device_translate']:
            address=c.reg_read(REGS[0]);write=c.reg_read(REGS[1]);out=c.reg_read(REGS[2]);translations.append(address)
            assert IOMMU<=address<IOMMU+0x20000 and write==1
            c.mem_write(out,struct.pack('<Q2I',address,0xff if bad_mapping[0] else 0,2));returned(1)
    for address in {STOP,syms['board_live'],syms['a733_clock_token_status'],syms['a733_counter_clock_now'],syms['aarch64_device_translate']}:
        cpu.hook_add(UC_HOOK_CODE,hook,begin=address,end=address)
    cpu.hook_add(UC_HOOK_MEM_READ,lambda c,kind,address,size,value,unused:reads.append(address),begin=IOMMU,end=IOMMU+0x1ffff)
    def invoke(address,*values):
        for register,value in zip(REGS,values):cpu.reg_write(register,value)
        stopped[0]=False;cpu.reg_write(UC_ARM64_REG_SP,0x50ffeff0);cpu.reg_write(UC_ARM64_REG_LR,STOP);cpu.emu_start(address,0,count=2000000);assert stopped[0]
        value=cpu.reg_read(REGS[0])&0xffffffff;return value if value<0x80000000 else value-0x100000000
    def get(address,size=4):return int.from_bytes(cpu.mem_read(address,size),'little')
    assert invoke(syms['a733_iommu_build'],tables,0x50000000,0x50000000,spanbase,6)==1
    assert invoke(syms['a733_iommu_init_decoder'],state,operations,base,tables)==1
    assert invoke(native['h264_iommu_admit'],base,tables,990000,state+io['admission'])==1
    # Initial installation/power/isolation are modeled. The scan and every lower
    # read, Device-attribute check and ordered load execute from the actual ELF.
    cpu.mem_write(state+io['state'],struct.pack('<I',5))
    expected=[IOMMU+bank+offset for bank in (0,0x10000) for offset in (0x88,0xb0,0xb4,0x8c,0x90,0x9c,0x94,0x98,0xa0)]
    expected += [IOMMU+micro+offset for micro in (0x10400,0x10600) for offset in (0x38,0x3c,0x40,0x44)]
    assert invoke(syms['a733_iommu_scan'],state,990000)==1
    for _ in range(100):
        result=invoke(syms['a733_iommu_poll'],state)
        if result:break
    assert result==1 and reads==expected and translations==expected
    fault=state+io['fault'];assert get(fault+types['a733_iommu_fault']['scanned_masters'],2)==0xc0 and get(fault+types['a733_iommu_fault']['complete'],1)==1
    observed=list(reads);refusals=[]
    for name,token,offset,deadline in [('wrong-token',14,0x10438,990000),('out-of-range',13,0x20000,990000),('unaligned',13,0x10439,990000),('wrong-deadline',13,0x10438,990001)]:
        before=len(reads);assert invoke(native['h264_iommu_read'],base,token,offset,0x50f30000,deadline)==-1 and len(reads)==before;refusals.append(name)
    bad_mapping[0]=True;before=len(reads);assert invoke(native['h264_iommu_read'],base,13,0x10438,0x50f30000,990000)==-1 and len(reads)==before;bad_mapping[0]=False;refusals.append('write-back-MMIO-mapping')
    cpu.mem_write(IOMMU+0x10438,struct.pack('<I',1));reads.clear();translations.clear();assert invoke(syms['a733_iommu_scan'],state,990000)==1
    for _ in range(100):
        result=invoke(syms['a733_iommu_poll'],state)
        if result:break
    assert result==-1 and get(state+io['state'])==7 and reads==expected
    (a.output/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,mmio_base=hex(IOMMU),successful_scan_reads=[hex(x) for x in observed],fault_scan_reads=[hex(x) for x in reads],refusals=refusals,scope='Actual linked IOMMU scan/poll -> native h264_iommu_read -> h264_load -> h264_device -> ordered MMIO loads. CPU translation, clock/power authority and initial installation are modeled; no parsing/copy case replay.'),indent=2)+'\n')
    print('PASS actual native decoder-IOMMU read path, both banks/masters6and7, refusal and retained fault')
if __name__=='__main__':main()
