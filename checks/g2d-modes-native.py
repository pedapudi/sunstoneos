#!/usr/bin/env python3
"""Execute the linked AArch64 G2D session validator and RCQ builder on every exported geometry case.

Each case patches the extension and geometry words of an accepted earlier
session. The actual a733_g2d_session_valid must give the reference verdict, and
the actual a733_g2d_commands must emit the host reference bytes for every
admitted case and nothing for a refusal. Only these two functions execute; no
provider, allocation, device, USB or physical execution is involved.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR,UC_ARM64_REG_PSTATE
sys.path.insert(0,str(Path(__file__).resolve().parent))
import harness
SESSION,BUFFER,STOP=0x41f00000,0x41f10000,0x501ff000


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image',type=Path,required=True)
    parser.add_argument('--session',type=Path,required=True,help='An accepted earlier G2D session; read only')
    parser.add_argument('--reference',type=Path,required=True,help='Output directory of g2d-modes-reference.py')
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    image=harness.Image(args.image,layout_names=('a733_g2d_session',))
    layout=image.layouts['a733_g2d_session']
    assert (layout['sw'],layout['output_format'],layout['reserved'],layout['width'])==(800,820,824,856)
    base=args.session.read_bytes();assert len(base)==904
    expected=args.reference/'commands.bin';emitted=expected.read_bytes()
    cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);cpu.reg_write(UC_ARM64_REG_PSTATE,13)
    cpu.mem_map(0x41000000,0x1200000);cpu.mem_map(0x50000000,0x200000)
    for address,data in image.parts:
        if data:cpu.mem_write(address,data)
    def call(name,*values):
        for index,value in enumerate(values):cpu.reg_write(UC_ARM64_REG_X0+index,value)
        cpu.reg_write(UC_ARM64_REG_SP,0x501fe000);cpu.reg_write(UC_ARM64_REG_LR,STOP)
        cpu.emu_start(image.address(name),STOP,count=2000000)
        assert cpu.reg_read(UC_ARM64_REG_PC)==STOP,(name,hex(cpu.reg_read(UC_ARM64_REG_PC)))
        return cpu.reg_read(UC_ARM64_REG_X0)
    results=[];admitted=0
    for line in (args.reference/'cases.txt').read_text().splitlines():
        name,valid,*words=line.split();words=list(map(int,words));valid=valid=='1'
        session=bytearray(base)
        struct.pack_into('<12I',session,856,*words[:12]);struct.pack_into('<8I',session,800,*words[12:])
        cpu.mem_write(SESSION,bytes(session));cpu.mem_write(BUFFER,b'\xa5'*4096)
        assert call('a733_g2d_session_valid',SESSION)==valid,name
        made=call('a733_g2d_commands',BUFFER,4096,SESSION)
        commands=bytes(cpu.mem_read(BUFFER,4096))
        if valid:
            assert made==1 and commands==emitted[admitted*4096:(admitted+1)*4096],name;admitted+=1
        else:assert made==0 and commands==b'\xa5'*4096,name
        results.append(dict(case=name,valid=valid,commands_sha256=hashlib.sha256(commands).hexdigest() if valid else None))
    assert admitted*4096==len(emitted)
    summary=dict(passed=True,physical_access=False,scope=__doc__,
        image_sha256=hashlib.sha256(args.image.read_bytes()).hexdigest(),
        base_session_sha256=hashlib.sha256(base).hexdigest(),
        reference_commands_sha256=hashlib.sha256(emitted).hexdigest(),admitted=admitted,cases=results)
    (args.output/'result.json').write_text(json.dumps(summary,indent=2)+'\n')
    print('PASS',len(results),'linked G2D validator cases;',admitted,'RCQ images match the host reference')


if __name__=='__main__':main()
