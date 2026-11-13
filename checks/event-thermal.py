#!/usr/bin/env python3
"""Execute thermal owner waits with the real kernel and precise deadline timer."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

import host_rtos
from harness import environment

ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--baseline-source',type=Path)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
network=Path.home()/'.cache/sunstoneos/sources/radio/stacks/FreeRTOS-Plus-TCP-b86ad086e9870099bb0ac18a75dcb7f6dd508cc7/source'
defines=['SUNSTONE_A733_FAULT_PAGE=1','SUNSTONE_A733_OS=1','SUNSTONE_BOARD_APPLICATION=1',
         'SUNSTONE_BOARD_SUPERVISED=1','SUNSTONE_BOARD_USB2_DUAL=1']
includes=[ROOT/'apps/os',ROOT/'apps/board',ROOT/'platform/a733-smp-cached',ROOT/'drivers/usb',ROOT/'network/include',
          network/'include',network/'portable/Compiler/GCC']

def build(name,extra=()):
    return host_rtos.build(a.output/name,['checks/event-thermal.c','drivers/thermal/a733_thermal.c',
        'apps/os/quiesce.c','runtime/deadline.c'],'event-thermal',defines=defines+list(extra),includes=includes,
        flags=['-Wno-error','-w','-ffunction-sections','-fdata-sections'],link=['-Wl,--gc-sections'])

def run(exe,scenario,phase):
    result=subprocess.run([str(exe),scenario,str(phase)],capture_output=True,text=True,env=environment(),timeout=20)
    assert result.returncode==0,(result.stdout,result.stderr)
    line=result.stdout.splitlines()[0]
    values={key:int(value) for key,value in (pair.split('=') for pair in line.split())}
    return values

exe=build('candidate');cases=[]
for scenario,phase in [('idle',0),('idle',123),('irq',123)]:
    cases.append(dict(scenario=scenario,**run(exe,scenario,phase)))
baseline=[]
if a.baseline_source:
    exe=build('baseline',['THERMAL_EVENT_BASELINE=1','THERMAL_SOURCE="'+str(a.baseline_source.resolve())+'"'])
    for phase in [0,123]:baseline.append(run(exe,'idle',phase))
# A timer wait replaced by one tick must fail the same idle-wake assertions.
mutant=host_rtos.mutated(a.output,'apps/os/thermal.c',
    'int result=sunstone_deadline_wait_until(idle_deadline());',
    'int result=ulTaskNotifyTake(pdTRUE,1)?SUNSTONE_DEADLINE_NOTIFIED:SUNSTONE_DEADLINE_EXPIRED;',
    'thermal-tick-wake.c')
control=build('negative-control',['THERMAL_SOURCE="'+str(mutant)+'"'])
result=subprocess.run([str(control),'idle','123'],capture_output=True,text=True,env=environment(),timeout=20)
assert result.returncode!=0,'tick-wake control passed'
(a.output/'result.json').write_text(json.dumps(dict(passed=True,cases=cases,baseline=baseline,
    negative_control='tick-wake control fails',source_sha256=hashlib.sha256((ROOT/'apps/os/thermal.c').read_bytes()).hexdigest(),
    limits='Real thermal task, FreeRTOS kernel and deadline implementation; modeled THS, GIC and CPU clock effects.'),indent=2)+'\n')
print('PASS thermal owner event waits:',cases,'baseline:',baseline)
