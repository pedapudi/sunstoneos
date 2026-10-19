#!/usr/bin/env python3
"""Build the native i2s image and exercise its application boundaries."""
import argparse
import hashlib
import importlib.util
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
    parser.add_argument('--cyclic-only',action='store_true',help='Run changed cyclic copied RX, immutable TX and final-count client branches only')
    parser.add_argument('--native-build',type=Path,help='Reuse this already-built native image for cyclic-only cases')
    parser.add_argument('--repeated-only',action='store_true',help='Run repeated client ownership and actual USB acknowledgment cases only')
    parser.add_argument('--duplex-only',action='store_true',help='Run changed duplex client and two-catalogue constructor cases only')
    parser.add_argument('--wav',type=Path,required=True)
    parser.add_argument('--qemu',type=Path,required=True)
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
        shutil.copy2(a.wav,out/'dependencies/recorded.wav')
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
    tool_paths=[a.qemu.resolve(),compiler,linker.resolve(),Path(sys.executable).resolve(),Path(unicorn.__file__),Path(elftools.__file__)]
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
        result = subprocess.run(command,capture_output=True,text=True,timeout=60)
        (out/(label+'.log')).write_text(result.stdout+result.stderr)
        status=result.returncode
        if label.startswith('native-pool-') and 'PASS native I2S' not in result.stdout:status=-1
        steps[label] = {'command':command,'exit':status}
        steps_path.write_text(json.dumps(steps,indent=2)+'\n')
        result.check_returncode()
        if status:raise RuntimeError(f'{label}: missing explicit fixture completion')
    toolchain = out/'toolchain.cmake'
    if not toolchain.exists():
        toolchain.write_text(f'set(SUNSTONE_LLD "{linker}" CACHE FILEPATH "")\ninclude("{source}/cmake/aarch64-clang.cmake")\nset(CMAKE_C_COMPILER "{compiler}")\nset(CMAKE_ASM_COMPILER "{compiler}")\n')
    build = a.native_build.resolve() if a.native_build else out/'build'
    if a.native_build:
        assert a.cyclic_only, '--native-build is only supported for focused cyclic cases'
    else:
        run('configure',['cmake','-S',source/'apps/i2s','-B',build,f'-DCMAKE_TOOLCHAIN_FILE={toolchain}',
        f'-DFREERTOS_DIR={out}/dependencies/freertos',f'-DTINYUSB_DIR={out}/dependencies/tinyusb',
        f'-DSUNSTONE_I2S_RECORDED_WAV={out}/dependencies/recorded.wav','-DCMAKE_EXPORT_COMPILE_COMMANDS=ON'])
        run('build',['cmake','--build',build,'--target','sunstone-a733-i2s','-j4'])
    image = build/'sunstone-a733-i2s.elf'
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
    unit=next(u for u in commands if u['file'].endswith('/apps/i2s/client.c'))
    flags=shlex.split(unit['command']);flags=flags[:flags.index('-o')]
    cases=[('admission',['checks/i2s-application-admission.c','apps/i2s/admission.c','drivers/storage/storage_digest.c'],['immutable-supervised-admission']),
           ('output',['checks/i2s-application-output.c','apps/i2s/client.c','drivers/storage/storage_digest.c'],['usb-pcm-read','usb-pending-refusal','usb-cancel']),
           ('client',['checks/i2s-application-client.c','apps/i2s/client.c'],
            ['capture-copy-stop','playback-copy-stop','cancel-retention','deadline-retention','failed-drain-retention']),
           ('native',['checks/i2s-application-native.c','drivers/audio/a733_audio_leases.c',
                      'drivers/audio/a733_i2s_platform.c','drivers/audio/a733_i2s_platform_native.c',
                      'drivers/clock/a733_clock_leases.c'],['native-parent-and-adapter'])]
    if a.duplex_only:
        cases=[('client',['checks/i2s-application-client.c','apps/i2s/client.c'],
            [None]*5+['duplex-client-completions','duplex-missing-tx-retention','duplex-cancel-both']),
            ('output',['checks/i2s-application-output.c','apps/i2s/client.c','drivers/storage/storage_digest.c'],
            [None]*3+['duplex-usb-completion-counts'])]
    repeated_session=None
    if a.repeated_only or a.cyclic_only:
        spec=importlib.util.spec_from_file_location('i2s_repeated_prepare',source/'apps/i2s/prepare.py')
        preparation=importlib.util.module_from_spec(spec);spec.loader.exec_module(preparation)
        _,_,identity,layout,pcm=preparation.image_data(image.read_bytes())
        pinned=Path.home()/'.cache/sunstoneos/evidence/i2s-binding-20261018/cases/nominal-session.bin'
        raw=bytearray(pinned.read_bytes()[:layout['size']])
        def put(name,value):
            f=layout['fields'][name];data=value if isinstance(value,bytes) else value.to_bytes(f['size'],'little')
            assert len(data)==f['size'];raw[f['offset']:f['offset']+f['size']]=data
        for name,value in {'version':1,'bytes':layout['size'],'direction':5,'dma.service_channels':3,
                'dma.allowed.desired_cpu':3,'dma.allowed.route_change_mask':3,'image':identity,
                'pcm':hashlib.sha256(pcm).digest()}.items():put(name,value)
        preparation.session_profile(bytes(raw),layout,identity,pcm)
        repeated_session=bytes(raw);(out/'repeated-session.bin').write_bytes(repeated_session)
        (out/'repeated-session-source.json').write_text(json.dumps({'source':str(pinned),'sha256':sha(pinned),'image_sha256':sha(image),'physical':False},indent=2)+'\n')
        common=['apps/i2s/repeated.c','apps/i2s/admission.c','drivers/storage/storage_digest.c',str(build/'i2s-application/recorded.c')]
        cases=[('repeated-client',['checks/i2s-repeated-client.c','apps/i2s/client.c']+common,
                ['repeated-rx-wrap','repeated-tx-wrap','repeated-duplex-wrap','slow-consumer-drained-retention',
                 'tx-underrun-drain','cancel-races-terminal','cancel-preserves-copied-backlog','pending-get-collected-before-cancel']),
               ('repeated-usb',['checks/i2s-repeated-usb.c','apps/i2s/client.c']+common,['usb-full-delivery-before-ack','usb-partial-delivery-before-ack'])]
    if a.cyclic_only:
        cases=[('cyclic-client',['checks/i2s-repeated-client.c','apps/i2s/client.c']+common,
            [None]*8+['cyclic-copied-rx-client','cyclic-immutable-tx-aggregate-progress','cyclic-duplex-final-drain-count'])]
    for label,names,labels in cases:
        if 'apps/i2s/client.c' in names and 'apps/i2s/repeated.c' not in names:names=names+['apps/i2s/repeated.c']
        executable=out/(label+'.elf')
        run(label+'-build',flags+[source/name for name in names]+[source/'runtime/memory.c',source/'runtime/memmove.c',
            '-nostdlib',f'-fuse-ld={linker}','-Wl,-Ttext=0x41000000',
            '-Wl,-e,i2s_check_run','-Wl,--gc-sections','-o',executable])
        with executable.open('rb') as f:
            elf=ELFFile(f)
            segments=[(int(p['p_vaddr']),bytes(p.data()),int(p['p_memsz']))
                      for p in elf.iter_segments() if p['p_type']=='PT_LOAD']
            symbols={s.name:int(s['st_value']) for s in elf.get_section_by_name('.symtab').iter_symbols()}
        for mode,case in enumerate(labels):
            if case is None:continue
            if steps.get(case,{}).get('exit')==0:continue
            uc=Uc(UC_ARCH_ARM64,UC_MODE_ARM);pages=set()
            for address,data,size in segments:
                for page in range(address&~4095,(address+size+4095)&~4095,4096):
                    if page not in pages:uc.mem_map(page,4096);pages.add(page)
                uc.mem_write(address,data)
            uc.mem_map(0x41f00000,0x1000);uc.mem_map(0x50000000,0x10000);uc.mem_map(0x02002000,0x2000)
            if repeated_session is not None:uc.mem_write(0x41f00000,repeated_session)
            uc.mem_map(0x02532000,0x1000);uc.mem_map(0x07090000,0x1000)
            uc.reg_write(UC_ARM64_REG_SP,0x5000f000);uc.reg_write(UC_ARM64_REG_X0,mode)
            def stop(machine,interrupt,unused):machine.emu_stop()
            uc.hook_add(UC_HOOK_INTR,stop)
            uc.emu_start(symbols['i2s_check_run'],0,count=3000000)
            result=int.from_bytes(uc.mem_read(symbols['i2s_check_result'],4),'little')
            steps[case]={'exit':0 if result==1 else result or -1,'result':result,'pc':uc.reg_read(UC_ARM64_REG_PC)}
            steps_path.write_text(json.dumps(steps,indent=2)+'\n');assert result==1,(case,result)
    if a.repeated_only or a.cyclic_only:
        (out/'result.json').write_text(json.dumps({'image_sha256':sha(image),
            'image_build_id':(build/'i2s-application/build-id.txt').read_text().strip(),
            'steps':steps,'physical':False},indent=2)+'\n')
        print('PASS cyclic native client RX copy, aggregate immutable TX and final drain counts' if a.cyclic_only else
              'PASS repeated native I2S image, owned copied-ring reuse, cancellation/drain and USB delivery acknowledgments')
        return
    # Actual native pool/service constructors; no scheduling or A733 MMIO.
    project=out/'pool-project';project.mkdir(exist_ok=True)
    pool_build=out/'pool-build';pool_link=project/'link.ld'
    text=(source/'platform/qemu-smp/link.ld').read_text().replace('    /DISCARD/',
        '    __heap_end = .;\n    .device_pool (NOLOAD) : ALIGN(4096) { __device_pool_start = .; . += 2097152; __device_pool_end = .; } > RAM\n    /DISCARD/')
    pool_link.write_text(text)
    project_text=f'''cmake_minimum_required(VERSION 3.24)
project(i2s_pool_check LANGUAGES C ASM)
add_subdirectory("{source}" sunstone)
apply_build_patch("{pool_build}/pool-port.c" "{pool_build}/sunstone/freertos-smp/port.c"
 "{source}/patches/freertos-smp-privileged.patch")
set_source_files_properties("{pool_build}/pool-port.c" PROPERTIES
 COMPILE_DEFINITIONS "npu_service_task_privileged=a733_dma_pool_task_privileged")
apply_build_patch("{pool_build}/start.S" "{source}/platform/qemu-smp/start.S"
 "{source}/checks/npu-service-start.patch")
get_target_property(units dma-startup-model SOURCES)
list(FILTER units EXCLUDE REGEX "checks/|platform/qemu-smp/start.S|dma-startup-port.c$")
list(APPEND units arch/aarch64/device_memory.c runtime/device_memory_pool.c
 platform/a733/memory_reservations.c platform/a733/dma_native.c drivers/dma/dma_memory.c
 drivers/audio/a733_i2s_stream.c drivers/dma/dma_service_i2s_stream.c
 platform/a733/dma_pool_session.c "{pool_build}/pool-port.c" "{pool_build}/start.S")
list(REMOVE_DUPLICATES units)
set(absolute)
foreach(unit IN LISTS units)
 if(NOT IS_ABSOLUTE "${{unit}}")
  set(unit "{source}/${{unit}}")
 endif()
 list(APPEND absolute "${{unit}}")
endforeach()
add_library(pool-core OBJECT ${{absolute}})
target_include_directories(pool-core PUBLIC "{source}/platform/qemu-smp" "{source}/include"
 "{source}/runtime/include" "{out}/dependencies/freertos/include" "{pool_build}/sunstone/freertos-smp")
target_compile_definitions(pool-core PUBLIC INCLUDE_xTaskGetSchedulerState=1
 INCLUDE_xTaskGetCurrentTaskHandle=1 INCLUDE_xSemaphoreGetMutexHolder=1)
target_compile_options(pool-core PUBLIC -march=armv8-a -mstrict-align -ffreestanding -fno-builtin
 -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections
 -fdata-sections -O2 -g -Wall -Wextra -Werror)
foreach(direction {"2" if a.duplex_only else "0 1"})
 add_executable(i2s-pool-${{direction}} "{source}/checks/i2s-application-pool.c")
 target_link_libraries(i2s-pool-${{direction}} PRIVATE pool-core)
 target_compile_definitions(i2s-pool-${{direction}} PRIVATE I2S_CHECK_TX=${{direction}})
 target_link_options(i2s-pool-${{direction}} PRIVATE -nostdlib -static -Wl,--gc-sections "-Wl,-T,{pool_link}")
 set_target_properties(i2s-pool-${{direction}} PROPERTIES SUFFIX ".elf")
endforeach()
'''
    (project/'CMakeLists.txt').write_text(project_text)
    run('pool-configure',['cmake','-S',project,'-B',pool_build,f'-DCMAKE_TOOLCHAIN_FILE={toolchain}',
        f'-DFREERTOS_DIR={out}/dependencies/freertos',f'-DTINYUSB_DIR={out}/dependencies/tinyusb',
        f'-DSUNSTONE_LLD={linker}'])
    run('pool-build',['cmake','--build',pool_build,'--target']+(['i2s-pool-2'] if a.duplex_only else ['i2s-pool-0','i2s-pool-1'])+['-j4'])
    for direction in ((2,) if a.duplex_only else (0,1)):
        run('native-pool-'+str(direction),[a.qemu,'-machine','virt,gic-version=3',
            '-cpu','cortex-a53','-smp','2','-m','128M','-nographic','-monitor','none',
            '-semihosting-config','enable=on,target=native','-kernel',pool_build/f'i2s-pool-{direction}.elf'])
    # Capture actual dependencies from both AArch64 CMake naming conventions.
    for dep in list(pool_build.rglob('*.obj.d'))+list(pool_build.rglob('*.o.d')):
        for name in dep.read_text().replace('\\\n',' ').split(':',1)[1].split():
            p=Path(name)
            if p.is_file():dependencies[str(p.resolve())]=sha(p)
    for name in dependencies:
        p=Path(name)
        if not p.is_relative_to(out):
            saved=out/'compiled-inputs'/str(p).lstrip('/');saved.parent.mkdir(parents=True,exist_ok=True)
            if not saved.exists():shutil.copy2(p,saved)
            assert sha(saved)==dependencies[name]
    (out/'compiled-dependencies.json').write_text(json.dumps(dependencies,indent=2)+'\n')
    (out/'result.json').write_text(json.dumps({'image_sha256':sha(image),
        'image_build_id':(build/'i2s-application/build-id.txt').read_text().strip(),
        'steps':steps,'physical':False},indent=2)+'\n')
    print('PASS native I2S image, parent/adapter, RX/TX pool and client boundary cases')
if __name__=='__main__':main()
