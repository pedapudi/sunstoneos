#!/usr/bin/env python3
"""Check background measurement waits and stop notification indices."""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import host_rtos
from harness import environment
ROOT=Path(__file__).resolve().parents[1]

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',required=True,type=Path)
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    spec=importlib.util.spec_from_file_location('application_events',ROOT/'checks/event-misc-application.py')
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    source=(ROOT/'runtime/bg_load.c').read_text()
    production=module.function(source,'sleep_until')+module.function(source,'vSunstoneBgStop')
    (args.output/'background-events.inc').write_text(production)
    exe=host_rtos.build(args.output/'build',['checks/event-misc-background.c','runtime/deadline.c'],
                        'background-events',includes=[args.output],defines=['configUSE_APPLICATION_TASK_TAG=1'],
                        link=['-Wl,--wrap=sunstone_deadline_wait_until'])
    for case in ([],['application-tag']):
        result=subprocess.run([str(exe),*case],capture_output=True,text=True,env=environment(),timeout=30)
        (args.output/('application-tag.log' if case else 'run.log')).write_text(result.stdout+result.stderr)
        assert result.returncode==0,result.stderr
    mutation=production.replace('configASSERT(sunstone_deadline_wait_until(until)!=SUNSTONE_DEADLINE_ERROR);','vTaskDelay(1);')
    assert mutation!=production
    (args.output/'background-events.inc').write_text(mutation)
    exe=host_rtos.build(args.output/'mutant',['checks/event-misc-background.c','runtime/deadline.c'],
                        'background-events-mutant',includes=[args.output],defines=['configUSE_APPLICATION_TASK_TAG=1'],
                        flags=['-Wno-unused-function'],
                        link=['-Wl,--wrap=sunstone_deadline_wait_until'])
    result=subprocess.run([str(exe)],capture_output=True,text=True,env=environment(),timeout=30)
    (args.output/'mutant.log').write_text(result.stdout+result.stderr);assert result.returncode!=0
    (args.output/'background-events.inc').write_text(production)
    (args.output/'result.json').write_text(json.dumps({'passed':True,'physical_access':False,'cases':2,
        'mutation_rejected':True,'unscheduled_idle_wakes':0},indent=2)+'\n')
    print('PASS background wait, tagged stop wake and tick-wake mutation')
if __name__=='__main__':main()
