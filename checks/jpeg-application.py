#!/usr/bin/env python3
"""Build one fixed native JPEG image and check its new integration contracts."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
sys.dont_write_bytecode=True
import unicorn
import elftools
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_INTR
from unicorn import arm64_const as registers
from unicorn.arm64_const import UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_X0
ROOT = Path(__file__).resolve().parents[1]
def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()

import importlib.util
_snapshot_spec=importlib.util.spec_from_file_location('jpeg_evidence_snapshot',ROOT/'tools/evidence-snapshot.py')
_snapshot=importlib.util.module_from_spec(_snapshot_spec);_snapshot_spec.loader.exec_module(_snapshot)
collect_dependencies=_snapshot.collect_dependencies

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--freertos', type=Path, required=True)
    parser.add_argument('--tinyusb', type=Path, required=True)
    parser.add_argument('--tcp', type=Path, required=True)
    parser.add_argument('--rsn', type=Path, required=True)
    parser.add_argument('--headers', type=Path, required=True)
    parser.add_argument('--fixture',type=Path,required=True)
    parser.add_argument('--ffmpeg',type=Path,default=Path('/usr/bin/ffmpeg'))
    parser.add_argument('--resume', action='store_true')
    a = parser.parse_args()
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=a.resume)
    source = out/'source'
    if not a.resume:
        names = subprocess.check_output(['git','ls-files','--cached','--others','--exclude-standard'], cwd=ROOT, text=True).splitlines()
        for name in names:
            p = ROOT/name
            if p.is_file():
                target = source/name
                target.parent.mkdir(parents=True,exist_ok=True)
                shutil.copy2(p,target)
        shutil.copytree(a.freertos,out/'dependencies/freertos',ignore=shutil.ignore_patterns('.git'))
        shutil.copytree(a.tinyusb/'src',out/'dependencies/tinyusb/src')
        shutil.copytree(a.tcp,out/'dependencies/tcp',ignore=shutil.ignore_patterns('.git'))
        shutil.copytree(a.rsn,out/'dependencies'/a.rsn.name,ignore=shutil.ignore_patterns('.git'))
        shutil.copy2(a.tcp.parent/'FreeRTOS-Plus-TCP-b86ad086.tar.gz',out/'dependencies/FreeRTOS-Plus-TCP-b86ad086.tar.gz')
        shutil.copy2(a.rsn.parent/(a.rsn.name+'.tar.gz'),out/'dependencies'/(a.rsn.name+'.tar.gz'))
        shutil.copytree(a.headers,out/'dependencies/headers')
        shutil.copy2(a.fixture,out/'fixture-source.jpg')
        import importlib.util
        spec=importlib.util.spec_from_file_location('jpeg_fixture',source/'apps/jpeg/fixture.py')
        fixture=importlib.util.module_from_spec(spec);spec.loader.exec_module(fixture)
        (out/'fixture.jpg').write_bytes(fixture.build((out/'fixture-source.jpg').read_bytes()))
        paths = [p for directory in (source,out/'dependencies') for p in directory.rglob('*') if p.is_file()]+[out/'fixture-source.jpg',out/'fixture.jpg']
        (out/'inputs.json').write_text(json.dumps({str(p.relative_to(out)):sha(p) for p in paths},indent=2)+'\n')
    else:
        for name,digest in json.loads((out/'inputs.json').read_text()).items():
            assert sha(out/name)==digest, f'Changed preserved input: {name}'
        for p in source.rglob('*'):
            current = ROOT/p.relative_to(source)
            if p.suffix!=".pyc" and p.is_file() and current.is_file():
                assert sha(p)==sha(current), f'Changed source: {current}; select a new output'
    compiler = Path(shutil.which('clang-21')).resolve()
    linker = Path.home()/'.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld'
    tool_paths=[a.ffmpeg.resolve(),compiler,linker.resolve(),Path(sys.executable).resolve(),Path(unicorn.__file__),Path(elftools.__file__)]
    tool_paths += [p for p in Path(unicorn.__file__).parent.rglob('*.so*') if p.is_file()]
    tools = {str(p):sha(p) for p in tool_paths}
    if a.resume:
        assert json.loads((out/'tools.json').read_text())==tools
    else:
        (out/'tools.json').write_text(json.dumps(tools,indent=2)+'\n')
    steps_path = out/'steps.json'
    steps = json.loads(steps_path.read_text()) if steps_path.exists() else {}
    def run(label,command):
        command = list(map(str,command))
        if steps.get(label,{}).get('exit')==0:
            assert steps[label]['command']==command
            return
        result = subprocess.run(command,capture_output=True,text=True)
        (out/(label+'.log')).write_text(result.stdout+result.stderr)
        steps[label] = {'command':command,'exit':result.returncode}
        steps_path.write_text(json.dumps(steps,indent=2)+'\n')
        result.check_returncode()
    host=[compiler,'-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-DSUNSTONE_BOARD_SUPERVISED=1','-I'+str(source/'include'),'-I'+str(source/'checks'),'-I'+str(source/'platform/a733-smp-cached')]
    jobs={
        'admission':['apps/jpeg/admission.c','drivers/storage/storage_digest.c','checks/jpeg-application-admission.c'],
        'retained':['drivers/video/a733_video_jpeg.c','drivers/video/a733_video_jpeg_run.c','drivers/video/a733_video_platform.c','drivers/video/a733_video_unit.c','drivers/clock/a733_clock_leases.c','drivers/gpu/a733_gpu_resources.c','checks/jpeg-application-retained.c'],
        'iommu':['drivers/iommu/a733_iommu.c','checks/jpeg-application-iommu.c']}
    compile_failures=[]
    for name,files in jobs.items():
        try:run(name+'-build',host+[source/f for f in files]+['-o',out/name])
        except subprocess.CalledProcessError:compile_failures.append(name+'-build')
    if compile_failures:
        raise RuntimeError('JPEG fixture compilation failed: '+', '.join(compile_failures)+'; inspect individual logs and steps.json')
    toolchain = out/'toolchain.cmake'
    if not toolchain.exists():
        toolchain.write_text(f'set(SUNSTONE_LLD "{linker}" CACHE FILEPATH "")\ninclude("{source}/cmake/aarch64-clang.cmake")\nset(CMAKE_C_COMPILER "{compiler}")\nset(CMAKE_ASM_COMPILER "{compiler}")\n')
    build = out/'build'
    assert sha(out/'fixture.jpg')=='3689fc87a6594a114026b65c2e104589ae3a872188bce0cb12ae08ab92ede398'
    run('reference',[a.ffmpeg,'-nostdin','-v','error','-threads','1','-bitexact','-i',out/'fixture.jpg','-pix_fmt','yuvj420p','-f','rawvideo',out/'reference.yuv'])
    assert (out/'reference.yuv').read_bytes()==bytes([128])*2304
    run('configure',['cmake','-S',source,'-B',build,f'-DCMAKE_TOOLCHAIN_FILE={toolchain}',
        f'-DFREERTOS_DIR={out}/dependencies/freertos',f'-DTINYUSB_DIR={out}/dependencies/tinyusb',
        f'-DSUNSTONE_TCP_DIR={out}/dependencies/tcp',f'-DSUNSTONE_RSN_DIR={out}/dependencies/{a.rsn.name}',
        f'-DSUNSTONE_NETWORK_C_HEADERS={out}/dependencies/headers','-DCMAKE_EXPORT_COMPILE_COMMANDS=ON','-DSUNSTONE_BOARD_SUPERVISED=ON',f'-DSUNSTONE_JPEG_FIXTURE={out}/fixture.jpg',f'-DSUNSTONE_JPEG_REFERENCE={out}/reference.yuv'])
    run('build',['cmake','--build',build,'--target','sunstone-a733-jpeg','-j4'])
    image = build/'sunstone-a733-jpeg.elf'
    dependencies,actual_commands,records=collect_dependencies(build,'sunstone-a733-jpeg')
    (out/'compiled-commands.json').write_text(json.dumps(actual_commands,indent=2)+'\n')
    (out/'dependency-records.json').write_text(json.dumps(records,indent=2)+'\n')
    for name in dependencies:
        p=Path(name)
        if not p.is_relative_to(out):
            saved=out/'compiled-inputs'/str(p).lstrip('/')
            saved.parent.mkdir(parents=True,exist_ok=True)
            if not saved.exists():shutil.copy2(p,saved)
            assert sha(saved)==dependencies[name]
    (out/'compiled-dependencies.json').write_text(json.dumps(dependencies,indent=2)+'\n')
    for name,files in jobs.items():
        extra=[out/'session.bin'] if name=='admission' else [out/'fixture.jpg'] if name=='retained' else []
        if name=='iommu' and steps.get('iommu-policy',{}).get('exit')==0:extra=['--translation-only']
        run(name,[out/name,*extra])
    import importlib.util
    spec=importlib.util.spec_from_file_location('jpeg_cases',source/'checks/jpeg-application-cases.py')
    cases=importlib.util.module_from_spec(spec);spec.loader.exec_module(cases)
    cases.run(image,out,source,compiler,linker,steps,steps_path)
    result={'result':'PASS','image_sha256':sha(image),'hardware_access':False,
        'limit':'Fixed supervised JPEG wiring, copied-pixel/guard/fault checks with modeled boundaries; physical decode unqualified.'}
    (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    (out/'image-manifest.json').write_text(json.dumps({'target':'sunstone-a733-jpeg','execution_profile':'jpeg-supervised-v1','final_elf':sha(image),'build_id':(build/'jpeg-application/build-id.txt').read_text().strip(),'compiled_dependencies':dependencies},indent=2)+'\n')
    print(json.dumps(result,indent=2))
if __name__=='__main__':main()
