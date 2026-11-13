#!/usr/bin/env python3
"""Check precise periodic waits without executing invalidation instructions."""
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
    source=(ROOT/'apps/os/tlbi-issuer.c').read_text()
    production=module.function(source,'sunstone_tlbi_sample')+module.function(source,'issuer')
    (args.output/'tlbi-events.inc').write_text(production)
    exe=host_rtos.build(args.output/'build',['checks/event-misc-tlbi.c','runtime/deadline.c'],
                        'tlbi-events',includes=[args.output],link=['-Wl,--wrap=sunstone_deadline_wait_until'])
    for case in ([],['late-alarm']):
        result=subprocess.run([str(exe),*case],capture_output=True,text=True,env=environment(),timeout=30)
        (args.output/('late-alarm.log' if case else 'run.log')).write_text(result.stdout+result.stderr)
        assert result.returncode==0,result.stderr
    mutation=production.replace('if(sunstone_deadline_wait_until(last)==SUNSTONE_DEADLINE_ERROR)ss_fail(0x760);',
                                'vTaskDelay(1);')
    assert mutation!=production
    (args.output/'tlbi-events.inc').write_text(mutation)
    exe=host_rtos.build(args.output/'mutant',['checks/event-misc-tlbi.c','runtime/deadline.c'],
                        'tlbi-events-mutant',includes=[args.output],flags=['-Wno-unused-function'],
                        link=['-Wl,--wrap=sunstone_deadline_wait_until'])
    result=subprocess.run([str(exe)],capture_output=True,text=True,env=environment(),timeout=30)
    (args.output/'mutant.log').write_text(result.stdout+result.stderr);assert result.returncode!=0
    (args.output/'tlbi-events.inc').write_text(production)
    (args.output/'result.json').write_text(json.dumps({'passed':True,'physical_access':False,'mutation_rejected':True,
        'declared_period_us':10000,'between_deadline_wakes':0},indent=2)+'\n')
    print('PASS periodic invalidation owner cadence and tick-wake mutation')
if __name__=='__main__':main()
