#!/usr/bin/env python3
"""Freeze one owner-task RTC runtime image and only newly affected behavior checks."""
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
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,required=True);parser.add_argument('--resume-setup',action='store_true')
    mode=parser.add_mutually_exclusive_group(required=True);mode.add_argument('--setup-only',action='store_true');mode.add_argument('--run-only',action='store_true')
    a=parser.parse_args();out=a.output.resolve();source=out/'source'
    if a.setup_only and not a.resume_setup:
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
        fact=json.dumps({'scope':'Software model; nondeployable','external_32768_stable':True,'exclusive_calendar_writer_lifetime':True,'remote_calendar_writers_excluded':True,'registered_native_calendar_clients_authorized':True,'source_change_authorized':True,
             'exclusive_osc32k_consumers':True,'remote_source_writers_excluded':True,
             'onboard_crystal_fitted':True,'calendar_write_authorized':True,
             'requested_day':20744,'requested_hms':0x123438},sort_keys=True).encode()
        (out/'model-clock.json').write_bytes(fact)
        day,hms=prepare.calendar_target(2026,10,17,18,52,56)
        raw=prepare.setting_session(data,4,day,hms,hashlib.sha256(fact).digest(),service=True)
        (out/'source-adoption-session.bin').write_bytes(data);(out/'session-template.bin').write_bytes(raw)
        save(out/'fixture-derivation.json',{'producer':str(original),'producer_sha256':sha(original),
             'setting_producer':'apps/rtc/prepare.py:setting_session','authority':'Software model; nondeployable.',
             'adaptations':['Explicit runtime flags4/day20744/HMS0x123438 and modeled clock digest','Final linked identity bound by actual request producer before native validation']})
        save(out/'source-inputs.json',{str(p.relative_to(source)):sha(p) for p in source.rglob('*') if p.is_file()})
    elif a.run_only:
        for name,digest in json.loads((out/'runtime-inputs.json').read_text()).items():assert sha(Path(name))==digest,name
    steps_path=out/'steps.json';steps=json.loads(steps_path.read_text()) if steps_path.exists() else {}
    commands_path=out/('setup-commands.json' if a.setup_only else 'runtime-commands.json');commands=json.loads(commands_path.read_text()) if commands_path.exists() else []
    def command(label,values):
        values=list(map(str,values))
        if steps.get(label,{}).get('exit')==0:assert steps[label]['command']==values;return
        commands.append(values);save(commands_path,commands)
        previous=steps.get(label,{})
        attempt=previous.get('attempt',0)+1
        log=out/(label+('' if attempt==1 else '-attempt'+str(attempt))+'.log')
        history=previous.get('history',[])+([previous] if previous else [])
        steps[label]={'state':'running','command':values,'attempt':attempt,'log':str(log),'history':history};save(steps_path,steps)
        try:
            result=subprocess.run(values,capture_output=True,env={**os.environ,'PYTHONDONTWRITEBYTECODE':'1'},timeout=20 if label.startswith('scheduler-') and label!='scheduler-build' else 240)
            output=result.stdout+result.stderr;code=result.returncode
        except subprocess.TimeoutExpired as error:
            output=(error.stdout or b'')+(error.stderr or b'')+b'\nDeclared bounded process deadline expired.\n';code=124
        log.write_bytes(output);steps[label].update(exit=code,state='complete');save(steps_path,steps)
        if code:raise RuntimeError(label+' failed; inspect preserved log')
    compiler=Path('/usr/bin/clang-21');linker=CACHE/'host/usr/bin/ld.lld-21'
    image=out/'build/sunstone-a733-rtc.elf'
    if a.setup_only:
        failures=[]
        kernel=CACHE/'sources/FreeRTOS-Kernel-97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2'
        origin=kernel;kernel=out/'scheduler-kernel'
        if not a.resume_setup:
            kernel.mkdir()
            for name in ('tasks.c','queue.c','list.c','LICENSE.md'):shutil.copy2(origin/name,kernel/name)
            shutil.copytree(origin/'include',kernel/'include')
            shutil.copytree(origin/'portable/ThirdParty/GCC/Posix',kernel/'portable/ThirdParty/GCC/Posix')
            save(out/'scheduler-source-pins.json',{'commit':'97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2','files':{str(p.relative_to(kernel)):sha(p) for p in kernel.rglob('*') if p.is_file()}})
        else:
            for name,digest in json.loads((out/'scheduler-source-pins.json').read_text())['files'].items():assert sha(kernel/name)==digest,name
        posix=kernel/'portable/ThirdParty/GCC/Posix'
        host=[compiler,'-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=undefined','-pthread','-D_POSIX_C_SOURCE=200809L',
              '-DSUNSTONE_BOARD_SUPERVISED=1','-I'+str(source/'checks/rtc-service-posix'),'-I'+str(posix),'-I'+str(kernel/'include'),'-I'+str(source/'include'),'-I'+str(source/'apps/rtc'),'-I'+str(source/'platform/a733-smp-cached')]
        jobs={'scheduler-build':host+[source/'checks/rtc-service-posix/model.c',source/'apps/rtc/service.c',source/'drivers/rtc/a733_rtc.c',source/'drivers/rtc/a733_rtc_initialize.c',
              source/'drivers/storage/storage_digest.c',kernel/'tasks.c',kernel/'queue.c',kernel/'list.c',posix/'port.c',posix/'utils/wait_for_event.c','-o',out/'scheduler']}
        for label,values in jobs.items():
            try:command(label,values)
            except RuntimeError as e:failures.append(str(e))
        launcher=out/'launcher';launcher.mkdir(exist_ok=a.resume_setup)
        try:
            if not a.resume_setup or not (launcher/'launch.bin').exists():module('rtc_prepare_setup',source/'apps/rtc/prepare.py').build_launcher(launcher,str(compiler),str(linker))
        except Exception as e:failures.append('launcher: '+str(e))
        model=(source/'checks/a733-smp-supervised-launch.py').read_text().replace('0x4153373353554333','0x4153373352544331').replace('480000000','1440000000')
        model=model.replace('ticks[0]+24000000','ticks[0]+144000000')
        model=model.replace('reason in (1,2,3,4,5,6,13)','reason in (1,2,3,4,5,6,13,14)')
        first=model.index('    if args.frequency_only:');last=model.index('    result =',first)
        model=model[:first]+"    cases=[check('RTC service handoff',12),check('RTC runtime extension truncated',14,mutation=(0x41f00000+608,768))]\n"+model[last:]
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
        cases=module('rtc_cases_setup',source/'checks/rtc-service-native-cases.py');prior,symbols,layouts,identity=cases.setup(image,out,source)
        identity=bytes.fromhex(json.loads((out/'service-preflight.json').read_text())['build_id'])
        nominal=prior.Machine(image,out,source,startup=6)
        (out/'nominal-admitted-session.bin').write_bytes(nominal.session)
        save(out/'native-validator-preflight.json',{'passed':True,'session_sha256':hashlib.sha256(nominal.session).hexdigest(),'contract_sha256':hashlib.sha256(nominal.contract).hexdigest(),'build_id':identity.hex(),'authority':'Software model; nondeployable'})
        producer=module('rtc_service_launch_producer',source/'apps/rtc/prepare.py')
        parts,address,actual_id=producer.base.image_parts(image);assert actual_id==identity
        launch_raw=bytearray((out/'session-template.bin').read_bytes());launch_raw[48:80]=identity
        epoch,generation,lease=struct.unpack_from('<3Q',launch_raw,24)
        request,contract=producer.request_bytes(address,identity,launch_raw,epoch,generation,lease)
        for name,data in [('request.bin',request),('contract.bin',contract),('session.bin',launch_raw),('identity.bin',identity)]:
            (launcher/name).write_bytes(data)
        save(launcher/'prepared.json',{'segments':[{'name':'session.bin','address':0x41f00000},{'name':'identity.bin','address':address}]})
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
    for variant in ('composition','gic-delay','partial','loss','cancel-queue'):
        command('scheduler-'+variant,[out/'scheduler',variant,out/(variant+'-service-output.bin')])
    if not step('native-service'):
        module('rtc_cases_run',source/'checks/rtc-service-native-cases.py').run(out,source)
        step('native-service',{'passed':True})
    command('launcher',[sys.executable,out/'rtc-launcher-model.py','--bundle',out/'launcher','--output',out/'launcher-result.json'])
    command('host',[sys.executable,source/'checks/rtc-service-host.py',out])
    save(out/'result.json',{'passed':True,'physical_access':False,'image_sha256':sha(image),'scope':'Reusable RTC owner/queue and two clients under actual pinned FreeRTOS POSIX scheduling; native callback/binding instructions under modeled providers. Prior cold/IRQ evidence reused; physical timing/UTC/USB remain unqualified.'})

    tool=module('rtc_service_proof',source/'tools/check-a733-rtc-supervised.py')
    sources={name:{'path':str(path.resolve()),'sha256':sha(path)} for name,path in tool.software_sources().items()}
    evidence=[out/name for name in ('result.json','host-result.json','launcher-result.json','native-validator-preflight.json','native-service-result.json','scheduler-composition.log','scheduler-gic-delay.log','scheduler-partial.log','scheduler-loss.log','scheduler-cancel-queue.log')]
    proof={'schema':'a733-rtc-supervised-software-v1','physical_access':False,'admission_cases':True,'launcher_cases':True,'host_capture':True,
       'image':{'path':str(image),'sha256':sha(image)},'launcher':{'path':str(out/'launcher/launch.bin'),'sha256':sha(out/'launcher/launch.bin')},
       'sources':sources,'evidence':[{'path':str(path),'sha256':sha(path)} for path in evidence],
       'limits':'Real POSIX FreeRTOS service scheduling with modeled hardware; native callback/seal instructions with modeled providers. Native USB serializer source/link only. No physical qualification.'}
    save(out/'software-evidence.json',proof)
    tool.validate_software(proof,{'files':{'image.elf':sha(image),'launch.bin':sha(out/'launcher/launch.bin')},'_source_hashes':{name:value['sha256'] for name,value in sources.items()}})
    print('PASS: native RTC owner-task service software boundaries.')
if __name__=='__main__':main()
