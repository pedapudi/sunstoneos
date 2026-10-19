#!/usr/bin/env python3
"""Execute the actual full-HD AArch32 launcher, stopping before hardware handoff.

The instruction/MMIO model follows checks/h264-launcher.py. No AArch64 startup,
shared cached preparation, GIC or secondary CPU instructions execute here.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm_const import UC_CPU_ARM_CORTEX_A15, UC_ARM_REG_R0, UC_ARM_REG_PC, UC_ARM_REG_CPSR, UC_ARM_REG_SP, UC_ARM_REG_LR
ROOT=Path(__file__).resolve().parents[1]
SESSION,CONTRACT,REQUEST,REPORT=0x41f00000,0x42004000,0x52c00,0x52d00
MODE,CONFIG,FLAG,ALT,RVBAR=0x02050014,0x02050010,0x070501c0,0x070501d4,0x08001004

def digest(data):return hashlib.sha256(data).hexdigest()
def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('image','session','output'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();a.output.mkdir(exist_ok=False)
    spec=importlib.util.spec_from_file_location('h264_prepare',ROOT/'apps/h264/prepare.py');prepare=importlib.util.module_from_spec(spec);spec.loader.exec_module(prepare)
    native=a.image.read_bytes();segments,id_address,identity=prepare.image_parts(a.image,3)
    template=a.session.read_bytes();prepare.session_check(template)
    launcher=a.output/'launcher';launcher.mkdir();prepare.build_launcher(launcher,'/usr/bin/clang-21','/home/sunil/.cache/sunstoneos/host/usr/bin/ld.lld-21',fullhd=True)
    binary=(launcher/'launch.bin').read_bytes()
    with (launcher/'launch.elf').open('rb') as stream:
        elf=ELFFile(stream);symbols={s.name:int(s['st_value']) for s in elf.get_section_by_name('.symtab').iter_symbols()}
    assert symbols['a733_smp_launch']==0x52000 and 0<len(binary)<=0xc00
    cases=[]
    def check(name,expected_reason,mutation=None):
        session=bytearray(template);struct.pack_into('<3Q',session,24,31,37,41);session[48:80]=identity
        # Mutate the unsupported case after the actual producer accepts its template.
        prepare.session_check(session)
        request,contract=prepare.request_bytes(id_address,identity,session,31,37,41)
        contract=bytearray(contract)
        if mutation:
            kind,offset,value=mutation;target=session if kind=='session' else contract
            struct.pack_into('<Q',target,offset,value)
        contract=bytes(contract)
        folder=a.output/name;folder.mkdir()
        for label,data in [('session',session),('request',request),('contract',contract)]: (folder/(label+'.bin')).write_bytes(data)
        cpu=Uc(UC_ARCH_ARM,UC_MODE_ARM);cpu.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_A15)
        for base,size in [(0x50000,0x30000),(0x41000000,0x1200000),(0x02050000,4096),
                (0x07050000,4096),(0x08001000,4096)]:cpu.mem_map(base,size)
        for segment in segments:
            if segment['data']:cpu.mem_write(segment['address'],segment['data'])
        cpu.mem_write(0x52000,binary);cpu.mem_write(REQUEST,request);cpu.mem_write(CONTRACT,contract);cpu.mem_write(SESSION,bytes(session))
        cpu.mem_write(0x52ff0,b'A'*16)
        cpu.reg_write(UC_ARM_REG_CPSR,0xa80f0053)
        saved={UC_ARM_REG_R0+i:0x12340000+i*0x101 for i in range(13)}
        saved.update({UC_ARM_REG_SP:0x70000,UC_ARM_REG_LR:0x60000,UC_ARM_REG_CPSR:cpu.reg_read(UC_ARM_REG_CPSR)})
        for reg,value in saved.items():cpu.reg_write(reg,value)
        system={0xee110f10:0,0xee100fb0:0x81000000,0xee1e0f10:24000000,0xee1c0f50:0}
        mmio={MODE:0,CONFIG:0,FLAG:0,ALT:0,RVBAR:0,RVBAR+4:0};reads=[];writes=[];ticks=[];stop=[]
        def read(uc,access,address,size,value,unused):
            if address<0x02000000 or address>=0x41000000:return
            assert address in mmio and size==4,(name,hex(address),size)
            reads.append(address);uc.mem_write(address,struct.pack('<I',mmio[address]))
        def write(uc,access,address,size,value,unused):
            if address<0x02000000:
                assert REPORT<=address<REPORT+128 or 0x53000<=address<0x54000 or 0x70000-60<=address<0x70000
            elif address>=0x41000000:
                assert CONTRACT<=address<CONTRACT+8 or CONTRACT+80<=address<CONTRACT+112
            else:
                assert address in (RVBAR,RVBAR+4) and size==4
                assert struct.unpack('<Q',uc.mem_read(CONTRACT,8))[0]==prepare.CONTRACT_MAGIC
                writes.append((address,value));mmio[address]=value
        def instruction(uc,address,size,unused):
            if address==0x60000:stop.append('return');uc.emu_stop();return
            word=struct.unpack('<I',uc.mem_read(address,4))[0]
            if word&0xffff0fff in system:
                uc.reg_write(UC_ARM_REG_R0+((word>>12)&15),system[word&0xffff0fff]);uc.reg_write(UC_ARM_REG_PC,address+4)
            elif word&0xfff00fff==0xec500f0e:
                value=100000000+100*len(ticks);ticks.append(value)
                uc.reg_write(UC_ARM_REG_R0+((word>>12)&15),value&0xffffffff)
                uc.reg_write(UC_ARM_REG_R0+((word>>16)&15),value>>32);uc.reg_write(UC_ARM_REG_PC,address+4)
            elif word==0xee0c0f50:
                assert uc.reg_read(UC_ARM_REG_R0)==3 and mmio[RVBAR]==0x41000000 and mmio[RVBAR+4]==0
                stop.append('handoff');uc.emu_stop()
            elif word==0xe320f003:raise AssertionError(name+' unexpectedly parked')
        cpu.hook_add(UC_HOOK_MEM_READ,read);cpu.hook_add(UC_HOOK_MEM_WRITE,write);cpu.hook_add(UC_HOOK_CODE,instruction)
        cpu.emu_start(0x52000,0x52c00,count=20000)
        report=struct.unpack('<16Q',cpu.mem_read(REPORT,128));published=bytes(cpu.mem_read(CONTRACT,288))
        assert report[0]==expected_reason,(name,report[0],stop)
        assert bytes(cpu.mem_read(SESSION,len(session)))==session and bytes(cpu.mem_read(0x52ff0,16))==b'A'*16
        if expected_reason==12:
            assert stop==['handoff'] and writes==[(RVBAR,0x41000000),(RVBAR+4,0)] and len(ticks)==4
            assert struct.unpack_from('<Q',published)[0]==prepare.CONTRACT_MAGIC
            assert struct.unpack_from('<4Q',published,80)==(ticks[0],ticks[0]+24000000,ticks[0]+1440000000,ticks[0]+1440000000)
            assert published[8:80]==contract[8:80] and published[112:]==contract[112:288]
        else:
            assert stop==['return'] and not writes and not ticks and published==contract[:288]
            assert report[1:7]==(0,0,31,37,41,0x4153373352454631) and not any(report[7:])
            for reg,value in saved.items():assert cpu.reg_read(reg)==value,(name,reg)
        cases.append({'case':name,'session_version':struct.unpack_from('<Q',session,600)[0],'reason':report[0],'stop':stop[0],
            'mmio_reads':[hex(x) for x in reads],'mmio_writes':writes,'counter_reads':ticks})
    check('full-HD-version3',12)
    check('VGA-version2-refused',14,('session',600,2))
    check('short-contract-refused',3,('contract',16,224))
    check('changed-session-digest-refused',3,('contract',224,0))
    check('changed-entry-digest-refused',14,('contract',256,0))
    check('changed-NC-range-refused',3,('contract',208,0x41c00000))
    assert a.image.read_bytes()==native
    report={'passed':True,'physical_access':False,'image_sha256':digest(native),'launcher_sha256':digest(binary),
        'cases':cases,'scope':'Actual generated AArch32 full-HD entry/admission/publication/refusal/restoration. System registers, counters and MMIO modeled. Stops before hardware RMR handoff. No AArch64 startup, cached preparation, GIC, secondary CPU or USB execution.'}
    (a.output/'result.json').write_text(json.dumps(report,indent=2)+'\n');print('PASS full-HD actual AArch32 launcher, 288-byte record and profile/digest/range refusals')
if __name__=='__main__':main()
