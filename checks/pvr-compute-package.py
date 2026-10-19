#!/usr/bin/env python3
"""Check two real compiler products, native construction and copied model output."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import runpy
import struct
import subprocess

ROOT=Path(__file__).resolve().parents[1]
def main():
    p=argparse.ArgumentParser(description=__doc__)
    for x in ('evidence','mesa-source','mesa-build'):p.add_argument('--'+x,type=Path,required=True)
    a=p.parse_args();out=a.evidence.resolve()
    spec=importlib.util.spec_from_file_location('package_compile',ROOT/'apps/gpu-program/compile.py')
    tool=importlib.util.module_from_spec(spec);spec.loader.exec_module(tool)
    def run(cmd,name,cwd=ROOT,env=None):
        r=subprocess.run(cmd,cwd=cwd,env=env,capture_output=True,text=True)
        (out/(name+'.log')).write_text(r.stdout+r.stderr)
        if r.returncode:raise ValueError(name+': '+r.stderr[-2000:])
        return cmd
    inputs=['drivers/gpu/pvr_compute_package.c','drivers/gpu/pvr_compute_job.c','drivers/gpu/pvr_compute_programs.c',
      'drivers/gpu/pvr_compute_context.c','drivers/gpu/pvr_gpu_vm.c','drivers/gpu/pvr_cccb.c','drivers/gpu/pvr_ccb.c']
    cc=['clang-21','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(ROOT/'include')]
    commands=[]
    commands.append(run(cc+[str(ROOT/'checks/pvr-compute-package.c')]+[str(ROOT/x) for x in inputs]+['-o',str(out/'package-native')],'native-build'))
    # The authoritative Mesa reference object and flags retain their existing pin.
    old=Path.home()/'.cache/sunstoneos/evidence/gpu-compute-programs-20261016'
    if hashlib.sha256((old/'mesa-pds.o').read_bytes()).hexdigest()!='233efe758751c3d3a17c1e9ca0df682399be41bfe9d6cc4f5ab096e57d8f9147':raise ValueError('PDS reference changed')
    baseline=Path.home()/'.cache/sunstoneos/evidence/gpu-compute-job-20261016'
    pins=runpy.run_path(str(ROOT/'checks/pvr-compute-job.py'))['SOURCE_HASHES']
    for name,sha in pins.items():
        if hashlib.sha256((a.mesa_source/name).read_bytes()).hexdigest()!=sha:raise ValueError('Mesa source changed: '+name)
    flags=json.loads((old/'manifest.json').read_text())['commands'][0]
    flags=flags[:1]+['-I'+str(ROOT/'include')]+flags[1:flags.index('-c')]+['-I'+str(baseline)]
    commands.append(run(flags+['-DUPSTREAM',str(ROOT/'checks/pvr-compute-package.c')]+[str(ROOT/x) for x in inputs]+
      [str(old/'mesa-pds.o'),str(a.mesa_build/'src/util/libmesa_util.a'),'-Wl,--gc-sections','-pthread','-lm','-o',str(out/'package-reference')],'reference-build',a.mesa_build))
    results=[]
    for kernel in ('affine','add'):
        directory=out/kernel
        for count in (1,37,127):
            if kernel=='affine':
                words=[count,17,0xfffffff0]+[i*31 for i in range(count)]
                expected=[(x*17+0xfffffff0)&0xffffffff for x in words[3:]]
                header,stride=12,4
            else:
                left=[i*31 for i in range(count)];right=[0xffffffff-i for i in range(count)]
                words=[count]+left+right;expected=[(x+y)&0xffffffff for x,y in zip(left,right)]
                header,stride=4,8
            path=directory/f'input-{count}.bin';path.write_bytes(struct.pack('<'+'I'*len(words),*words))
            expect=directory/f'expected-{count}.bin';expect.write_bytes(struct.pack('<'+'I'*count,*expected))
            raw=tool.package(directory/'program.bin',directory/'manifest.json',path,count*4,header,stride,4)
            pkg=directory/f'package-{count}.bin';pkg.write_bytes(raw)
            for binary in ('native','reference'):
                commands.append(run([str(out/('package-'+binary)),str(pkg),str(expect)],f'{kernel}-{count}-{binary}'))
            results.append({'kernel':kernel,'count':count,'package_sha256':hashlib.sha256(raw).hexdigest(),
              'expected_sha256':hashlib.sha256(expect.read_bytes()).hexdigest()})
    files=[ROOT/x for x in inputs]+[ROOT/'checks/pvr-compute-package.c',ROOT/'checks/pvr-compute-package.py',ROOT/'include/pvr_compute_package.h',ROOT/'include/pvr_compute_job.h']
    manifest={'scope':'Real Mesa code/PDS/CDM bytes; modeled firmware timelines/numerical writes, no USC execution',
      'commands':commands,'cases':results,'inputs':{str(x):hashlib.sha256(x.read_bytes()).hexdigest() for x in files}}
    (out/'package-check.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print('PASS six package cases: two compiler-produced programs, count bounds, copied modeled output and upstream PDS/CDM bytes')
if __name__=='__main__':main()
