#!/usr/bin/env python3
"""Freeze one RTC native link and independent fixtures before new behavior checks."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
sys.dont_write_bytecode=True
sys.path.insert(0,str(Path(__file__).resolve().parent))
import harness
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
        # The preserved native H.264 producer supplies the native-valid USB-only
        # board prefix. Only the new RTC extension is modeled/nondeployable.
        original=CACHE/'evidence/h264-native-20261018/session.bin'
        data=original.read_bytes();assert len(data)==856
        raw=data[:592]+struct.pack('<5Q',0x4153373352544131,1,768,12,13)
        facts={key:json.dumps({'scope':'Software RTC model; nondeployable','fact':key},sort_keys=True).encode() for key in ('initial','clock','electrical','supervision')}
        for key,value in facts.items():(out/('model-'+key+'.json')).write_bytes(value);raw+=hashlib.sha256(value).digest()
        raw+=struct.pack('<2I',2,0);assert len(raw)==768
        (out/'source-native-session.bin').write_bytes(data);(out/'session-template.bin').write_bytes(raw)
        save(out/'fixture-derivation.json',{'producer':str(original),'producer_sha256':sha(original),'adaptations':['Preserved actual native producer USB-only board prefix','RTC extension hashes name software model records','Linked image ID and actual request/contract producer bind native case identity'],'authority':'Software model; nondeployable.'})
        save(out/'source-inputs.json',{str(p.relative_to(source)):sha(p) for p in source.rglob('*') if p.is_file()})
    else:
        for name,digest in json.loads((out/'runtime-inputs.json').read_text()).items():assert sha(Path(name))==digest,name
    steps=harness.Steps(out,'setup-commands.json' if a.setup_only else 'runtime-commands.json');command=steps.command
    compiler=harness.CLANG;linker=harness.LLD
    image=out/'build/sunstone-a733-rtc.elf'
    if a.setup_only:
        failures=[]
        try:command('driver-build',[compiler,'-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(source/'include'),source/'checks/a733-rtc.c',source/'drivers/rtc/a733_rtc.c',source/'drivers/rtc/a733_rtc_boot.c','-o',out/'driver'])
        except RuntimeError as e:failures.append(str(e))
        launcher=out/'launcher';launcher.mkdir()
        try:module('rtc_prepare_setup',source/'apps/rtc/prepare.py').build_launcher(launcher,str(compiler),str(linker))
        except Exception as e:failures.append('launcher: '+str(e))
        model=(source/'checks/a733-smp-supervised-launch.py').read_text().replace('0x4153373353554333','0x4153373352544331').replace('480000000','1440000000')
        model=model.replace('reason in (1,2,3,4,5,6,13)','reason in (1,2,3,4,5,6,13,14)')
        first=model.index('    if args.frequency_only:');last=model.index('    result =',first)
        model=model[:first]+"    cases=[check('RTC complete session handoff',12),check('RTC truncated extension refused',14,mutation=(0x41f00000+608,592))]\n"+model[last:]
        (out/'rtc-launcher-model.py').write_text(model)
        if failures:raise RuntimeError('\n'.join(failures))
        (out/'toolchain.cmake').write_text(f'set(SUNSTONE_LLD "{linker}" CACHE FILEPATH "")\ninclude("{source}/cmake/aarch64-clang.cmake")\nset(CMAKE_C_COMPILER "{compiler}")\nset(CMAKE_ASM_COMPILER "{compiler}")\n')
        deps=CACHE/'evidence/h264-native-20261018/dependencies'
        command('configure',['cmake','-S',source/'apps/rtc','-B',out/'build','-DCMAKE_TOOLCHAIN_FILE='+str(out/'toolchain.cmake'),'-DPython3_EXECUTABLE='+sys.executable,
            '-DFREERTOS_DIR='+str(deps/'freertos'),'-DTINYUSB_DIR='+str(deps/'tinyusb'),'-DSUNSTONE_TCP_DIR='+str(deps/'tcp'),'-DSUNSTONE_RSN_DIR='+str(deps/'wpa_supplicant-2.12'),'-DSUNSTONE_NETWORK_C_HEADERS='+str(deps/'headers'),'-DSUNSTONE_BOARD_SUPERVISED=ON','-DCMAKE_EXPORT_COMPILE_COMMANDS=ON'])
        command('native-build',['cmake','--build',out/'build','--target','sunstone-a733-rtc','-j4'])
        snapshot=module('rtc_evidence',source/'tools/evidence-snapshot.py')
        dependencies,actual,records=snapshot.collect_dependencies(out/'build','sunstone-a733-rtc')
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
    command('driver',[out/'driver'])
    try:module('rtc_cases_run',source/'checks/rtc-application-cases.py').run(out,source,steps.case)
    except BaseException as error:steps.failed(error);raise
    command('launcher',[sys.executable,out/'rtc-launcher-model.py','--bundle',out/'launcher','--output',out/'launcher-result.json'])
    command('host',[sys.executable,source/'checks/rtc-host.py',out])
    save(out/'result.json',{'passed':True,'physical_access':False,'image_sha256':sha(image),'scope':'Actual driver operations plus linked admission/sealing/IRQ-notification/private-copy instructions; MMIO, GIC/cache/publication, clock and USB transport remain modeled.'})
    print('PASS: native RTC composition software boundaries.')
if __name__=='__main__':main()
