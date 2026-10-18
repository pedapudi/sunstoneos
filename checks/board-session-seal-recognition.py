#!/usr/bin/env python3
"""Cover only the retained version3 profile handoff in a Mimus-enabled image."""
import argparse
import ast
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
from types import SimpleNamespace
sys.dont_write_bytecode=True
ROOT=Path(__file__).resolve().parents[1];CACHE=Path.home()/'.cache/sunstoneos'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def save(p,value):p.write_text(json.dumps(value,indent=2)+'\n')
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,required=True)
    modes=parser.add_mutually_exclusive_group(required=True);modes.add_argument('--setup-only',action='store_true');modes.add_argument('--run-only',action='store_true')
    a=parser.parse_args();out=a.output.resolve();source=out/'source'
    if a.setup_only:
        out.mkdir(parents=True,exist_ok=False)
        for name in subprocess.check_output(['git','ls-files','--cached','--others','--exclude-standard'],cwd=ROOT,text=True).splitlines():
            src=ROOT/name
            if src.is_file():dst=source/name;dst.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(src,dst)
        old=CACHE/'evidence/bluetooth-mimus-native-20261017/native-final';manifest=json.loads((old/'manifest.json').read_text());pins={}
        for record in manifest['artifacts']:
            path=Path(record['original'])
            if any(part in str(path) for part in ('/mimus-worktrees/','/newlib-smp-','/arm-gnu-toolchain-')):
                assert sha(path)==record['sha256'],path;pins[str(path)]=record['sha256']
        save(out/'reused-engine-pins.json',pins);shutil.copy2(old/'manifest.json',out/'prior-recognition-manifest.json')
    else:
        for name,digest in json.loads((out/'runtime-inputs.json').read_text()).items():assert sha(Path(name))==digest,name
    def command(label,values):
        values=list(map(str,values));save(out/(label+'-command.json'),values)
        result=subprocess.run(values,capture_output=True,env={**os.environ,'PYTHONDONTWRITEBYTECODE':'1'})
        (out/(label+'.log')).write_bytes(result.stdout+result.stderr);result.check_returncode()
    cases=module('retained_session_cases',source/'checks/board-session-seal-cases.py')
    image=out/'build/sunstone-a733-bluetooth.elf'
    if a.setup_only:
        # Reuse effective options from the preserved Mimus-enabled configuration.
        old_cache=CACHE/'build/bluetooth-mimus-native-20261017/CMakeCache.txt';shutil.copy2(old_cache,out/'prior-CMakeCache.txt')
        values={line.split(':',1)[0]:line.split('=',1)[1] for line in old_cache.read_text().splitlines() if ':' in line and '=' in line and not line.startswith('//')}
        names=['FREERTOS_DIR','TINYUSB_DIR','MIMUS_DIR','NEWLIB_ROOT','NEWLIB_SMP_ROOT','SUNSTONE_TCP_DIR','SUNSTONE_RSN_DIR','SUNSTONE_NETWORK_C_HEADERS','SUNSTONE_BT_SECURITY_DIR','SUNSTONE_LLD','CMAKE_C_COMPILER','CMAKE_CXX_COMPILER','CMAKE_ASM_COMPILER']
        command('configure',['cmake','-S',source,'-B',out/'build','-DCMAKE_TOOLCHAIN_FILE='+str(source/'cmake/aarch64-clang.cmake'),
            '-DPython3_EXECUTABLE='+sys.executable,'-DCMAKE_EXPORT_COMPILE_COMMANDS=ON','-DSUNSTONE_BOARD_SUPERVISED=ON',
            '-DSUNSTONE_BLUETOOTH_APPLICATION=ON','-DSUNSTONE_BLUETOOTH_MIMUS=ON',*['-D'+name+'='+values[name] for name in names]])
        command('native-build',['cmake','--build',out/'build','--target','sunstone-a733-bluetooth','-j4'])
        snapshot=module('recognition_snapshot',source/'tools/evidence-snapshot.py');dependencies,commands,records=snapshot.collect_dependencies(out/'build','sunstone-a733-bluetooth')
        save(out/'compiled-dependencies.json',dependencies);save(out/'compiled-commands.json',commands);save(out/'dependency-records.json',records)
        cache=(out/'build/CMakeCache.txt').read_text();assert 'SUNSTONE_BLUETOOTH_MIMUS:BOOL=ON' in cache
        # Exact compiler flags are also retained in generated flags.make records.
        flags=(out/'build/CMakeFiles/sunstone-a733-bluetooth.dir/flags.make').read_text()
        assert 'SUNSTONE_BLUETOOTH_RECOGNITION=1' in flags
        symbols,segments,layouts,identity=cases.load(image,'bluetooth')
        from elftools.elf.elffile import ELFFile
        with image.open('rb') as f:
            entries={}
            for symbol in ELFFile(f).get_section_by_name('.symtab').iter_symbols():
                if symbol.name in ('board_application_start','bluetooth_application_init','bluetooth_recognition_start','xTaskCreateStatic','bluetooth'):
                    entries.setdefault(symbol.name,set()).add(int(symbol['st_value']))
        assert all(len(entries.get(name,set()))==1 for name in ('board_application_start','bluetooth_application_init','bluetooth_recognition_start','xTaskCreateStatic','bluetooth'))
        save(out/'handoff-symbols.json',{name:next(iter(addresses)) for name,addresses in entries.items()})
        # Execute only the existing nominal producer's input-construction nodes.
        producer=source/'checks/bluetooth-mimus-host.py';tree=ast.parse(producer.read_text());body=next(n.body for n in tree.body if isinstance(n,ast.FunctionDef) and n.name=='main')
        target=lambda n,name:isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id==name for t in n.targets)
        first=next(i for i,n in enumerate(body) if target(n,'profile'));last=next(i for i,n in enumerate(body) if target(n,'session'))
        namespace={'CACHE':CACHE,'Path':Path,'struct':struct,'hashlib':hashlib,'json':json,'a':SimpleNamespace(output=out),
            'prep':module('recognition_prepare',source/'apps/bluetooth/prepare.py')}
        exec(compile(ast.Module(body=body[first:last],type_ignores=[]),str(producer),'exec'),namespace)
        profile=namespace['profile'];session=bytearray((CACHE/'evidence/board-supervised-20261017/batch1/session-fixture.bin').read_bytes())
        b=layouts['a733_board_session'];p=layouts['bluetooth_profile'];r=layouts['a733_radio_platform_config']
        for name in ('topology','sid_authority'):session[b[name]:b[name]+8]=profile[p[name]:p[name]+8]
        for name in ('fabric','fabric_hz'):profile[p['radio']+r[name]:p['radio']+r[name]+12]=session[b[name]:b[name]+12]
        session[b['sid_evidence']:b['sid_evidence']+32]=hashlib.sha256(profile).digest()
        (out/'bt-session.bin').write_bytes(session);(out/'bt-profile.bin').write_bytes(profile)
        save(out/'nominal-producer.json',{'source':str(producer),'sha256':sha(producer),'line_start':body[first].lineno,'line_end':body[last-1].end_lineno,
            'adaptations':['Native-valid board prefix','Topology/SID from profile','Fabric IDs/rates from board prefix','Recomputed profile digest','Linked image ID/contract bound at runtime'],
            'profile_sha256':sha(out/'bt-profile.bin'),'session_sha256':sha(out/'bt-session.bin'),'physical_authority':'Modeled and nondeployable.'})
        save(out/'image.json',{'sha256':sha(image),'build_id':identity.hex(),'recognition_enabled':True})
        for name,digest in dependencies.items():
            path=Path(name)
            if not path.is_relative_to(out):
                dst=out/'compiled-inputs'/str(path).lstrip('/');dst.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(path,dst);assert sha(dst)==digest
        import unicorn,elftools
        paths=[p for p in out.rglob('*') if p.is_file()]+list(map(Path,dependencies))
        paths += [Path(values[name]) for name in ('CMAKE_C_COMPILER','CMAKE_CXX_COMPILER','CMAKE_ASM_COMPILER','SUNSTONE_LLD')]+[Path(sys.executable)]
        paths += [p for package in (unicorn,elftools) for p in Path(package.__file__).parent.rglob('*') if p.is_file() and '__pycache__' not in p.parts]
        save(out/'runtime-inputs.json',{str(p):sha(p) for p in paths});print('Recognition-enabled setup complete; no native case executed.');return
    from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2
    m=cases.Machine(image,'bluetooth',out,source,(out/'bt-session.bin').read_bytes(),(out/'bt-profile.bin').read_bytes())
    assert m.call('board_session_prepare',0x43200000)==1 and m.call('board_identity_prepare',24000024)==1
    retained=m.call('bluetooth_admitted_profile');assert retained and len(m.sid)==4
    profile=(out/'bt-profile.bin').read_bytes();assert bytes(m.cpu.mem_read(retained,832))==profile
    m.cpu.mem_write(0x43000000,bytes(832));m.cpu.mem_write(0x41f00000,bytes(592))
    session=bytearray(m.raw);struct.pack_into('<2Q',session,160,60000001,24);m.cpu.mem_write(m.symbols['board'],bytes(session))
    symbols=json.loads((out/'handoff-symbols.json').read_text());m.symbols.update(symbols);observed=[]
    def handoff(address):
        if address==symbols['xTaskCreateStatic']:m.returned(0x50100000);return True
        if address==symbols['bluetooth_application_init']:
            assert m.cpu.reg_read(UC_ARM64_REG_X1)==retained
            start=m.cpu.reg_read(UC_ARM64_REG_X0+6);end=m.cpu.reg_read(UC_ARM64_REG_X0+7)
            assert (start,end)==(1,60000001)
            m.cpu.mem_write(symbols['bluetooth']+m.layouts['bluetooth_application']['deadline'],struct.pack('<Q',end-10000));m.returned(1);return True
        if address==symbols['bluetooth_recognition_start']:
            pointer=m.cpu.reg_read(UC_ARM64_REG_X1);assert pointer==retained+m.layouts['bluetooth_profile']['recognition']
            assert bytes(m.cpu.mem_read(pointer,80))==profile[752:832] and m.cpu.reg_read(UC_ARM64_REG_X2)==59990001
            observed.append(pointer);m.returned(1);return True
        return False
    m.extra=handoff;assert m.call('board_application_start')==1 and len(observed)==1
    save(out/'result.json',{'passed':True,'image_sha256':sha(image),'retained_profile':retained,'recognition_argument':observed[0],
        'scope':'Actual v3 validators, session/profile seal and recognition-guarded startup handoff; task creation, application constructor and recognizer modeled. No audio/inference replay.','physical_access':False})
    print('PASS actual retained version3 profile handoff to modeled recognition start.')
if __name__=='__main__':main()
