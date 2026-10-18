#!/usr/bin/env python3
"""Build the native camera image and exercise its application boundaries."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import unicorn
import elftools
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_INTR
from unicorn import arm64_const as registers
from unicorn.arm64_const import UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_X0
ROOT = Path(__file__).resolve().parents[1]
def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--freertos', type=Path, required=True)
    parser.add_argument('--tinyusb', type=Path, required=True)
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
        paths = [p for directory in (source,out/'dependencies') for p in directory.rglob('*') if p.is_file()]
        (out/'inputs.json').write_text(json.dumps({str(p.relative_to(out)):sha(p) for p in paths},indent=2)+'\n')
    else:
        for name,digest in json.loads((out/'inputs.json').read_text()).items():
            assert sha(out/name)==digest, f'Changed preserved input: {name}'
        for p in source.rglob('*'):
            current = ROOT/p.relative_to(source)
            if p.is_file() and current.is_file():
                assert sha(p)==sha(current), f'Changed source: {current}; select a new output'
    compiler = Path(shutil.which('clang-21')).resolve()
    linker = Path.home()/'.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld'
    tool_paths=[compiler,linker.resolve(),Path(sys.executable).resolve(),Path(unicorn.__file__),Path(elftools.__file__)]
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
    toolchain = out/'toolchain.cmake'
    if not toolchain.exists():
        toolchain.write_text(f'set(SUNSTONE_LLD "{linker}" CACHE FILEPATH "")\ninclude("{source}/cmake/aarch64-clang.cmake")\nset(CMAKE_C_COMPILER "{compiler}")\nset(CMAKE_ASM_COMPILER "{compiler}")\n')
    build = out/'build'
    run('configure',['cmake','-S',source/'apps/camera','-B',build,f'-DCMAKE_TOOLCHAIN_FILE={toolchain}',
        f'-DFREERTOS_DIR={out}/dependencies/freertos',f'-DTINYUSB_DIR={out}/dependencies/tinyusb',
        '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON'])
    run('build',['cmake','--build',build,'--target','sunstone-a733-camera','-j4'])
    image = build/'sunstone-a733-camera.elf'
    dependencies = {}
    for dep in list(build.rglob('*.o.d'))+list(build.rglob('*.obj.d')):
        for name in dep.read_text().replace('\\\n',' ').split(':',1)[1].split():
            p = Path(name)
            if p.is_file():dependencies[str(p.resolve())]=sha(p)
    commands = json.loads((build/'compile_commands.json').read_text())
    for unit in commands:
        p = Path(unit['file']);dependencies[str(p.resolve())]=sha(p)
    for name in dependencies:
        p=Path(name)
        if not p.is_relative_to(out):
            saved=out/'compiled-inputs'/str(p).lstrip('/')
            saved.parent.mkdir(parents=True,exist_ok=True)
            if not saved.exists():shutil.copy2(p,saved)
            assert sha(saved)==dependencies[name]
    (out/'compiled-dependencies.json').write_text(json.dumps(dependencies,indent=2)+'\n')
    unit=next(u for u in commands if u['file'].endswith('/apps/camera/capture.c'))
    flags=shlex.split(unit['command']);flags=flags[:flags.index('-o')]
    for label,names in [('publication',['checks/camera-application.c']),
                        ('output',['checks/camera-application-output.c','apps/camera/capture.c','drivers/storage/storage_digest.c']),
                        ('admission',['checks/camera-application-admission.c','apps/camera/admission.c',
                                      'drivers/storage/storage_digest.c'])]:
        executable=out/(label+'.elf')
        run(label+'-build',flags+[source/name for name in names]+[source/'runtime/memory.c',
            '-nostdlib',f'-fuse-ld={linker}','-Wl,-Ttext=0x41000000',
            '-Wl,-e,camera_check_run','-Wl,--gc-sections','-o',executable])
        with executable.open('rb') as f:
            elf=ELFFile(f)
            segments=[(int(p['p_vaddr']),bytes(p.data()),int(p['p_memsz']))
                      for p in elf.iter_segments() if p['p_type']=='PT_LOAD']
            symbols={s.name:int(s['st_value']) for s in elf.get_section_by_name('.symtab').iter_symbols()}
        labels=['complete-publication','partial-copy-refusal','cancel-refusal','deadline-refusal','transient-contention'] if label=='publication' else ['usb-frame-read','usb-pending-refusal','usb-cancel'] if label=='output' else ['immutable-admission']
        for mode,case in enumerate(labels):
            if steps.get(case,{}).get('exit')==0:continue
            uc=Uc(UC_ARCH_ARM64,UC_MODE_ARM);pages=set()
            for address,data,size in segments:
                for page in range(address&~4095,(address+size+4095)&~4095,4096):
                    if page not in pages:uc.mem_map(page,4096);pages.add(page)
                uc.mem_write(address,data)
            uc.mem_map(0x50000000,0x10000);uc.mem_map(0x41f00000,0x1000)
            uc.reg_write(UC_ARM64_REG_SP,0x5000f000);uc.reg_write(UC_ARM64_REG_X0,mode)
            stopped=[]
            def stop(machine,interrupt,unused):stopped.append(interrupt);machine.emu_stop()
            uc.hook_add(UC_HOOK_INTR,stop)
            uc.emu_start(symbols['camera_check_run'],0,count=3000000)
            result=int.from_bytes(uc.mem_read(symbols['camera_check_result'],4),'little')
            steps[case]={'exit':0 if result==1 else result or -1,'result':result,'pc':uc.reg_read(UC_ARM64_REG_PC)}
            steps_path.write_text(json.dumps(steps,indent=2)+'\n')
            assert result==1,(case,result)
    with image.open('rb') as f:
        elf=ELFFile(f);symbols={s.name for s in elf.get_section_by_name('.symtab').iter_symbols()}
    assert {'a733_camera_native_init','a733_camera_native_poll','a733_camera_native_copy',
            'camera_capture_task','camera_usb0_task','a733_pmic_poll'}<=symbols
    assert not {'sunxi_mmc_init','a733_usb2_native_init','a733_typec_native_init'}&symbols
    (out/'result.json').write_text(json.dumps({'image_sha256':sha(image),
        'image_build_id':(build/'camera-application/build-id.txt').read_text().strip(),
        'steps':steps,'physical':False},indent=2)+'\n')
    print('PASS native camera image and nine application boundary cases')
if __name__=='__main__':main()
