#!/usr/bin/env python3
"""One focused USB cold-platform batch; no board or USB application traffic."""
import argparse
import ctypes
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_LR, UC_ARM64_REG_SP
if not __debug__:raise SystemExit('checks require assertions; unset PYTHONOPTIMIZE and do not pass -O')


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--source-cache',type=Path,required=True)
    p.add_argument('--clang',default='/usr/lib/llvm-21/bin/clang')
    p.add_argument('--lld',required=True)
    p.add_argument('--reuse-passed',type=Path,help='Reuse matching completed source/host/object evidence; execute native adapter only')
    a=p.parse_args();root=Path(__file__).resolve().parents[1]
    a.output.mkdir(parents=True,exist_ok=False);snapshot=a.output/'source';snapshot.mkdir()
    paths=['include/a733_usb_platform.h','include/a733_clock_reference.h','include/a733_usb_platform_native.h','include/a733_clock_leases.h',
           'include/a733_power_authority.h','include/a733_range.h',
           'drivers/usb/a733_usb_platform.c','drivers/usb/a733_usb_platform_native.c',
           'drivers/clock/a733_clock_leases.c','checks/a733-usb-platform.c','checks/a733-usb-rom-handoff.c','checks/a733-usb-platform.py']
    hashes={}
    for path in paths:
        dest=snapshot/path;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(root/path,dest)
        hashes[path]=hashlib.sha256(dest.read_bytes()).hexdigest()
    (a.output/'inputs.json').write_text(json.dumps(hashes,indent=2)+'\n')
    if a.reuse_passed:
        prior=json.loads((a.reuse_passed/'inputs.json').read_text())
        assert all(prior[name]==digest for name,digest in hashes.items() if name!='checks/a733-usb-platform.py')
        assert (a.reuse_passed/'host-result.log').read_text().startswith('PASS:')
        for name in ['published-phy.c','published-phy.so','published-usbc_dev.c','source-comparison.json',
                     'platform-check','host-build.log','host-result.log','a733_usb_platform.o','a733_clock_leases.o']:
            shutil.copyfile(a.reuse_passed/name,a.output/name)
    commands=[]
    def run(command,label):
        commands.append(command);(a.output/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
        result=subprocess.run(command,capture_output=True,text=True)
        (a.output/(label+'.log')).write_text(result.stdout+result.stderr)
        if result.returncode:raise RuntimeError(label+' failed; retained log at '+str(a.output))
    # Preserve exact published function and license, rather than reproducing its
    # tuning formula in another implementation-derived assertion.
    src=a.source_cache/'bsp/drivers/usb/sunxi_usb/usbc/usbc_dev.c'
    original=src.read_text();start=original.index('void USBC_Phyx_Write(');end=original.index('\nEXPORT_SYMBOL(USBC_Phyx_Write)',start)
    shim=original[:original.index('#include')]+'''\n#include <stdint.h>
#include <stddef.h>
typedef uint32_t __u32; typedef void *__hdle;
typedef struct {void *base_addr;} __usbc_otg_t;
#define USBC_REG_o_PHYTUNE 0x418
static uint32_t cell;
#define USBC_Readl(a) ((void)(a),cell)
#define USBC_Writel(v,a) ((void)(a),cell=(v))
'''+original[start:end]+'''\nuint32_t reference(uint32_t old) {
__usbc_otg_t h={0};cell=old;USBC_Phyx_Write(&h,0x188);return cell;}
'''
    (a.output/'published-phy.c').write_text(shim)
    shutil.copyfile(src,a.output/'published-usbc_dev.c')
    if not a.reuse_passed:
        run([a.clang,'-shared','-fPIC','-O2',str(a.output/'published-phy.c'),'-o',str(a.output/'published-phy.so')],'reference-build')
        lib=ctypes.CDLL(str(a.output/'published-phy.so'));lib.reference.argtypes=[ctypes.c_uint32];lib.reference.restype=ctypes.c_uint32
        for old in [0,0xffffffff,0xaaaaaaaa,0x55555555]:
            assert lib.reference(old)==(old&~0xf3f)|0x806
    references={'usbc_dev.c':hashlib.sha256(src.read_bytes()).hexdigest()}
    for file,pattern in [('sunxi_usb/udc/sunxi_udc_dma.h',r'CONFIG_ARCH_SUN60IW2\)\s*\n#define DMA_CHAN_TOTAL\s+\(16\)'),
                         ('sunxi_usb/usbc/usbc.c',r'void USBC_SelectPhyToDevice[\s\S]*?base_addr \+ 0x420[\s\S]*?reg_value \|= \(0x01\)'),
                         ('sunxi_usb/include/sunxi_usb_bsp.h',r'USBC_REG_o_PHYTUNE\s+0x0418')]:
        source=a.source_cache/'bsp/drivers/usb'/file
        assert re.search(pattern,source.read_text()),file
        references[file]=hashlib.sha256(source.read_bytes()).hexdigest()
    if a.reuse_passed:
        assert json.loads((a.output/'source-comparison.json').read_text())['inputs']==references
    else:
        (a.output/'source-comparison.json').write_text(json.dumps({'published_tuning_cases':4,'inputs':references},indent=2)+'\n')
    flags=[a.clang,'-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer','-I'+str(snapshot/'include')]
    if not a.reuse_passed:
        run(flags+[str(snapshot/path) for path in ['checks/a733-usb-platform.c','drivers/usb/a733_usb_platform.c','drivers/clock/a733_clock_leases.c']]+['-o',str(a.output/'platform-check')],'host-build')
        run([str(a.output/'platform-check')],'host-result')
        run(flags+['-I'+str(snapshot/'checks')]+[str(snapshot/path) for path in ['checks/a733-usb-rom-handoff.c','drivers/usb/a733_usb_platform.c','drivers/clock/a733_clock_leases.c']]+['-o',str(a.output/'rom-check')],'rom-build')
        run([str(a.output/'rom-check')],'rom-result')
    native=[a.clang,'--target=aarch64-none-elf','-march=armv8-a','-mstrict-align','-mgeneral-regs-only','-O2','-g','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin','-I'+str(snapshot/'include')]
    if not a.reuse_passed:
        for path in ['drivers/usb/a733_usb_platform.c','drivers/clock/a733_clock_leases.c']:
            run(native+['-c',str(snapshot/path),'-o',str(a.output/(Path(path).stem+'.o'))],Path(path).stem+'-native')
    elfpath=a.output/'native.elf'
    run(native+[str(snapshot/'drivers/usb/a733_usb_platform_native.c'),'-nostdlib','--ld-path='+a.lld,'-Wl,-Ttext,0x400000','-Wl,-e,a733_usb_platform_native_read','-o',str(elfpath)],'native-link')
    u=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
    for addr,size in [(0x400000,0x10000),(0x500000,0x1000),(0x600000,0x10000),(0x700000,0x1000),
                      (0x02002000,0x2000),(0x04100000,0x1000),(0x07010000,0x1000),(0x07090000,0x1000)]:u.mem_map(addr,size)
    with elfpath.open('rb') as f:
        elf=ELFFile(f);symbols={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
        for seg in elf.iter_segments():
            if seg['p_type']=='PT_LOAD' and seg['p_filesz']:
                addr=seg['p_vaddr'];lo=addr&~4095;size=((addr+seg['p_filesz']+4095)&~4095)-lo
                try:u.mem_map(lo,size)
                except Exception:pass
                u.mem_write(addr,seg.data())
    writes=[]
    u.hook_add(UC_HOOK_MEM_WRITE,lambda uc,access,address,size,value,data:writes.append((address,size,value)))
    def call(name,address,value):
        writes.clear();u.reg_write(UC_ARM64_REG_X0,0);u.reg_write(UC_ARM64_REG_X1,address)
        u.reg_write(UC_ARM64_REG_X2,value);u.reg_write(UC_ARM64_REG_SP,0x60fff0);u.reg_write(UC_ARM64_REG_LR,0x700000)
        u.emu_start(symbols[name],0x700000,count=2000);return u.reg_read(UC_ARM64_REG_X0)
    def writecase(address,old,value,ok):
        u.mem_write(address,old.to_bytes(4,'little'))
        assert call('a733_usb_platform_native_write',address,value)==ok
        assert writes==([(address,1,value&255) if address==0x04100040 else (address,4,value)] if ok else [])
    writecase(0x02003340,0x20000005,0x21000005,1)
    writecase(0x02003340,0xa0000005,0xa1000005,0)
    writecase(0x02003304,0x20000000,0x21000100,1)
    writecase(0x02003304,0x21000100,0x20000000,0)
    writecase(0x02003304,0x21000100,0x20000100,1)
    writecase(0x04100040,0x01400360,0x01400320,1)
    writecase(0x04100040,0x01400020,0x01400060,0)
    writecase(0x04100048,0x00040003,0,1)
    writecase(0x04100050,0xface00ff,0xface0000,1)
    writecase(0x04100500,0xfaceffff,0xface0000,1)
    writecase(0x04100400,0x8000c07f,0x8000c000,1)
    writecase(0x04100400,0x8000307f,0x80003000,1)
    writecase(0x04100400,0x8000c070,0x80003000,0)
    writecase(0x04100410,0x10000088,0x10000020,1)
    writecase(0x04100418,0xface7040,0xface7846,1)
    writecase(0x04100400,0x80000070,0x8000f000,1)
    writecase(0x04100400,0x80000070,0x8000f070,0)
    for address in [0x020025c0,0x020025a4,0x04100040,0x04100540]:writecase(address,0,1,0)
    assert call('a733_usb_platform_native_write',0x020500b0,1)==0 and not writes
    for channel in range(16):
        address=0x04100540+channel*16;u.mem_write(address,(channel+5).to_bytes(4,'little'))
        assert call('a733_usb_platform_native_read',address,0x500000)==1
        assert int.from_bytes(u.mem_read(0x500000,4),'little')==channel+5
    assert call('a733_usb_platform_native_read',0x04100640,0x500000)==0
    report={'result':'PASS','scope':['published PHY tune comparison','sanitized platform ownership/deadline/core refusal composition',
              'native MMIO allowlist and field preservation'], 'native_elf':hashlib.sha256(elfpath.read_bytes()).hexdigest(),
            'physical_access':False,'reused_evidence':str(a.reuse_passed) if a.reuse_passed else None,
            'limitations':'Register/connector behavior is modeled; this does not qualify physical clocks, supplies, USB or recovery.'}
    (a.output/'result.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))


if __name__=='__main__':main()
