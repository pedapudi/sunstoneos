#!/usr/bin/env python3
"""Check the final background-worker stop publication and application fixture ABI."""
import argparse
import importlib.util
import json
import hashlib
from pathlib import Path
import subprocess
import sys
import host_rtos
from harness import environment
ROOT=Path(__file__).resolve().parents[1]
GATE_RUNS=[['--output','{output}'],['--readiness-only','--output','{output}']]
CHECK_INPUTS=['runtime/bg_load.c','include/sunstone_bg.h','include/sunstone_bg_count.h','include/sunstone_tlbi.h',
    'apps/os/tlbi-issuer.c','apps/event-waits/main.c','apps/event-waits/manifest.json','apps/event-waits/host.py',
    'apps/event-waits/cmake/application.cmake','checks/event-misc-background-stop.py','checks/event-misc-background-stop.c',
    'checks/event-misc-workload-readiness.c','checks/event-misc-application.py','checks/host_rtos.py','checks/harness.py',
    'runtime/deadline.c','checks/host/host_rtos.c','checks/host/host_deadline.c','checks/host/FreeRTOSConfig.h',
    'tools/sunstone-manifest.py','include/sunstone_app.h']
def report(output,data):
    data['inputs_sha256']={name:hashlib.sha256((ROOT/name).read_bytes()).hexdigest() for name in CHECK_INPUTS}
    (output/'result.json').write_text(json.dumps(data,indent=2)+'\n')
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',required=True,type=Path)
    p.add_argument('--native-only',action='store_true');p.add_argument('--smp-build',type=Path)
    p.add_argument('--readiness-only',action='store_true')
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    if a.readiness_only:
        spec=importlib.util.spec_from_file_location('events',ROOT/'checks/event-misc-application.py')
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        production=module.function((ROOT/'apps/event-waits/main.c').read_text(),'wait_ready')
        (a.output/'workload-readiness.inc').write_text(production)
        sources=['checks/event-misc-workload-readiness.c','runtime/deadline.c']
        exe=host_rtos.build(a.output/'build',sources,'readiness',includes=[a.output])
        for name,args in [('idle',[]),('race',['race'])]:
            r=subprocess.run([str(exe),*args],capture_output=True,text=True,env=environment(),timeout=30)
            (a.output/(name+'.log')).write_text(r.stdout+r.stderr);assert not r.returncode,r.stderr
        mutation=production.replace('ss_wait_until(finish_by);','ss_wait_until(now()+1000);')
        assert mutation!=production;(a.output/'workload-readiness.inc').write_text(mutation)
        exe=host_rtos.build(a.output/'mutant',sources,'readiness-mutant',includes=[a.output])
        r=subprocess.run([str(exe)],capture_output=True,text=True,env=environment(),timeout=30)
        (a.output/'mutant.log').write_text(r.stdout+r.stderr);assert r.returncode
        (a.output/'workload-readiness.inc').write_text(production)
        spec=importlib.util.spec_from_file_location('decoder',ROOT/'apps/event-waits/host.py')
        decoder=importlib.util.module_from_spec(spec);spec.loader.exec_module(decoder)
        fields=[decoder.MAGIC,1204000,4000,300,290,700000,680000,1,4,24,120,0,5]
        assert decoder.decode(decoder.FORMAT.pack(*fields))['stopped_workers']==24
        for index,value in [(0,0),(1,1199999),(1,3000001),(2,5000),(3,0),(5,0),(7,2),(9,23),(10,0),(11,1),(12,6)]:
            changed=fields.copy();changed[index]=value
            try:decoder.decode(decoder.FORMAT.pack(*changed))
            except ValueError:pass
            else:raise AssertionError(('decoder accepted invalid field',index,value))
        report(a.output,{'passed':True,'physical_access':False,'readiness_publication_race':True,
            'periodic_poll_mutation_rejected':True,'preemption_gap_us':250,'decoder_cases':12})
        print('PASS readiness publication waits and 80-byte decoder');return
    if a.native_only:
        if not a.smp_build:p.error('--native-only requires --smp-build naming a configured base directory')
        cache=Path.home()/'.cache/sunstoneos'
        generated=a.output/'generated'
        cmd=[sys.executable,'-B',str(ROOT/'tools/sunstone-manifest.py'),'emit','--manifest',str(ROOT/'apps/event-waits/manifest.json'),'--output',str(generated)]
        r=subprocess.run(cmd,capture_output=True,text=True);(a.output/'manifest.log').write_text(r.stdout+r.stderr);assert not r.returncode,r.stderr
        flags=[str(host_rtos.CLANG),'--target=aarch64-none-elf','-mcpu=cortex-a55','-std=c11','-O2','-Wall','-Wextra','-Werror',
            '-ffreestanding','-fno-builtin','-mgeneral-regs-only','-include','stdint.h','-DSUNSTONE_BG_LOAD=1','-DSUNSTONE_TLBI_ISSUER=0','-DSUNSTONE_TLBI_CORE=5',
            '-DconfigUSE_APPLICATION_TASK_TAG=1','-DINCLUDE_xTaskGetCurrentTaskHandle=1','-I'+str(generated),
            '-I'+str(ROOT/'platform/a733-smp-cached'),'-I'+str(ROOT/'include'),'-I'+str(ROOT/'apps/os'),
            '-I'+str(ROOT/'apps/board'),'-I'+str(ROOT/'runtime/include'),'-Wno-unused-function',
            '-I'+str(a.smp_build/'freertos-smp/include'),'-I'+str(a.smp_build/'freertos-a733-smp-cached'),
            '-isystem',str(cache/'host/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-elf/aarch64-none-elf/include')]
        commands=[cmd]
        for index,unit in enumerate(['apps/event-waits/main.c','runtime/bg_load.c','apps/os/tlbi-issuer.c']):
            command=[*flags,'-c',str(ROOT/unit),'-o',str(a.output/(str(index)+'.o'))];commands.append(command)
            r=subprocess.run(command,capture_output=True,text=True);(a.output/(str(index)+'.log')).write_text(r.stdout+r.stderr);assert not r.returncode,r.stderr
        report(a.output,{'passed':True,'physical_access':False,'commands':commands,
            'limits':'Selected native fixture and background source objects; full image requires central deadline capacity integration.'})
        print('PASS returning workload manifest and selected native source objects');return
    spec=importlib.util.spec_from_file_location('events',ROOT/'checks/event-misc-application.py')
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    source=(ROOT/'runtime/bg_load.c').read_text()
    production='\n'.join(module.function(source,n) for n in ('stopped_one','vSunstoneBgStop','ulSunstoneBgStopped'))
    (a.output/'background-stop-events.inc').write_text(production)
    exe=host_rtos.build(a.output/'build',['checks/event-misc-background-stop.c','runtime/deadline.c'],'stop',
        includes=[a.output],defines=['configUSE_APPLICATION_TASK_TAG=1'])
    for name,args in [('os',[]),('tagged',['tagged'])]:
        r=subprocess.run([str(exe),*args],capture_output=True,text=True,env=environment(),timeout=30)
        (a.output/(name+'.log')).write_text(r.stdout+r.stderr);assert not r.returncode,r.stderr
    mutation=production.replace('if(waiter)xTaskNotifyGiveIndexed(waiter,xTaskGetApplicationTaskTag(waiter)?SS_NOTIFY_DEADLINE:SS_NOTIFY_OWN);','(void)waiter;')
    assert mutation!=production;(a.output/'background-stop-events.inc').write_text(mutation)
    exe=host_rtos.build(a.output/'mutant',['checks/event-misc-background-stop.c','runtime/deadline.c'],'stop-mutant',
        includes=[a.output],defines=['configUSE_APPLICATION_TASK_TAG=1'])
    r=subprocess.run([str(exe)],capture_output=True,text=True,env=environment(),timeout=30)
    (a.output/'mutant.log').write_text(r.stdout+r.stderr);assert r.returncode
    (a.output/'background-stop-events.inc').write_text(production)
    report(a.output,{'passed':True,'physical_access':False,'os_and_tagged_waiters':True,'missing_publication_mutation_rejected':True})
    print('PASS background stop acknowledgment and missing-publication mutation')
if __name__=='__main__':main()
