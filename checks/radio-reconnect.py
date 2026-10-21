#!/usr/bin/env python3
"""Build or execute only the affected retained-session reconnect composition."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import socket
import time
import shlex

ROOT=Path(__file__).resolve().parents[1]
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--evidence',type=Path,required=True)
    parser.add_argument('--accepted',type=Path,default=Path.home()/'.cache/sunstoneos/evidence/radio-session-20261016/initial')
    parser.add_argument('--phase',choices=['build','run'],required=True)
    args=parser.parse_args();out=args.evidence.resolve();out.mkdir(parents=True,exist_ok=True)
    prior=json.loads((args.accepted/'results.json').read_text())
    if args.phase=='build':
        assert not (out/'build').exists(),'Preserve an existing build before changing executable inputs'
        configure=prior['commands'][0]['command']
        options=[word for word in configure if word.startswith('-D') and not word.startswith('-DCMAKE_TOOLCHAIN_FILE=')]
        options+=['-DSUNSTONE_LLD=/home/sunil/.cache/sunstoneos/host/usr/bin/ld.lld-21']
        project=out/'project';project.mkdir()
        (project/'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.24)
project(radio_reconnect C ASM)
add_subdirectory("{ROOT}" base)
get_target_property(inputs radio-session-model SOURCES)
list(FILTER inputs EXCLUDE REGEX "/checks/a733-radio-session[.]c$")
set_property(TARGET radio-session-model PROPERTY SOURCES "${{inputs}}")
target_sources(radio-session-model PRIVATE "{ROOT}/checks/radio-reconnect.c" "{ROOT}/apps/board/radio.c")
set_source_files_properties("{ROOT}/checks/radio-reconnect.c" "{ROOT}/apps/board/radio.c" TARGET_DIRECTORY radio-session-model PROPERTIES COMPILE_OPTIONS "-include;stdint.h")
get_target_property(link_options radio-session-model LINK_OPTIONS)
list(FILTER link_options EXCLUDE REGEX "--wrap=(FreeRTOS_accept|prvHandleListen_IPV4)$")
set_property(TARGET radio-session-model PROPERTY LINK_OPTIONS "${{link_options}}")
target_link_options(radio-session-model PRIVATE "-Wl,--undefined=board_radio_reconnect")
''')
        toolchain=out/'toolchain.cmake'
        toolchain.write_text('set(CMAKE_SYSTEM_NAME Generic)\nset(CMAKE_SYSTEM_PROCESSOR aarch64)\nset(CMAKE_C_COMPILER /usr/bin/clang-21)\nset(CMAKE_ASM_COMPILER /usr/bin/clang-21)\nset(CMAKE_C_COMPILER_TARGET aarch64-none-elf)\nset(CMAKE_ASM_COMPILER_TARGET aarch64-none-elf)\nset(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)\nset(CMAKE_EXE_LINKER_FLAGS_INIT "--ld-path=/home/sunil/.cache/sunstoneos/host/usr/bin/ld.lld-21")\n')
        result={'accepted_lower_evidence':str(args.accepted.resolve()),'accepted_result_sha256':sha(args.accepted/'results.json'),'commands':[]}
        commands=[['cmake','-G','Unix Makefiles','-S',str(project),'-B',str(out/'build'),f'-DCMAKE_TOOLCHAIN_FILE={toolchain}','-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',*options],
                  ['cmake','--build',str(out/'build'),'--target','radio-session-model','-j4']]
        for command in commands:
            run=subprocess.run(command,capture_output=True,text=True)
            result['commands'].append({'command':command,'returncode':run.returncode,'stdout':run.stdout,'stderr':run.stderr})
            (out/'build-result.json').write_text(json.dumps(result,indent=2)+'\n')
            if run.returncode:raise SystemExit(run.returncode)
        matches=list((out/'build').rglob('radio-session-model.elf'));assert len(matches)==1,matches
        elf=matches[0];artifact=out/('radio-reconnect-'+sha(elf)+'.elf');shutil.copyfile(elf,artifact)
        result.update(artifact=str(artifact),elf_sha256=sha(artifact))
        (out/'build-result.json').write_text(json.dumps(result,indent=2)+'\n');print(artifact)
    else:
        assert not (out/'result.json').exists(),'Preserve an earlier run before another execution'
        build=json.loads((out/'build-result.json').read_text());elf=Path(build['artifact']);assert sha(elf)==build['elf_sha256']
        command=prior['runs'][0]['command'][:];command[-1]=str(elf)
        monitor=out/'qmp.sock';command+=['-qmp',f'unix:{monitor},server=on,wait=off']
        stdout=out/'stdout.log';stderr=out/'stderr.log'
        with stdout.open('w') as output,stderr.open('w') as errors:
            process=subprocess.Popen(command,stdout=output,stderr=errors)
            try:returncode=process.wait(timeout=90)
            except subprocess.TimeoutExpired:
                # Leave the same process alive. Inspect its recorded final
                # status before resumption; migration may leave postmigrate.
                diagnostic={'pid':process.pid,'qmp':str(monitor),'elf_sha256':sha(elf)}
                try:
                    connection=socket.socket(socket.AF_UNIX);connection.connect(str(monitor))
                    channel=connection.makefile('rwb',buffering=0);json.loads(channel.readline())
                    def qmp(name,arguments=None):
                        channel.write((json.dumps({'execute':name,'arguments':arguments or {}})+'\n').encode())
                        while True:
                            response=json.loads(channel.readline())
                            if 'return' in response or 'error' in response:return response
                    diagnostic['capabilities']=qmp('qmp_capabilities')
                    diagnostic['stop']=qmp('stop');diagnostic['stopped_status']=qmp('query-status')
                    assert 'return' in diagnostic['stop'] and diagnostic['stopped_status']['return']['status']=='paused'
                    diagnostic['registers']=qmp('human-monitor-command',{'command-line':'info registers'})
                    state=out/'unfinished-machine.bin'
                    diagnostic['migration']=qmp('migrate',{'uri':'exec:cat > '+shlex.quote(str(state))})
                    for _ in range(100):
                        diagnostic['migration_status']=qmp('query-migrate')
                        if diagnostic['migration_status'].get('return',{}).get('status') in ['completed','failed']:break
                        time.sleep(.05)
                    if diagnostic['migration_status'].get('return',{}).get('status')=='completed':
                        diagnostic['checkpoint']={'path':str(state),'sha256':sha(state)}
                    diagnostic['final_status']=qmp('query-status');connection.close()
                except Exception as error:diagnostic['preservation_error']=repr(error)
                (out/'unfinished.json').write_text(json.dumps(diagnostic,indent=2)+'\n')
                returncode=None
        result={'command':command,'elf_sha256':sha(elf),'returncode':returncode,'stdout':stdout.read_text(),'stderr':stderr.read_text(),
                'scope':'Actual native FreeRTOS/provider/session/offline/reconnect/socket paths; MMIO, firmware initial completion, initial association and entropy modeled. OPEN/fixed address reconnect, not complete WPA2 handshake or hardware qualification.'}
        (out/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(result['stdout'],end='');print(result['stderr'],end='')
        assert returncode==0 and 'PASS fixed-address healthy reconnect' in result['stdout']
if __name__=='__main__':main()
