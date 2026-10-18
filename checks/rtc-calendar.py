#!/usr/bin/env python3
"""Freeze one RTC initialization image and only newly affected behavior checks."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
sys.dont_write_bytecode=True
ROOT=Path(__file__).resolve().parents[1]
CACHE=Path.home()/'.cache/sunstoneos'
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def save(path,value):path.write_text(json.dumps(value,indent=2)+'\n')
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,required=True)
    mode=parser.add_mutually_exclusive_group(required=True);mode.add_argument('--setup-only',action='store_true');mode.add_argument('--run-only',action='store_true')
    a=parser.parse_args();out=a.output.resolve();source=out/'source'
    if a.setup_only:
        out.mkdir(parents=True,exist_ok=False)
        for name in subprocess.check_output(['git','ls-files','--cached','--others','--exclude-standard'],cwd=ROOT,text=True).splitlines():
            src=ROOT/name
            if src.is_file():
                dst=source/name;dst.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(src,dst)
        references=out/'reference-sources';references.mkdir()
        for name,pin in json.loads((source/'apps/rtc/sources.json').read_text()).items():
            original=CACHE/'sources'/pin['archive'];assert sha(original)==pin['sha256'],name
            shutil.copy2(original,references/(name+original.suffix))
        original=CACHE/'evidence/rtc-native-20261018/session-template.bin'
        data=original.read_bytes();assert len(data)==768
        prepare=module('rtc_initialization_producer',source/'apps/rtc/prepare.py')
        fact=json.dumps({'scope':'Software model; nondeployable','source_change_authorized':True,
             'exclusive_osc32k_consumers':True,'remote_source_writers_excluded':True,
             'onboard_crystal_fitted':True,'calendar_write_authorized':True,
             'requested_day':20744,'requested_hms':0x123438},sort_keys=True).encode()
        (out/'model-clock.json').write_bytes(fact)
        day,hms=prepare.calendar_target(2026,10,17,18,52,56)
        raw=prepare.setting_session(data,3,day,hms,hashlib.sha256(fact).digest())
        (out/'source-adoption-session.bin').write_bytes(data);(out/'session-template.bin').write_bytes(raw)
        save(out/'fixture-derivation.json',{'producer':str(original),'producer_sha256':sha(original),
             'setting_producer':'apps/rtc/prepare.py:setting_session','authority':'Software model; nondeployable.',
             'adaptations':['Explicit flags3/day20744/HMS0x123438 and modeled clock digest','Final linked identity bound by actual request producer before native validation']})
        save(out/'source-inputs.json',{str(p.relative_to(source)):sha(p) for p in source.rglob('*') if p.is_file()})
    else:
        for name,digest in json.loads((out/'runtime-inputs.json').read_text()).items():assert sha(Path(name))==digest,name
    steps_path=out/'steps.json';steps=json.loads(steps_path.read_text()) if steps_path.exists() else {}
    commands_path=out/('setup-commands.json' if a.setup_only else 'runtime-commands.json');commands=[]
    def command(label,values):
        values=list(map(str,values))
        if steps.get(label,{}).get('exit')==0:assert steps[label]['command']==values;return
        commands.append(values);save(commands_path,commands)
        steps[label]={'state':'running','command':values};save(steps_path,steps)
        result=subprocess.run(values,capture_output=True,env={**os.environ,'PYTHONDONTWRITEBYTECODE':'1'})
        (out/(label+'.log')).write_bytes(result.stdout+result.stderr);steps[label]={'exit':result.returncode,'command':values};save(steps_path,steps)
        if result.returncode:raise RuntimeError(label+' failed; inspect preserved log')
    compiler=Path('/usr/bin/clang-21');linker=CACHE/'host/usr/bin/ld.lld-21'
    image=out/'build/sunstone-a733-rtc.elf'
    if a.setup_only:
        failures=[]
        host=[compiler,'-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
              '-DSUNSTONE_BOARD_SUPERVISED=1','-I'+str(source/'include'),'-I'+str(source/'platform/a733-smp-cached')]
        jobs={'driver-build':host+[source/'checks/a733-rtc-calendar.c',source/'drivers/rtc/a733_rtc.c',source/'drivers/rtc/a733_rtc_initialize.c','-o',out/'driver'],
              'default-startup-build':host+[source/'checks/rtc-calendar-default.c',source/'apps/board/admission.c',source/'drivers/storage/storage_digest.c','-o',out/'default-startup'],
              'rtc-startup-build':host+['-DA733_BOARD_STARTUP_SECONDS=6','-DEXPECT_STARTUP=6',source/'checks/rtc-calendar-default.c',source/'apps/board/admission.c',source/'drivers/storage/storage_digest.c','-o',out/'rtc-startup']}
        for label,values in jobs.items():
            try:command(label,values)
            except RuntimeError as e:failures.append(str(e))
        launcher=out/'launcher';launcher.mkdir()
        try:module('rtc_prepare_setup',source/'apps/rtc/prepare.py').build_launcher(launcher,str(compiler),str(linker))
        except Exception as e:failures.append('launcher: '+str(e))
        model=(source/'checks/a733-smp-supervised-launch.py').read_text().replace('0x4153373353554333','0x4153373352544331').replace('480000000','1440000000')
        model=model.replace('ticks[0]+24000000','ticks[0]+144000000')
        model=model.replace('reason in (1,2,3,4,5,6,13)','reason in (1,2,3,4,5,6,13,14)')
        first=model.index('    if args.frequency_only:');last=model.index('    result =',first)
        model=model[:first]+"    cases=[check('RTC initialization six-second startup and60secondend',12),check('RTC setting extension truncated',14,mutation=(0x41f00000+608,768))]\n"+model[last:]
        (out/'rtc-launcher-model.py').write_text(model)
        if failures:raise RuntimeError('\n'.join(failures))
        (out/'toolchain.cmake').write_text(f'set(SUNSTONE_LLD "{linker}" CACHE FILEPATH "")\ninclude("{source}/cmake/aarch64-clang.cmake")\nset(CMAKE_C_COMPILER "{compiler}")\nset(CMAKE_ASM_COMPILER "{compiler}")\n')
        deps=CACHE/'evidence/h264-native-20261018/dependencies'
        command('configure',['cmake','-S',source/'apps/rtc','-B',out/'build','-DCMAKE_TOOLCHAIN_FILE='+str(out/'toolchain.cmake'),'-DPython3_EXECUTABLE='+sys.executable,
            '-DFREERTOS_DIR='+str(deps/'freertos'),'-DTINYUSB_DIR='+str(deps/'tinyusb'),'-DSUNSTONE_TCP_DIR='+str(deps/'tcp'),'-DSUNSTONE_RSN_DIR='+str(deps/'wpa_supplicant-2.12'),'-DSUNSTONE_NETWORK_C_HEADERS='+str(deps/'headers'),'-DSUNSTONE_BOARD_SUPERVISED=ON','-DCMAKE_EXPORT_COMPILE_COMMANDS=ON'])
        command('native-build',['cmake','--build',out/'build','--target','sunstone-a733-rtc','-j4'])
        snapshot=module('rtc_evidence',source/'tools/evidence-snapshot.py')
        dependencies,actual,records=snapshot.collect_dependencies(out/'build','sunstone-a733-rtc')
        for unit in ('apps/board/main.c','apps/rtc/admission.c','apps/rtc/initialize.c'):
            records_for_unit=[record for record in actual if record['file'].endswith('/'+unit)]
            assert records_for_unit and all('-DA733_BOARD_STARTUP_SECONDS=6' in record['command'] for record in records_for_unit),unit
        save(out/'compiled-dependencies.json',dependencies);save(out/'compiled-commands.json',actual);save(out/'dependency-records.json',records)
        for name,digest in dependencies.items():
            path=Path(name)
            if not path.is_relative_to(out):
                dst=out/'compiled-inputs'/str(path).lstrip('/');dst.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(path,dst);assert sha(dst)==digest
        cases=module('rtc_cases_setup',source/'checks/rtc-application-cases.py');symbols,segments,layouts,identity=cases.setup(image,out)
        text=(out/'build/rtc-application/boot.c').read_text()
        order=['board_session_prepare(&admission)','rtc_session_prepare(&admission)','a733_cached_prepare(','rtc_boot_route()','for(unsigned c=1;c<8;']
        assert [text.index(x) for x in order]==sorted(text.index(x) for x in order)
        assert 'board_identity_prepare' not in text
        # Verify feature selection, actual patched startup and const identity.
        save(out/'image-manifest.json',{'target':'sunstone-a733-rtc','execution_profile':'rtc-supervised-v1','final_elf':sha(image),'build_id':identity.hex(),'compiled_dependencies':dependencies,'generated_boot_order':order})
        import unicorn,elftools
        tools=[compiler,linker,Path(sys.executable),*[Path(shutil.which(name)) for name in ('cmake','make','patch')],Path('/usr/lib/llvm-21/bin/llvm-objcopy')]
        paths=[p for p in out.rglob('*') if p.is_file() and p.name not in ('steps.json','runtime-inputs.json')]+list(map(Path,dependencies))+tools
        paths += [p for package in (unicorn,elftools) for p in Path(package.__file__).parent.rglob('*') if p.is_file() and '__pycache__' not in p.parts]
        save(out/'runtime-inputs.json',{str(p):sha(p) for p in dict.fromkeys(paths)})
        print('Setup passed; no runtime cases executed.');return
    def step(name,result=None):
        if result is None:
            if steps.get(name,{}).get('exit')==0:return True
            steps[name]={'state':'running'};save(steps_path,steps);return False
        steps[name]={'exit':0,**result};save(steps_path,steps)
    command('driver',[out/'driver'])
    command('default-startup',[out/'default-startup',out/'source-adoption-session.bin'])
    command('rtc-startup',[out/'rtc-startup',out/'source-adoption-session.bin'])
    try:module('rtc_cases_run',source/'checks/rtc-calendar-cases.py').run(out,source,step)
    except BaseException as error:
        import traceback
        (out/'runtime-failure.log').write_text(traceback.format_exc())
        for label,value in steps.items():
            if value.get('state')=='running':value.update(state='failed',error=str(error))
        save(steps_path,steps);raise
    command('launcher',[sys.executable,out/'rtc-launcher-model.py','--bundle',out/'launcher','--output',out/'launcher-result.json'])
    command('host',[sys.executable,source/'checks/rtc-calendar-host.py',out])
    save(out/'result.json',{'passed':True,'physical_access':False,'image_sha256':sha(image),'scope':'New explicit calendar/source operations and linked sealed initialization/copy instructions; default startup validation, own v2 launcher and host boundary. Physical clocks/GIC/cache/USB and wall-clock accuracy remain modeled/unqualified.'})
    command('software-proof',[sys.executable,source/'checks/rtc-host-proof.py',out])
    print('PASS: native RTC initialization software boundaries.')
if __name__=='__main__':main()
