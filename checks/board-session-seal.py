#!/usr/bin/env python3
"""Freeze two affected native links and run only new session/profile binding cases."""
import argparse
import ast
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
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
    spec=importlib.util.spec_from_file_location(name,path);result=importlib.util.module_from_spec(spec);spec.loader.exec_module(result);return result
def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,required=True)
    mode=parser.add_mutually_exclusive_group(required=True);mode.add_argument('--setup-only',action='store_true');mode.add_argument('--run-only',action='store_true')
    args=parser.parse_args();out=args.output.resolve();source=out/'source'
    if args.setup_only:
        out.mkdir(parents=True,exist_ok=False)
        for name in subprocess.check_output(['git','ls-files','--cached','--others','--exclude-standard'],cwd=ROOT,text=True).splitlines():
            src=ROOT/name
            if src.is_file():
                dst=source/name;dst.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(src,dst)
        original=CACHE/'evidence/bluetooth-native-20261017/host-capture/binding-model'
        base=CACHE/'evidence/board-supervised-20261017/batch1/session-fixture.bin'
        shutil.copy2(base,out/'native-base-session.bin');shutil.copy2(original/'profile.bin',out/'component-profile.bin')
        save(out/'fixture-inputs.json',{str(base):sha(out/'native-base-session.bin'),str(original/'profile.bin'):sha(out/'component-profile.bin')})
        assert len((out/'native-base-session.bin').read_bytes())==592 and len((out/'component-profile.bin').read_bytes())==624
        ast.parse((source/'checks/board-session-seal-cases.py').read_text())
        save(out/'source-inputs.json',{str(p.relative_to(source)):sha(p) for p in source.rglob('*') if p.is_file()})
    else:
        for name,digest in json.loads((out/'runtime-inputs.json').read_text()).items():assert sha(Path(name))==digest,name
    steps=harness.Steps(out,'setup-commands.json' if args.setup_only else 'runtime-commands.json');command=steps.command
    compiler=harness.CLANG;linker=harness.LLD
    if args.setup_only:
        configuration=out/'configuration';configuration.mkdir()
        text=(source/'apps/board/CMakeLists.txt').read_text()
        text=text.replace('get_filename_component(SUNSTONE_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)',f'set(SUNSTONE_SOURCE_ROOT "{source}")')
        text+='\ninclude("${SUNSTONE_SOURCE_ROOT}/cmake/a733-bluetooth.cmake")\n'
        (configuration/'CMakeLists.txt').write_text(text)
        (out/'toolchain.cmake').write_text(f'set(SUNSTONE_LLD "{linker}" CACHE FILEPATH "")\ninclude("{source}/cmake/aarch64-clang.cmake")\nset(CMAKE_C_COMPILER "{compiler}")\nset(CMAKE_ASM_COMPILER "{compiler}")\n')
        dependencies=CACHE/'evidence/h264-native-20261018/dependencies'
        command('configure',['cmake','-S',configuration,'-B',out/'build','-DCMAKE_TOOLCHAIN_FILE='+str(out/'toolchain.cmake'),'-DPython3_EXECUTABLE='+sys.executable,
            '-DFREERTOS_DIR='+str(dependencies/'freertos'),'-DTINYUSB_DIR='+str(dependencies/'tinyusb'),'-DSUNSTONE_TCP_DIR='+str(dependencies/'tcp'),
            '-DSUNSTONE_RSN_DIR='+str(dependencies/'wpa_supplicant-2.12'),'-DSUNSTONE_NETWORK_C_HEADERS='+str(dependencies/'headers'),
            '-DSUNSTONE_BT_SECURITY_DIR='+str(CACHE/'sources/bluetooth-security-zephyr-v4.2.1'),'-DSUNSTONE_BOARD_SUPERVISED=ON','-DCMAKE_EXPORT_COMPILE_COMMANDS=ON'])
        command('native-build',['cmake','--build',out/'build','--target','sunstone-a733-board','sunstone-a733-bluetooth','-j4'])
        snapshot=module('session_evidence',source/'tools/evidence-snapshot.py');cases=module('session_cases',source/'checks/board-session-seal-cases.py')
        closures={};preflight={}
        for kind in ('board','bluetooth'):
            target='sunstone-a733-'+kind
            dependencies,commands,records=snapshot.collect_dependencies(out/'build',target)
            save(out/(kind+'-dependencies.json'),dependencies);save(out/(kind+'-commands.json'),commands);save(out/(kind+'-dependency-records.json'),records)
            closures.update(dependencies)
            symbols,segments,layouts,identity=cases.load(out/'build'/(target+'.elf'),kind)
            preflight[kind]={'symbols':symbols,'image':sha(out/'build'/(target+'.elf')),'build_id':identity.hex()}
            for name in ('board_application','aarch64_translation'):assert name in layouts
        save(out/'preflight.json',preflight)
        cases.derive_nominal(out)
        for name,digest in closures.items():
            path=Path(name)
            if not path.is_relative_to(out):
                dst=out/'compiled-inputs'/str(path).lstrip('/');dst.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(path,dst);assert sha(dst)==digest
        import unicorn,elftools
        tools=[compiler,linker,Path(sys.executable),*[Path(shutil.which(name)) for name in ('cmake','make','patch')]]
        save(out/'runtime-inputs.json',{str(p):sha(p) for p in [*[p for p in out.rglob('*') if p.is_file() and p.name not in ('steps.json','runtime-inputs.json')],*map(Path,closures),*tools,
            *[p for package in (unicorn,elftools) for p in Path(package.__file__).parent.rglob('*') if p.is_file() and '__pycache__' not in p.parts]]})
        print('Setup passed; no runtime cases executed.');return
    cases=module('session_cases',source/'checks/board-session-seal-cases.py')
    try:cases.run(out,source,steps.case)
    except BaseException as error:steps.failed(error);raise
    save(out/'result.json',{'passed':True,'physical_access':False,'scope':'Actual linked session/profile copying, hashing, identity and provider admission; EL3/cache/SID/translation/provider work are modeled boundaries.'})
    print('PASS retained authenticated board/Bluetooth sessions and profiles.')
if __name__=='__main__':main()
