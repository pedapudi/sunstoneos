#!/usr/bin/env python3
"""Execute the OS headset interrupt binding and registered return on the host kernel."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

import host_rtos
from harness import environment

ROOT=Path(__file__).resolve().parents[1]
GATE_RUNS=[['--output','{output}']]
CHECK_INPUTS=['apps/os/headset.c','apps/os/quiesce.c','apps/os/quiesce.h',
    'apps/bluetooth/work-state.h','apps/bluetooth/bluetooth.h','apps/bluetooth/headset.h',
    'apps/bluetooth/security-source.py','apps/bluetooth/security-source.json',
    'checks/os-headset-uart-events.py','checks/os-headset-uart-events.c',
    'checks/host_rtos.py','checks/host/host_rtos.c','checks/host/host_deadline.c',
    'runtime/deadline.c','include/sunstone_deadline.h','drivers/uart/a733_uart.c','drivers/bluetooth/bt_h4.c']
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--cases',nargs='+',help='Run only named scenarios when resuming a check.')
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
security=a.output/'security'
subprocess.run([str(Path.home()/'.cache/sunstoneos/venv/bin/python3'),'-B',
    str(ROOT/'apps/bluetooth/security-source.py'),'--source',
    str(Path.home()/'.cache/sunstoneos/sources/bluetooth-security-zephyr-v4.2.1'),'--output',str(security)],
    check=True,env=environment())
network=Path.home()/'.cache/sunstoneos/sources/radio/stacks/FreeRTOS-Plus-TCP-b86ad086e9870099bb0ac18a75dcb7f6dd508cc7/source'
defines=['SUNSTONE_A733_OS=1','SUNSTONE_A733_OS_HEADSET_IRQ=1','SUNSTONE_A733_FAULT_PAGE=1',
    'SUNSTONE_BLUETOOTH_HEADSET=1','SUNSTONE_AIC8800_RSN=1']
includes=[ROOT/'apps/os',ROOT/'apps/board',ROOT/'apps/bluetooth',ROOT/'network/include',
    network/'include',network/'portable/Compiler/GCC',security]
sources=['checks/os-headset-uart-events.c','apps/os/quiesce.c','runtime/deadline.c',
    'drivers/uart/a733_uart.c','drivers/bluetooth/bt_h4.c']
exe=host_rtos.build(a.output/'host',sources,'headset-uart',defines=defines,includes=includes,
    flags=['-ffunction-sections','-fdata-sections'],link=['-Wl,--gc-sections'])
cases=['budget','legacy-idle','idle','short-rx','chain','backlog-tx','blocked','software-busy','sync-delay','slow-sync','stuck-sync','stuck-rearm','late-rearm',
    'rearm-race','repeat-irq','late','protocol-expiry','quiesce','stop-fault','shared']
if a.cases:
    assert set(a.cases)<=set(cases),a.cases
    cases=a.cases
logs=[]
for case in cases:
    result=subprocess.run([str(exe),case],capture_output=True,text=True,env=environment(),timeout=20)
    (a.output/(case+'.log')).write_text(result.stdout+result.stderr)
    assert result.returncode==0,(case,result.stdout,result.stderr)
    logs.append(result.stdout)
controls=[
    ('periodic-idle','return classification_deadline(until);',
     'uint64_t poll=a733_smp_counter()/24+1000;if(poll<until)until=poll;return classification_deadline(until);',
     'idle','application_calls==1'),
    ('reset-earliest','if(!atomic_load_explicit(&headset_irq.since,memory_order_relaxed))',
     'if(1)','repeat-irq','atomic_load(&headset_irq.since)==1000124'),
    ('rx-empty-rearm','headset_irq.armed=bt_h4_irq_ready(&app.hci.transport)!=0;',
     'headset_irq.armed=bt_h4_irq_ready(&app.hci.transport)!=0&&empty;',
     'short-rx','short_timeout'),
    ('clear-unclassified','if(encoded&&empty&&!unclassified){','if(encoded){',
     'repeat-irq','atomic_load(&headset_irq.since)==1000124'),
    ('drop-software-event','++os_headset_irq_record.software_passes;xTaskNotifyGive(headset.owner);',
     '++os_headset_irq_record.software_passes;',
     'chain','!layers'),
    ('rx-ready-tx-suppression','headset_irq.armed&&!headset_irq.masked&&(uart->ier&2u)&&',
     'headset_irq.armed&&','backlog-tx','!unclassified'),
    ('late-rearm-success','if(a733_smp_counter()/24>=limit)return -1;',
     'if(0)return -1;','late-rearm','retain_calls==1'),
    ('stale-retry-time','uint64_t observed=a733_smp_counter()/24;',
     'uint64_t observed=now;','slow-sync','headset_irq.retry_at==449'),
    ('premature-retirement','else if(stopped)os_quiesce_complete(&headset_quiesce);',
     'else os_quiesce_complete(&headset_quiesce);','quiesce','stop_calls==3'),
]
for name,old,new,case,diagnostic in controls:
    mutant=host_rtos.mutated(a.output,'apps/os/headset.c',old,new,name+'.c')
    binary=host_rtos.build(a.output/name,sources,'headset-uart',
        defines=defines+['HEADSET_SOURCE="'+str(mutant)+'"'],includes=includes,
        flags=['-ffunction-sections','-fdata-sections'],link=['-Wl,--gc-sections'])
    result=subprocess.run([str(binary),case],capture_output=True,text=True,env=environment(),timeout=20)
    (a.output/(name+'-failure.log')).write_text(result.stdout+result.stderr)
    assert result.returncode!=0 and diagnostic in result.stderr,(name,result.stdout,result.stderr)
# Restoring the destructive quiet query must erase BUSY before the deferred
# upper pass reaches the actual UART IIR path, and fail this binding case.
query=host_rtos.mutated(a.output,'drivers/uart/a733_uart.c',
    'if(!read_reg(u,RFL,&status))return -1;\n    return !(status&0x1ffu);',
    'if(!read_reg(u,USR,&status))return -1;\n    return !(status&8u);','uart-destructive-quiet.c')
binary=host_rtos.build(a.output/'destructive-quiet',
    [query if name=='drivers/uart/a733_uart.c' else name for name in sources],
    'headset-uart',defines=defines,includes=includes,
    flags=['-ffunction-sections','-fdata-sections'],link=['-Wl,--gc-sections'])
result=subprocess.run([str(binary),'software-busy'],capture_output=True,text=True,env=environment(),timeout=20)
(a.output/'destructive-quiet-failure.log').write_text(result.stdout+result.stderr)
assert result.returncode!=0 and 'uart_busy&&uart_rfl_reads==1' in result.stderr,result.stderr
(a.output/'result.json').write_text(json.dumps(dict(passed=True,cases=cases,
    idle_wakes_per_second=0 if 'idle' in cases else None,
    legacy_idle_wakes_per_second=1000 if 'legacy-idle' in cases else None,
    pre_irq_us=3414,receive_budget_us=5000,post_irq_us=1586,
    negative_controls=[name for name,*_ in controls]+['destructive-quiet'],
    source_sha256={name:hashlib.sha256((ROOT/name).read_bytes()).hexdigest() for name in CHECK_INPUTS},
    limits='Actual OS headset binding, FreeRTOS kernel, precise timer and quiesce registry; modeled UART/GIC and application boundaries. No physical headset qualification.'),indent=2)+'\n')
print(''.join(logs),end='');print('PASS native headset event binding:',len(cases),'cases and',len(controls)+1,'negative controls')
