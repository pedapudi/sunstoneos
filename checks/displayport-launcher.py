#!/usr/bin/env python3
"""Check the prepared DisplayPort SRAM launcher without rebuilding or accessing hardware."""
import argparse,hashlib,json,struct
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE
from unicorn.arm_const import UC_CPU_ARM_CORTEX_A15,UC_ARM_REG_R0,UC_ARM_REG_PC,UC_ARM_REG_CPSR,UC_ARM_REG_SP,UC_ARM_REG_LR
SESSION,CONTRACT,REQUEST,REPORT=0x41f00000,0x42004000,0x52c00,0x52d00
MODE,CONFIG,FLAG,ALT,RVBAR=0x02050014,0x02050010,0x070501c0,0x070501d4,0x08001004
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--bundle',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=False)
image=args.bundle/'image.elf'
with image.open('rb') as stream:
    elf=ELFFile(stream)
    segments=[(int(s['p_vaddr']),s.data()) for s in elf.iter_segments() if s['p_type']=='PT_LOAD']
code=(args.bundle/'launch.bin').read_bytes()
assert 0<len(code)<=0xc00
template=(args.bundle/'session.bin').read_bytes()
contract_page=(args.bundle/'contract.bin').read_bytes()
assert len(contract_page)==4096 and not any(contract_page[288:])
contract=contract_page[:288]
request=(args.bundle/'request.bin').read_bytes()
assert len(template)==952 and len(contract)==288
assert struct.unpack_from('<3Q',template,592)==(0x4153373344504131,1,952)
identity=struct.unpack_from('<3Q',template,24)
results=[]
for name,offset,value,reason in [
    ('bounded-handoff',None,None,12),
    ('typec-source-overvoltage',916,struct.pack('<I',5251),14),
    ('memory-authority-mismatch',200,bytes(8),14),
]:
    session=bytearray(template)
    if offset is not None:session[offset:offset+len(value)]=value
    cpu=Uc(UC_ARCH_ARM,UC_MODE_ARM);cpu.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_A15)
    for address,size in [(0x50000,0x30000),(0x41000000,0x1200000),(0x02050000,4096),
                         (0x07050000,4096),(0x08001000,4096)]:
        cpu.mem_map(address,size)
    for address,data in segments:
        if data:cpu.mem_write(address,data)
    for address,data in [(0x52000,code),(REQUEST,request),(CONTRACT,contract),(SESSION,bytes(session))]:
        cpu.mem_write(address,data)
    cpu.mem_write(0x52ff0,b'A'*16);cpu.reg_write(UC_ARM_REG_CPSR,0xa80f0053)
    saved={UC_ARM_REG_R0+i:0x12340000+i*0x101 for i in range(13)}
    saved.update({UC_ARM_REG_SP:0x70000,UC_ARM_REG_LR:0x60000,UC_ARM_REG_CPSR:cpu.reg_read(UC_ARM_REG_CPSR)})
    for register,value in saved.items():cpu.reg_write(register,value)
    system={0xee110f10:0,0xee100fb0:0x81000000,0xee1e0f10:24000000,0xee1c0f50:0}
    mmio={MODE:0,CONFIG:0,FLAG:0,ALT:0,RVBAR:0,RVBAR+4:0}
    writes=[];ticks=[];terminal=[]
    def read(uc,access,address,size,value,unused):
        if address<0x02000000 or address>=0x41000000:return
        assert address in mmio and size==4,(name,hex(address),size)
        uc.mem_write(address,struct.pack('<I',mmio[address]))
    def write(uc,access,address,size,value,unused):
        if address<0x02000000:
            assert REPORT<=address<REPORT+128 or 0x53000<=address<0x54000 or 0x70000-60<=address<0x70000
        elif address>=0x41000000:
            assert CONTRACT<=address<CONTRACT+8 or CONTRACT+80<=address<CONTRACT+112
        else:
            assert address in (RVBAR,RVBAR+4) and size==4
            assert struct.unpack('<Q',uc.mem_read(CONTRACT,8))[0]==0x4153373344504331
            writes.append((address,value));mmio[address]=value
    def instruction(uc,address,size,unused):
        if address==0x60000:terminal.append('return');uc.emu_stop();return
        word=struct.unpack('<I',uc.mem_read(address,4))[0]
        if word&0xffff0fff in system:
            uc.reg_write(UC_ARM_REG_R0+((word>>12)&15),system[word&0xffff0fff]);uc.reg_write(UC_ARM_REG_PC,address+4)
        elif word&0xfff00fff==0xec500f0e:
            value=100000000+100*len(ticks);ticks.append(value)
            uc.reg_write(UC_ARM_REG_R0+((word>>12)&15),value&0xffffffff)
            uc.reg_write(UC_ARM_REG_R0+((word>>16)&15),value>>32);uc.reg_write(UC_ARM_REG_PC,address+4)
        elif word==0xee0c0f50:
            assert uc.reg_read(UC_ARM_REG_R0)==3 and mmio[RVBAR]==0x41000000 and mmio[RVBAR+4]==0
            terminal.append('handoff');uc.emu_stop()
        elif word==0xe320f003:raise AssertionError(name+' unexpectedly parked')
    cpu.hook_add(UC_HOOK_MEM_READ,read);cpu.hook_add(UC_HOOK_MEM_WRITE,write)
    cpu.hook_add(UC_HOOK_CODE,instruction)
    cpu.emu_start(0x52000,0x52c00,count=20000)
    report=struct.unpack('<16Q',cpu.mem_read(REPORT,128))
    published=bytes(cpu.mem_read(CONTRACT,288))
    assert report[0]==reason,(name,report[0],terminal)
    assert bytes(cpu.mem_read(SESSION,len(session)))==session
    assert bytes(cpu.mem_read(0x52ff0,16))==b'A'*16
    if reason==12:
        assert terminal==['handoff'] and writes==[(RVBAR,0x41000000),(RVBAR+4,0)] and len(ticks)==4
        assert struct.unpack_from('<4Q',published,80)==(ticks[0],ticks[0]+24000000,ticks[0]+480000000,ticks[0]+480000000)
        assert published[8:80]==contract[8:80] and published[112:]==contract[112:]
    else:
        assert terminal==['return'] and not writes and not ticks and published==contract
        assert report[1:7]==(0,0,*identity,0x4153373352454631) and not any(report[7:])
        for register,value in saved.items():assert cpu.reg_read(register)==value,(name,register)
    results.append(dict(case=name,reason=reason,terminal=terminal[0],mmio_writes=writes,counter_reads=ticks))
report=dict(passed=True,physical_access=False,cases=results,
    image_sha256=hashlib.sha256(image.read_bytes()).hexdigest(),
    launcher_sha256=hashlib.sha256(code).hexdigest(),session_sha256=hashlib.sha256(template).hexdigest(),
    scope='Actual prepared AArch32 entry, DisplayPort source/authority predicates, fixed 20-second contract publication and refusal restoration. System registers, counter and MMIO are modeled. No rebuild, reset, CPU startup, USB or physical execution.')
(args.output/'result.json').write_text(json.dumps(report,indent=2)+'\n')
print('PASS three DisplayPort SRAM launcher cases')
