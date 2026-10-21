#!/usr/bin/env python3
"""Build one AArch64 DHCP/DNS/SNTP composition and execute only its new cases."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[1]
def write_changed(path,value):
    if not path.exists() or path.read_text()!=value:path.write_text(value)
def main():
    p=argparse.ArgumentParser(description=__doc__)
    for n in ('tcp','rsn','kernel','tinyusb','headers','clang','linker','qemu','evidence'):p.add_argument('--'+n,type=Path,required=True)
    a=p.parse_args();out=a.evidence;out.mkdir(parents=True,exist_ok=True)
    spec=importlib.util.spec_from_file_location('network_pins',ROOT/'checks/radio-network.py');pins=importlib.util.module_from_spec(spec);spec.loader.exec_module(pins)
    # Aggregate independent source/tool/configuration setup errors before building.
    errors=[]
    for label,operation in [('TCP source',lambda:pins.verify(a.tcp)),('RSN source',lambda:(a.rsn/'src/rsn_supp/wpa.c').read_bytes()),('kernel',lambda:(a.kernel/'portable/GCC/ARM_AARCH64_SRE/port.c').read_bytes()),('headers',lambda:(a.headers/'stdint.h').read_bytes())]+[(n,lambda path=getattr(a,n):path.read_bytes()) for n in ('clang','linker','qemu')]:
        try:operation()
        except BaseException as ex:errors.append(label+': '+str(ex))
    assert not errors,'; '.join(errors)
    sources=['checks/radio-network-dhcp-dns.c','checks/radio-network-dhcp-dns.py','checks/radio-network.c','checks/radio-network.py','checks/aic8800-runtime.c','checks/aic8800-sdio-transport.c','checks/sdio-controller.c','drivers/net/aic8800_network.c','drivers/net/sunstone_net.c','drivers/net/sunstone_sntp.c','include/aic8800_network.h','include/sunstone_net.h','include/a733_rtc_service.h','network/include/FreeRTOSIPConfig.h','network/include/FreeRTOSConfig.h','runtime/memory.c','cmake/radio-network.cmake','cmake/a733-board.cmake','cmake/radio-dispatch.cmake','CMakeLists.txt']
    inputs={str(ROOT/x):pins.digest(ROOT/x) for x in sources};inputs.update({str(x):pins.digest(x) for x in (a.clang,a.linker,a.qemu)})
    path=out/'results.json';record=json.loads(path.read_text()) if path.exists() else {'physical_access':False,'inputs':inputs,'commands':[],'cases':{}}
    if record['inputs']!=inputs:
        assert not record.get('built') and not record['cases'],'Frozen completed batch changed.'
        record.setdefault('setup_input_history',[]).append(record['inputs']);record['inputs']=inputs
    def save():path.write_text(json.dumps(record,indent=2)+'\n')
    project=out/'project';project.mkdir(exist_ok=True)
    write_changed(project/'CMakeLists.txt',f'''cmake_minimum_required(VERSION 3.24)
project(network_clients C ASM)
set(SUNSTONE_TCP_DIR "")
set(SUNSTONE_RSN_DIR "")
set(SUNSTONE_NETWORK_C_HEADERS "")
add_subdirectory("{ROOT}" base)
set(SUNSTONE_TCP_DIR "{a.tcp}")
set(SUNSTONE_RSN_DIR "{a.rsn}")
set(SUNSTONE_NETWORK_C_HEADERS "{a.headers}")
include("{ROOT}/cmake/radio-network.cmake")
include("{ROOT}/cmake/radio-dispatch.cmake")
get_target_property(units radio-network-model SOURCES)
list(REMOVE_ITEM units "{ROOT}/checks/radio-network.c")
add_executable(radio-network-dhcp-dns-model ${{units}} "{ROOT}/checks/radio-network-dhcp-dns.c")
foreach(prop INCLUDE_DIRECTORIES COMPILE_OPTIONS LINK_OPTIONS LINK_DEPENDS COMPILE_DEFINITIONS)
 get_target_property(value radio-network-model ${{prop}})
 if(value)
  set_property(TARGET radio-network-dhcp-dns-model PROPERTY ${{prop}} "${{value}}")
 endif()
endforeach()
set_target_properties(radio-network-dhcp-dns-model PROPERTIES SUFFIX ".elf")
''')
    toolchain=out/'toolchain.cmake';write_changed(toolchain,f'''set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER "{a.clang}")
set(CMAKE_ASM_COMPILER "{a.clang}")
set(CMAKE_C_COMPILER_TARGET aarch64-none-elf)
set(CMAKE_ASM_COMPILER_TARGET aarch64-none-elf)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(SUNSTONE_LLD "{a.linker}")
set(TINYUSB_DIR "{a.tinyusb}")
set(Python3_EXECUTABLE "{sys.executable}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "--ld-path={a.linker}")
# The repository toolchain also defines the path-map helpers its CMakeLists uses.
include("{ROOT}/cmake/aarch64-clang.cmake")
''')
    if not record.get('built'):
        commands=[['cmake','-S',str(project),'-B',str(out/'build'),'-DCMAKE_TOOLCHAIN_FILE='+str(toolchain),'-DFREERTOS_DIR='+str(a.kernel),'-DSUNSTONE_TCP_DIR='+str(a.tcp),'-DSUNSTONE_RSN_DIR='+str(a.rsn),'-DSUNSTONE_NETWORK_C_HEADERS='+str(a.headers),'-DCMAKE_EXPORT_COMPILE_COMMANDS=ON'],['cmake','--build',str(out/'build'),'--target','radio-network-dhcp-dns-model','-j4']]
        for cmd in commands:
            if cmd[1]=='-S' and any(old['command']==cmd and old['exit_code']==0 for old in record['commands']):continue
            r=subprocess.run(cmd,capture_output=True,text=True);record['commands'].append({'command':cmd,'exit_code':r.returncode,'stdout':r.stdout,'stderr':r.stderr});save()
            if r.returncode:raise RuntimeError(r.stdout[-1500:]+r.stderr[-6000:])
        record['built']=True;record['image_sha256']=pins.digest(out/'build/radio-network-dhcp-dns-model.elf');save()
    # Audit the linked image and every compiler dependency before emulation.
    spec=importlib.util.spec_from_file_location('network_evidence',ROOT/'tools/evidence-snapshot.py');evidence=importlib.util.module_from_spec(spec);spec.loader.exec_module(evidence)
    dependencies,commands,records=evidence.collect_dependencies(out/'build','radio-network-dhcp-dns-model')
    record['dependencies']=dependencies;record['compile_commands']=commands;record['dependency_records']=records
    symbols=subprocess.run(['/usr/bin/llvm-nm-21','--defined-only',str(out/'build/radio-network-dhcp-dns-model.elf')],capture_output=True,text=True,check=True).stdout
    names={line.split()[-1] for line in symbols.splitlines() if line.split()}
    required={'kernel_main','sunstone_net_cancel','sunstone_sntp_poll','sunstone_sntp_rtc_request','sunstone_net_ip_step','sunstone_net_dns_bind','sunstone_net_dns_close','FreeRTOS_gethostbyname_a','vApplicationIPNetworkEventHook_Multi'}
    assert required<=names,'Missing linked network entry points: '+str(required-names)
    record['required_symbols']=sorted(required);save()
    for case in ('dhcp','acquisition','static','omitted','sntp'):
        if record['cases'].get(case,{}).get('exit_code')==0:continue
        cmd=[str(a.qemu),'-M','virt,gic-version=3,secure=off,virtualization=off','-cpu','cortex-a53','-m','128M','-nographic','-nic','none','-monitor','none','-semihosting-config','enable=on,target=native,arg='+case,'-kernel',str(out/'build/radio-network-dhcp-dns-model.elf')]
        r=subprocess.run(cmd,capture_output=True,text=True,timeout=40);record['cases'][case]={'command':cmd,'exit_code':r.returncode,'stdout':r.stdout,'stderr':r.stderr};save()
        if r.returncode:raise RuntimeError(r.stdout+r.stderr)
    record['result']='PASS new actual-stack DHCP/DNS/SNTP cases only; no physical RF, credentials, entropy, or networking qualification';save();print(record['result'])
if __name__=='__main__':main()
