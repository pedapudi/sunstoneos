#!/usr/bin/env python3
"""Check local-memory compiler metadata, native resources and actual OpenCL source."""
import argparse
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import runpy
import subprocess
import sys
from harness import environment
ROOT=Path(__file__).resolve().parents[1]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build',type=Path,required=True)
    parser.add_argument('--mesa-build',type=Path,required=True)
    parser.add_argument('--mesa-source',type=Path,required=True)
    parser.add_argument('--producer',type=Path,required=True,help='profile-four compiler output with manifest and usc.log')
    parser.add_argument('--control-producer',type=Path,required=True)
    parser.add_argument('--ld',required=True,help='configured AArch64 linker, named ld.lld')
    a=parser.parse_args();out=a.build.resolve();out.mkdir(parents=True,exist_ok=True)
    env={**environment(),'TMPDIR':str(out/'tmp')}
    (out/'tmp').mkdir(exist_ok=True)
    commands=[]
    def run(command,name,cwd=ROOT):
        commands.append(command)
        r=subprocess.run(command,cwd=cwd,env=env,capture_output=True,text=True)
        (out/(name+'.log')).write_text(r.stdout+r.stderr)
        if r.returncode:raise ValueError(name+' failed: '+r.stderr[-2000:])
    spec=importlib.util.spec_from_file_location('gpu_compile',ROOT/'apps/gpu-program/compile.py')
    compiler=importlib.util.module_from_spec(spec);spec.loader.exec_module(compiler)
    manifest=json.loads((ROOT/'apps/queue-gemm/sources.json').read_text())
    # Bind generated descriptor to the real compiler product and unchanged source.
    producer=a.producer
    blob=(producer/'program.bin').read_bytes()
    assert hashlib.sha256((ROOT/'apps/queue-gemm/tiled_gemm.cl').read_bytes()).hexdigest()==manifest['source_sha256']
    assert compiler.emit_queue_program(blob,manifest)==(ROOT/'apps/queue-gemm/program.h').read_text()
    log=(producer/'usc.log').read_text()
    assert '@load_shared ' in log and '@store_shared ' in log and 'cf3[idx0]' in log
    assert '@load_shared_base_ptr' not in log
    control=json.loads((ROOT/'apps/queue-gemm/control-sources.json').read_text())
    assert compiler.emit_queue_program((a.control_producer/'program.bin').read_bytes(),control)==(ROOT/'apps/queue-gemm/control-program.h').read_text()
    assert hashlib.sha256((ROOT/'apps/queue-gemm/direct_gemm.cl').read_bytes()).hexdigest()==control['source_sha256']
    cases=['compiler-direct-common-store','real-descriptor-reproduction']
    for index,value in [(0,253),(1,6),(2,100),(3,1),(4,4),(5,1),(6,1),(7,1),(8,2),(9,1),
                        (10,8),(11,4),(12,2),(13,0),(13,241),(14,1),(15,1),(16,1),(17,1)]:
        bad=copy.deepcopy(manifest);bad['metadata'][index]=value
        try:compiler.emit_queue_program(blob,bad)
        except ValueError:cases.append('metadata-refusal-'+str(index)+'-'+str(value))
        else:raise AssertionError('unsupported metadata accepted')
    inputs=['drivers/gpu/pvr_compute_package.c','drivers/gpu/pvr_compute_job.c','drivers/gpu/pvr_compute_programs.c',
            'drivers/gpu/pvr_compute_context.c','drivers/gpu/pvr_gpu_vm.c','drivers/gpu/pvr_cccb.c','drivers/gpu/pvr_ccb.c']
    cc=['clang-21','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
        '-fno-sanitize-recover=all','-fno-omit-frame-pointer','-I'+str(ROOT/'include')]
    run(cc+[str(ROOT/'checks/gpu-local/resources.c')]+[str(ROOT/p) for p in inputs]+['-o',str(out/'resources')],'resources-build')
    run([str(out/'resources')],'resources')
    # Reuse the pinned upstream PDS reference object and extracted CDM encoder.
    old=Path.home()/'.cache/sunstoneos/evidence/gpu-compute-programs-20261016'
    baseline=Path.home()/'.cache/sunstoneos/evidence/gpu-compute-job-20261016'
    assert hashlib.sha256((old/'mesa-pds.o').read_bytes()).hexdigest()=='233efe758751c3d3a17c1e9ca0df682399be41bfe9d6cc4f5ab096e57d8f9147'
    pins=runpy.run_path(str(ROOT/'checks/pvr-compute-job.py'))['SOURCE_HASHES']
    for name,sha in pins.items():assert hashlib.sha256((a.mesa_source/name).read_bytes()).hexdigest()==sha,name
    flags=json.loads((old/'manifest.json').read_text())['commands'][0]
    flags=flags[:1]+['-I'+str(ROOT/'include')]+flags[1:flags.index('-c')]+['-I'+str(baseline)]
    run(flags+['-DUPSTREAM',str(ROOT/'checks/gpu-local/resources.c')]+[str(ROOT/p) for p in inputs]+
        [str(old/'mesa-pds.o'),str(a.mesa_build/'src/util/libmesa_util.a'),'-Wl,--gc-sections','-pthread','-lm','-o',str(out/'resources-reference')],
        'resources-reference-build',a.mesa_build)
    run([str(out/'resources-reference')],'resources-reference')
    # Execute the unchanged OpenCL shader on 32 host threads. Its __local arrays
    # are real shared globals and every barrier synchronizes all threads.
    run(['clang-21','-x','cl','-cl-std=CL2.0','-O1','-g','-fsanitize=address,undefined',
         '-I'+str(ROOT/'checks/gpu-local'),'-c',str(ROOT/'apps/queue-gemm/tiled_gemm.cl'),'-o',str(out/'kernel.o')],'kernel-build')
    run(cc+[str(ROOT/'checks/gpu-local/kernel-host.c'),str(out/'kernel.o'),'-pthread','-lm','-o',str(out/'kernel-check')],'kernel-host-build')
    run([str(out/'kernel-check')],'kernel-host')
    generated=out/'generated'
    run([sys.executable,str(ROOT/'tools/sunstone-manifest.py'),'emit','--manifest',str(ROOT/'apps/queue-gemm/manifest.json'),'--output',str(generated)],'manifest')
    (generated/'FreeRTOS.h').write_text('#include <stdint.h>\ntypedef uint64_t TickType_t;\n')
    (generated/'task.h').write_text('TickType_t xTaskGetTickCount(void);\nvoid vTaskDelay(TickType_t);\n')
    run(cc+['-I'+str(generated),str(ROOT/'apps/queue-gemm/main.c'),str(ROOT/'checks/gpu-local/application-host.c'),'-o',str(out/'application-check')],'application-build')
    for mode in range(4):run([str(out/'application-check'),str(mode)],'application-'+str(mode),out)
    decoder=runpy.run_path(str(ROOT/'apps/queue-gemm/host.py'))['decode']
    result=decoder((out/'result.bin').read_bytes())
    run(['clang-21','--target=aarch64-none-elf','-mgeneral-regs-only','-ffreestanding','-std=c11','-O2','-Wall','-Wextra','-Werror',
         '-I'+str(ROOT/'include'),'-I'+str(generated),'-c',str(ROOT/'apps/queue-gemm/main.c'),'-o',str(out/'application-aarch64.o')],'aarch64-application')
    run(['clang-21','-x','cl','-cl-std=CL2.0','-O1','-g','-fsanitize=address,undefined',
         '-I'+str(ROOT/'checks/gpu-local'),'-c',str(ROOT/'apps/queue-gemm/direct_gemm.cl'),'-o',str(out/'control-kernel.o')],'control-kernel-build')
    run(cc+['-DSUNSTONE_GPU_GEMM_CONTROL=1',str(ROOT/'checks/gpu-local/kernel-host.c'),str(out/'control-kernel.o'),'-pthread','-lm','-o',str(out/'control-kernel-check')],'control-kernel-host-build')
    run([str(out/'control-kernel-check')],'control-kernel-host')
    run(cc+['-DSUNSTONE_GPU_GEMM_CONTROL=1','-I'+str(generated),str(ROOT/'apps/queue-gemm/main.c'),str(ROOT/'checks/gpu-local/application-host.c'),'-o',str(out/'control-application-check')],'control-application-build')
    for mode in range(4):run([str(out/'control-application-check'),str(mode)],'control-application-'+str(mode),out)
    control_result=decoder((out/'result.bin').read_bytes())
    # Existing zero-local descriptor still reproduces and keeps code bounds.
    run([sys.executable,str(ROOT/'checks/gpu-queue-program.py'),'--build',str(out/'legacy'),'--ld',a.ld],'legacy-descriptor')
    evidence={'schema':'sunstone-gpu-local-host-v1','physical_access':False,'cases':cases,'commands':commands,
              'application_result':result,'control_result':control_result,'scope':'Pinned compiler IR, real Mesa CDM encoder, unchanged OpenCL kernel on host threads, modeled application queue'}
    (out/'result.json').write_text(json.dumps(evidence,indent=2)+'\n')
    print(json.dumps({'cases':len(cases),'application':result,'physical_access':False}))
if __name__=='__main__':main()
