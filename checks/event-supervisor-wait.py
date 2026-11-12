#!/usr/bin/env python3
"""Check precise supervisor cadence, early hints, delayed service and refusal."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

import host_rtos
from harness import CLANG, environment

ROOT=Path(__file__).resolve().parents[1]
GATE_RUNS=[['--output','{output}']]
CHECK_INPUTS=['apps/os/supervisor-wait.c','apps/os/supervisor-wait.h',
    'apps/board/main.c','apps/os/main.patch','apps/os/os-services.h','cmake/sunstone-os-core.cmake',
    'checks/event-supervisor-wait.c','checks/event-supervisor-wait.py',
    'runtime/deadline.c','include/sunstone_deadline.h','checks/host_rtos.py',
    'checks/host/host_rtos.c','checks/host/host_rtos.h','checks/host/host_deadline.c']


def binding(source):
    body=source.split('#elif defined(SUNSTONE_A733_OS)\nstatic void supervise',1)[1].split('\n#else\n',1)[0]
    assert 'if(!os_supervisor_wait_init(&wait))' in body
    assert 'if(os_supervisor_wait_next(&wait)<0)' in body
    assert 'vTaskDelay' not in body
    assert body.count('board_fail_at(SUNSTONE_CRUMB_FAIL_SITE(BOARD),0x7d')==2


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--cases',nargs='*',help='Run only named scenarios when resuming.')
    parser.add_argument('--controls',nargs='*',help='Run only named negative controls when resuming.')
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    generated=args.output/'main.c'
    patched=subprocess.run(['patch','--batch','--fuzz=0','--output',str(generated),
        str(ROOT/'apps/board/main.c'),str(ROOT/'apps/os/main.patch')],capture_output=True,text=True)
    (args.output/'patch.log').write_text(patched.stdout+patched.stderr)
    assert not patched.returncode,patched.stderr
    caller=generated.read_text();binding(caller)
    try:
        binding(caller.replace('if(os_supervisor_wait_next(&wait)<0)','if(0)'))
    except AssertionError:
        pass
    else:
        raise AssertionError('missing supervisor binding mutation passed')
    sources=['checks/event-supervisor-wait.c','apps/os/supervisor-wait.c','runtime/deadline.c']
    defines=['SUNSTONE_DEADLINE_SLOTS=2']
    link=['-Wl,--wrap=sunstone_deadline_register','-Wl,--wrap=sunstone_deadline_wait_until']
    executable=host_rtos.build(args.output/'host',sources,'supervisor-wait',defines=defines,link=link)
    cases=['cadence','early-hints','late-service','registration-failure','wait-failure','other-owner']
    if args.cases is not None:
        assert set(args.cases)<=set(cases),args.cases
        cases=args.cases
    for case in cases:
        result=subprocess.run([str(executable),case],capture_output=True,text=True,env=environment(),timeout=20)
        (args.output/(case+'.log')).write_text(result.stdout+result.stderr)
        assert not result.returncode,(case,result.stdout,result.stderr)
        print(result.stdout,end='')
    controls=[
        ('tick-poll','uint64_t until=w->next_us;','uint64_t until=sunstone_deadline_now_us()+1000;',
         'cadence','!events'),
        ('hint-defers-phase','uint64_t until=w->next_us;',
         'w->next_us=now+OS_SUPERVISOR_OBSERVATION_US;uint64_t until=w->next_us;',
         'early-hints','schedule.next_us==20123'),
        ('overdue-spin','w->next_us+=phases*OS_SUPERVISOR_OBSERVATION_US;',
         '(void)phases;','late-service','waits<64'),
        ('unlatched-error','w->failed=1;','w->failed=0;','wait-failure','schedule.failed'),
    ]
    if args.controls is not None:
        assert set(args.controls)<={row[0] for row in controls},args.controls
        controls=[row for row in controls if row[0] in args.controls]
    for name,old,new,case,diagnostic in controls:
        changed=host_rtos.mutated(args.output,'apps/os/supervisor-wait.c',old,new,name+'.c')
        binary=host_rtos.build(args.output/name,[changed if path=='apps/os/supervisor-wait.c' else path
            for path in sources],'supervisor-wait',defines=defines,includes=[ROOT/'apps/os'],link=link)
        result=subprocess.run([str(binary),case],capture_output=True,text=True,env=environment(),timeout=20)
        (args.output/(name+'-failure.log')).write_text(result.stdout+result.stderr)
        assert result.returncode and diagnostic in result.stderr,(name,result.stdout,result.stderr)
    # Use the real AArch64 FreeRTOS declarations; the helper has no device ABI.
    kernel=Path.home()/'.cache/sunstoneos/sources/FreeRTOS-Kernel-97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2'
    command=[str(CLANG),'--target=aarch64-none-elf','-std=c11','-O2','-ffreestanding','-fno-builtin',
        '-Wall','-Wextra','-Werror','-I'+str(ROOT/'include'),'-I'+str(kernel/'include'),
        '-I'+str(kernel/'portable/GCC/ARM_AARCH64_SRE'),'-c',str(ROOT/'apps/os/supervisor-wait.c'),
        '-o',str(args.output/'supervisor-wait.o')]
    result=subprocess.run(command,env=environment(),capture_output=True,text=True)
    (args.output/'native-compile.log').write_text(result.stdout+result.stderr)
    (args.output/'native-command.json').write_text(json.dumps(command,indent=2)+'\n')
    assert not result.returncode,result.stderr
    (args.output/'result.json').write_text(json.dumps(dict(passed=True,cases=cases,
        negative_controls=[name for name,*_ in controls]+['missing-caller-binding'],idle_wakes_per_second=50 if 'cadence' in cases else None,
        period_us=20000,registered_owner_slots=1,
        source_sha256={path:hashlib.sha256((ROOT/path).read_bytes()).hexdigest() for path in CHECK_INPUTS},
        limits='Actual FreeRTOS kernel, notification/deadline wait and deterministic timer; modeled scheduling and injected backend wait error. Supervisor policy, watchdog and physical timing are not executed.'),indent=2)+'\n')
    print('PASS supervisor wait:',len(cases),'scenarios,',len(controls),'negative controls and native compilation')


if __name__=='__main__':
    main()
