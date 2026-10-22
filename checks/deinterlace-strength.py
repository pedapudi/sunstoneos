#!/usr/bin/env python3
"""Check selectable DI301 TNR strength from plan generation to host capture labels.

Host plan generation runs the pinned DI301 configuration code under ASan/UBSan.
The linked AArch64 image must generate identical register images. Host
preparation must mirror admission and refuse strength on an image older than
revision 4. A rebuilt SRAM launcher runs in the shared AArch32 model inside an
earlier TNR bundle. Device effects, filtering arithmetic and physical execution
are outside every case.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR,UC_ARM64_REG_PSTATE
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'checks'))
import harness
import sram_launcher
PLAN_BYTES=0x5000
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value


def host_plans(args):
    out=args.output/'host';out.mkdir()
    sources=[ROOT/'checks/deinterlace-strength-plan.c',ROOT/'drivers/deinterlace/a733_deinterlace.c',
        args.generated/'di301_alg.c',args.generated/'di-formats.c']
    commands=[[args.cc,'-std=gnu11','-O1','-g','-w','-fsanitize=address,undefined','-fno-sanitize-recover=all',
        '-I'+str(ROOT/'include'),'-I'+str(ROOT/'drivers/deinterlace'),'-I'+str(args.generated),*map(str,sources),'-o',str(out/'plan')],
        [str(out/'plan'),str(out/'plans.bin')]]
    for index,command in enumerate(commands):
        run=subprocess.run(command,capture_output=True,text=True);(out/f'step{index}.log').write_text(run.stdout+run.stderr);run.check_returncode()
    data=(out/'plans.bin').read_bytes();assert len(data)==4*PLAN_BYTES
    return [data[i*PLAN_BYTES:(i+1)*PLAN_BYTES] for i in range(4)],(out/'step1.log').read_text().strip()


def linked_plans(args,base,plans):
    image=harness.Image(args.image,layout_names=('a733_deinterlace_session',))
    layout=image.layouts['a733_deinterlace_session'];assert layout['strength']==884
    cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);cpu.reg_write(UC_ARM64_REG_PSTATE,13)
    cpu.mem_map(0x41000000,0x1200000);cpu.mem_map(0x50000000,0x200000)
    for address,data in image.parts:
        if data:cpu.mem_write(address,data)
    session,plan,stop=0x41f00000,0x41f10000,0x501ff000
    def call(name,*values):
        for index,value in enumerate(values):cpu.reg_write(UC_ARM64_REG_X0+index,value)
        cpu.reg_write(UC_ARM64_REG_SP,0x501fe000);cpu.reg_write(UC_ARM64_REG_LR,stop)
        cpu.emu_start(image.address(name),stop,count=20000000)
        assert cpu.reg_read(UC_ARM64_REG_PC)==stop,(name,hex(cpu.reg_read(UC_ARM64_REG_PC)))
        return cpu.reg_read(UC_ARM64_REG_X0)
    results=[]
    for strength in range(6):
        raw=bytearray(base);struct.pack_into('<I',raw,884,strength)
        cpu.mem_write(session,bytes(raw));cpu.mem_write(plan,bytes(PLAN_BYTES+16))
        valid=strength<4
        assert call('a733_deinterlace_session_valid',session)==valid,strength
        assert call('a733_deinterlace_commands',plan,session)==valid,strength
        words=bytes(cpu.mem_read(plan,PLAN_BYTES))
        assert words==(plans[strength] if valid else bytes(PLAN_BYTES)),strength
        results.append(dict(strength=strength,admitted=valid,strength_word=hex(struct.unpack_from('<I',words,0x4000)[0])))
    return results


def host_predicates(args,base):
    prep=module('deinterlace_strength_preparation',ROOT/'apps/deinterlace/prepare.py')
    capture=module('deinterlace_strength_capture',ROOT/'apps/deinterlace/capture.py')
    image=args.image.read_bytes();previous=args.previous_image.read_bytes()
    results=[]
    for mode in (1,2,3):
        for strength in (0,1,2,3,4):
            geometry=(640,480,640,640,0x3231564e,mode,1,strength);want=strength==0 or (mode==3 and strength<=3)
            try:prep.geometry_check(geometry)
            except ValueError:accepted=False
            else:accepted=True
            assert accepted==want,(mode,strength);results.append(dict(mode=mode,strength=strength,accepted=accepted))
    labels=[]
    for strength in range(4):
        raw=bytearray(base);struct.pack_into('<I',raw,884,strength);raw=bytes(raw);prep.session_check(raw)
        prep.embedded_reference(image,raw)
        if strength:
            try:prep.embedded_reference(previous,raw)
            except ValueError:pass
            else:raise AssertionError('Revision3 image accepted a selected strength.')
        else:prep.embedded_reference(previous,raw)
        # A pending response carries the geometry words and no payload.
        epoch,generation=struct.unpack_from('<2Q',raw,24)
        frame=struct.pack('<4I2Q',0x52494453,1,0,0,epoch,generation)+raw[768:800]+hashlib.sha256(raw).digest()+bytes(32)
        frame+=raw[856:888]+bytes(32)
        decoded=capture.decode(frame,raw);assert decoded['format']=='YUV420'
        labels.append(decoded['mode'])
    assert labels==['TNR-FIXED','TNR-INITIAL-LOW','TNR-INITIAL-MIDDLE','TNR-INITIAL-HIGH']
    return results,labels


def launcher(args):
    bundle=args.output/'launcher-bundle';bundle.mkdir()
    for name in ('image.elf','session.bin','contract.bin','request.bin'):shutil.copy2(args.bundle/name,bundle/name)
    shutil.copy2(args.launcher,bundle/'launch.bin')
    template=(bundle/'session.bin').read_bytes();assert struct.unpack_from('<2I',template,876)==(3,1)
    cases=[('strength-%d-handoff'%strength,12,(884,struct.pack('<I',strength)),None) for strength in (0,1,2,3)]
    cases+=[('strength-out-of-range',14,(884,struct.pack('<I',4)),None),
        ('strength-outside-tnr',14,(876,struct.pack('<3I',2,1,1)),None)]
    return sram_launcher.check_bundle(bundle,args.output/'launcher',(0x4153373344494131,1,888),(0x4153373344494331,288),20,cases,
        'Rebuilt deinterlacer SRAM launcher in an earlier TNR bundle; strength words patched. System registers, counter and MMIO are modeled.',
        'PASS {count} rebuilt deinterlacer SRAM launcher cases')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--generated',type=Path,required=True,help='Adapted DI301 sources from the application build')
    parser.add_argument('--image',type=Path,required=True,help='The revision4 deinterlace ELF')
    parser.add_argument('--previous-image',type=Path,required=True,help='An accepted revision3 deinterlace ELF')
    parser.add_argument('--bundle',type=Path,required=True,help='An earlier prepared TNR bundle; read only')
    parser.add_argument('--launcher',type=Path,required=True,help='The rebuilt launch.bin')
    parser.add_argument('--cc',default='/usr/bin/clang-21')
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    base=(args.bundle/'session.bin').read_bytes();assert len(base)==888 and struct.unpack_from('<I',base,884)[0]==0
    plans,summary=host_plans(args)
    linked=linked_plans(args,base,plans)
    predicates,labels=host_predicates(args,base)
    launch=launcher(args)
    result=dict(passed=True,physical_access=False,scope=__doc__,host=summary,linked=linked,host_predicates=predicates,
        capture_labels=labels,launcher_cases=[case['case'] for case in launch['cases']],
        image_sha256=hashlib.sha256(args.image.read_bytes()).hexdigest(),
        previous_image_sha256=hashlib.sha256(args.previous_image.read_bytes()).hexdigest(),
        launcher_sha256=launch['launcher_sha256'])
    (args.output/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(summary);print('PASS',len(linked),'linked plan cases,',len(predicates),'host predicate cases,',len(launch['cases']),'launcher cases')


if __name__=='__main__':main()
