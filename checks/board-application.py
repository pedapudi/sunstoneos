#!/usr/bin/env python3
"""Build the native board composition and exercise only its new contracts."""
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
    parser.add_argument('--tcp', type=Path, required=True)
    parser.add_argument('--rsn', type=Path, required=True)
    parser.add_argument('--headers', type=Path, required=True)
    parser.add_argument('--supervised', action='store_true')
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
    run('configure',['cmake','-S',source/'apps/board','-B',build,f'-DCMAKE_TOOLCHAIN_FILE={toolchain}',
        f'-DFREERTOS_DIR={out}/dependencies/freertos',f'-DTINYUSB_DIR={out}/dependencies/tinyusb',
        f'-DSUNSTONE_TCP_DIR={out}/dependencies/tcp',f'-DSUNSTONE_RSN_DIR={out}/dependencies/{a.rsn.name}',
        f'-DSUNSTONE_NETWORK_C_HEADERS={out}/dependencies/headers','-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',f'-DSUNSTONE_BOARD_SUPERVISED={"ON" if a.supervised else "OFF"}'])
    run('build',['cmake','--build',build,'--target','sunstone-a733-board','-j4'])
    image = build/'sunstone-a733-board.elf'
    dependencies = {}
    for dep in build.rglob('*.o.d'):
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
    # Reuse the actual client translation unit's compile configuration.
    client = next(unit for unit in commands if unit['file'].endswith('/apps/board/client.c'))
    flags = shlex.split(client['command'])
    flags = flags[:flags.index('-o')]
    cleaned = []
    index = 0
    while index < len(flags):
        if flags[index] in ('-MT','-MF','-MQ'):
            index += 2
        elif flags[index] in ('-MD','-MMD'):
            index += 1
        else:
            cleaned.append(flags[index]);index += 1
    flags = cleaned
    if a.supervised:
        import importlib.util
        spec=importlib.util.spec_from_file_location('board_supervised_check',source/'checks/board-supervised.py')
        check=importlib.util.module_from_spec(spec);spec.loader.exec_module(check)
        result=check.run(image,out,source,compiler,run,steps,steps_path)
        (out/'image-manifest.json').write_text(json.dumps({'target':'sunstone-a733-board',
            'execution_profile':'board-supervised-v2','final_elf':sha(image),
            'build_id':(build/'board-application/build-id.txt').read_text().strip(),
            'compiled_dependencies':dependencies},indent=2)+'\n')
        (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
        print(json.dumps(result,indent=2));return
    fixture = out/'client.elf'
    run('client-build',flags+[source/'apps/board/client.c',source/'checks/board-application.c',
        source/'runtime/memory.c','-nostdlib',f'-fuse-ld={linker}','-Wl,-Ttext=0x41000000',
        '-Wl,-e,board_check_run','-Wl,--gc-sections','-o',fixture])
    watchdog_fixture = out/'watchdog.elf'
    run('watchdog-build',flags+[source/'checks/board-application-watchdog.c',source/'runtime/memory.c',
        '-nostdlib',f'-fuse-ld={linker}','-Wl,-Ttext=0x41000000','-Wl,--gc-sections',
        '-Wl,-e,board_check_run','-o',watchdog_fixture])
    output_fixture = out/'output.elf'
    run('output-build',flags+[source/'checks/board-application-output.c',source/'runtime/memory.c',
        source/'drivers/storage/storage_digest.c','-nostdlib',f'-fuse-ld={linker}',
        '-Wl,-Ttext=0x41000000','-Wl,--gc-sections','-Wl,-e,board_check_run','-o',output_fixture])
    run('admission-build',[compiler,'-std=c11','-O1','-g','-fsanitize=address,undefined',
        '-I'+str(source/'include'),'-I'+str(source/'platform/a733-smp-cached'),
        source/'apps/board/admission.c',source/'drivers/storage/storage_digest.c',
        source/'checks/board-application-admission.c','-o',out/'admission'])
    run('admission',[out/'admission'])
    # Each client case is a separate machine. Stop at its explicit BRK result;
    # never continue a successful case or replay it after another case fails.
    def machine_cases(executable,labels):
        with executable.open('rb') as f:
            elf = ELFFile(f)
            segments = [(int(p['p_vaddr']),bytes(p.data()),int(p['p_memsz'])) for p in elf.iter_segments() if p['p_type']=='PT_LOAD']
            symbols = {s.name:int(s['st_value']) for s in elf.get_section_by_name('.symtab').iter_symbols()}
        for mode,label in enumerate(labels):
            if steps.get(label,{}).get('exit')==0:continue
            uc = Uc(UC_ARCH_ARM64,UC_MODE_ARM)
            pages = set()
            for address,data,size in segments:
                for page in range(address&~4095,(address+size+4095)&~4095,4096):
                    if page not in pages:uc.mem_map(page,4096);pages.add(page)
                uc.mem_write(address,data)
            uc.mem_map(0x50000000,0x10000)
            uc.reg_write(UC_ARM64_REG_SP,0x5000f000)
            uc.reg_write(UC_ARM64_REG_X0,mode)
            checkpoint=out/(label+'.checkpoint.json')
            if checkpoint.exists():
                saved=json.loads(checkpoint.read_text())
                assert saved['elf']==sha(executable)
                memory=(out/(label+'.checkpoint.bin')).read_bytes()
                for address,offset,length in saved['memory']:
                    uc.mem_write(address,memory[offset:offset+length])
                for name,value in saved['registers'].items():
                    uc.reg_write(getattr(registers,name),value)
            stopped=[]
            def stop(machine,interrupt,unused):
                stopped.append(interrupt);machine.emu_stop()
            uc.hook_add(UC_HOOK_INTR,stop)
            start=uc.reg_read(UC_ARM64_REG_PC) if checkpoint.exists() else symbols['board_check_run']
            uc.emu_start(start,0,count=1000000)
            result = int.from_bytes(uc.mem_read(symbols['board_check_result'],4),'little')
            if not result and not stopped:
                chunks=[];layout=[];offset=0
                for begin,end,permissions in uc.mem_regions():
                    chunk=bytes(uc.mem_read(begin,end-begin+1))
                    layout.append([begin,offset,len(chunk)]);chunks.append(chunk);offset+=len(chunk)
                names=['UC_ARM64_REG_X'+str(i) for i in range(31)]+[
                    'UC_ARM64_REG_SP','UC_ARM64_REG_PC','UC_ARM64_REG_NZCV','UC_ARM64_REG_FPCR','UC_ARM64_REG_FPSR']
                (out/(label+'.checkpoint.bin')).write_bytes(b''.join(chunks))
                checkpoint.write_text(json.dumps({'elf':sha(executable),'memory':layout,
                    'registers':{name:uc.reg_read(getattr(registers,name)) for name in names}},indent=2)+'\n')
                raise RuntimeError(f'{label} reached its instruction allowance; state preserved. Resume this output without replay.')
            steps[label]={'exit':0 if result==1 else result or -1,'result':result,'pc':uc.reg_read(UC_ARM64_REG_PC)}
            steps_path.write_text(json.dumps(steps,indent=2)+'\n')
            assert result==1, (label,result)
            if 'board_check_frame' in symbols:
                length=int.from_bytes(uc.mem_read(symbols['board_check_frame_bytes'],4),'little')
                (out/(label+'.frame')).write_bytes(uc.mem_read(symbols['board_check_frame'],length))
                (out/(label+'.session')).write_bytes(uc.mem_read(symbols['board'],592))
                # Profile size is stored in its native fixed header.
                profile_size=int.from_bytes(uc.mem_read(symbols['profile']+12,4),'little')
                (out/(label+'.profile')).write_bytes(uc.mem_read(symbols['profile'],profile_size))
    machine_cases(fixture,['copied-sector-and-continuing-poll','retained-host-failure','post-copy-attachment-loss'])
    machine_cases(watchdog_fixture,['watchdog-no-progress','watchdog-completed-transfer','watchdog-expired-operation'])
    machine_cases(output_fixture,['output-completed-radio','output-failed-radio','output-pending-sector'])
    import importlib.util
    spec=importlib.util.spec_from_file_location('board_observe',source/'apps/board/observe.py')
    observer=importlib.util.module_from_spec(spec);spec.loader.exec_module(observer)
    if steps.get('observer-validation',{}).get('exit')!=0:
        for label in ['output-completed-radio','output-failed-radio','output-pending-sector']:
            frame=(out/(label+'.frame')).read_bytes();session=(out/(label+'.session')).read_bytes();profile=(out/(label+'.profile')).read_bytes()
            decoded=observer.decode(frame,session,profile)
            assert decoded['radio_state']==(3 if label=='output-failed-radio' else 2)
            assert decoded['verified_radio_bytes']==(0 if label=='output-failed-radio' else 5)
            if label=='output-pending-sector':assert decoded['sector'] is None
            else:assert decoded['sector']==bytes((i^0x5a)&255 for i in range(512))
            wrong=bytearray(frame);wrong[32]^=1
            try:observer.decode(wrong,session,profile)
            except ValueError:pass
            else:raise AssertionError('changed admitted identity accepted')
        steps['observer-validation']={'exit':0}
        steps_path.write_text(json.dumps(steps,indent=2)+'\n')
    # Two independently linked role contexts and time implementations coexist.
    with image.open('rb') as f:
        elf = ELFFile(f)
        symbols = {s.name for s in elf.get_section_by_name('.symtab').iter_symbols()}
    assert {'tusb_rhport_init','board_host_tusb_rhport_init',
            'board_host_tusb_time_millis_api','_tusb_rhport_role','board_host_tusb_rhport_role','tud_task_ext','tuh_task_ext'} <= symbols
    # Device timing is correctly discarded when no device caller needs it;
    # its object must still define the independent device implementation.
    device_unit=next(unit for unit in commands if unit['file'].endswith('/apps/board/usb0.c'))
    command=shlex.split(device_unit['command'])
    device_object=Path(command[command.index('-o')+1])
    if not device_object.is_absolute():device_object=Path(device_unit['directory'])/device_object
    with device_object.open('rb') as f:
        elf=ELFFile(f)
        assert 'tusb_time_millis_api' in {s.name for s in elf.get_section_by_name('.symtab').iter_symbols()}
    result={'result':'PASS','hardware_access':False,'image_sha256':sha(image),
        'fixture_sha256':{p.name:sha(p) for p in [fixture,watchdog_fixture,output_fixture,out/'admission']},'build_id':(build/'board-application/build-id.txt').read_text().strip(),
        'cases':[name for name in steps if not name.endswith('-build') and name not in ('build','configure')]+['linked-independent-TinyUSB-contexts'],
        'limit':'Native client execution models provider returns; actual Type-C/USB2 component evidence reused. No full board startup or physical transfer claimed.'}
    (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))
if __name__=='__main__':main()
