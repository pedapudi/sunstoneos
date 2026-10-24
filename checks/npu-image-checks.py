#!/usr/bin/env python3
"""Build the separately configured NPU images, write their session fixtures and run their checks.

The accelerator build is a configured build of apps/accelerators (its
single-core NPU, all-core NPU and combined images); the root build, when
given, is a configured central build with SUNSTONE_A733_NPU_BOARD=ON. The
runner builds the targets with nice -n 10 and six jobs, converts the reviewed
session fixtures to the image's session layout, and invokes each check, which
keeps its own result record. Fixtures carry software identities only; they
admit no board.
"""
import argparse,hashlib,json,struct,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
EVIDENCE=Path.home()/'.cache/sunstoneos/evidence'
PYTHON=[sys.executable]
# The reviewed fixtures predate the reduced record: version-2 single-core
# NPU pages of 608 bytes and the 9,080-byte combined session.
NPU_FIXTURE=EVIDENCE/'npu-complete-nn-20261019/host/session.bin'
COMBINED_FIXTURE=EVIDENCE/'gpu-npu-composition-20261020/frozen/artifacts/2/session.bin'
COMBINED_CONTRACT=EVIDENCE/'gpu-npu-composition-20261020/frozen/artifacts/0/contract.bin'
OLD_NPU=(608,224,296,520,88,112,160,192)
OLD_COMBINED=(9080,24,608,632,760,792,8288)

def module(name,path):
    import importlib.util
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m

def npu_page(old,layout,version):
    """The reduced record from a 608-byte page: the identities and digests, the
    nine profile words, the fabric record, the PMIC configuration and pins."""
    size,boot,boot_bytes,npu,pmic,pins=layout;o=OLD_NPU
    assert len(old)==o[0] and boot==o[1] and pins-pmic==o[7]-o[6]
    new=bytearray(size);new[:boot]=old[:boot];struct.pack_into('<QQ',new,8,version,size)
    profile=struct.unpack_from('<14Q',old,o[1])
    struct.pack_into('<9Q',new,boot,*profile[3:12])
    new[boot+npu:boot+npu+24]=old[o[1]+o[5]:o[1]+o[5]+24]
    new[boot+pmic:boot+boot_bytes]=old[o[1]+o[6]:o[1]+o[2]]
    return bytes(new)

def combined(old,npu_raw,layout,version):
    size,no,ns,go,ho,po,ps,_=layout;o=OLD_COMBINED
    assert len(old)==o[0] and ps==o[6] and len(npu_raw)==ns
    new=bytearray(size);struct.pack_into('<3Q',new,0,struct.unpack_from('<Q',old,0)[0],version,size)
    new[no:no+ns]=npu_raw;new[go:go+128]=old[o[3]:o[3]+128];new[ho:ho+32]=old[o[4]:o[4]+32];new[po:po+ps]=old[o[5]:o[5]+ps]
    assert new[ho:ho+32]==hashlib.sha256(new[po:po+ps]).digest()
    return bytes(new)

def run(command,log):
    result=subprocess.run(list(map(str,command)),capture_output=True,text=True)
    log.write_text(result.stdout+result.stderr)
    return {'command':list(map(str,command)),'returncode':result.returncode,
            'last_line':(result.stdout.strip().splitlines() or [''])[-1],'log':str(log)}

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--accelerators',type=Path,required=True,help='configured apps/accelerators build')
    p.add_argument('--root',type=Path,help='configured central build with SUNSTONE_A733_NPU_BOARD=ON')
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False);out=a.output;checks=ROOT/'checks'
    builds=[['nice','-n','10','cmake','--build',a.accelerators,'-j','6']]
    if a.root:builds.append(['nice','-n','10','cmake','--build',a.root,'-j','6','--target','sunstone-a733-npu-board'])
    for i,c in enumerate(builds):
        r=run(c,out/f'build-{i}.log');assert r['returncode']==0,r
    images={'single-core':a.accelerators/'a733-npu-convolution-single-core.elf',
            'all-core':a.accelerators/'npu/a733-npu-convolution-supervised.elf',
            'accelerators':a.accelerators/'sunstone-a733-accelerators.elf'}
    if a.root:images['board']=a.root/'sunstone-a733-npu-board.elf'
    prep=module('npu_prepare',ROOT/'apps/npu-convolution/prepare-supervised.py')
    acc=module('acc_prepare',ROOT/'apps/accelerators/prepare.py')
    fixtures=out/'fixtures';fixtures.mkdir()
    layout=prep.image_data(images['single-core'].read_bytes())[3]
    single=npu_page(NPU_FIXTURE.read_bytes(),layout,prep.SINGLE_CORE_VERSION);(fixtures/'npu-session.bin').write_bytes(single)
    clayout=acc.image_data(images['accelerators'].read_bytes())[3]
    inner=npu_page(COMBINED_FIXTURE.read_bytes()[OLD_COMBINED[1]:OLD_COMBINED[1]+OLD_COMBINED[2]],acc.NPU_LAYOUT,prep.SINGLE_CORE_VERSION)
    both=combined(COMBINED_FIXTURE.read_bytes(),inner,clayout,acc.SESSION_VERSION);(fixtures/'accelerator-session.bin').write_bytes(both)
    acc.session_profile(both,clayout)
    runs={}
    for name in ('single-core','accelerators') + (('board',) if a.root else ()):
        runs['clock-reference-'+name]=run(PYTHON+[checks/'npu-application-clock-reference.py','--image',images[name],
            '--output',out/('clock-reference-'+name)],out/f'clock-reference-{name}.log')
    for name,image in images.items():
        runs['stop-native-'+name]=run(PYTHON+[checks/'npu-stop-native.py','--elf',image,'--evidence',out/('stop-native-'+name)],
            out/f'stop-native-{name}.log')
    runs['single-core-retention']=run(PYTHON+[checks/'npu-single-core.py','--image',images['single-core'],
        '--session',fixtures/'npu-session.bin','--output',out/'single-core-retention','--retention-only'],out/'single-core-retention.log')
    runs['accelerator-providers']=run(PYTHON+[checks/'accelerator-providers.py','--image',images['accelerators'],
        '--session',fixtures/'accelerator-session.bin','--contract',COMBINED_CONTRACT,'--output',out/'accelerator-providers'],
        out/'accelerator-providers.log')
    if a.root:
        runs['board-providers']=run(PYTHON+[checks/'npu-board-providers.py','--image',images['board'],
            '--output',out/'board-providers'],out/'board-providers.log')
    record={'images':{k:hashlib.sha256(v.read_bytes()).hexdigest() for k,v in images.items()},
            'fixtures':{f.name:hashlib.sha256(f.read_bytes()).hexdigest() for f in fixtures.iterdir()},
            'sources':{str(s):hashlib.sha256(s.read_bytes()).hexdigest() for s in (NPU_FIXTURE,COMBINED_FIXTURE,COMBINED_CONTRACT)},
            'runs':runs}
    (out/'runs.json').write_text(json.dumps(record,indent=2)+'\n')
    failed=[k for k,v in runs.items() if v['returncode']]
    for k,v in runs.items():print(('FAIL ' if v['returncode'] else 'ok   ')+k+': '+v['last_line'])
    if failed:raise SystemExit('Failed: '+', '.join(failed))
    print('PASS',len(runs),'NPU image checks')
if __name__=='__main__':main()
