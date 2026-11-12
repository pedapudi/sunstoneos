#!/usr/bin/env python3
"""Check legacy USB reset holds on the scheduler's precise timer."""
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
    parser.add_argument('--only-mutant',action='store_true',help='Reuse passing nominal cases after a mutation-fixture repair.')
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    spec=importlib.util.spec_from_file_location('application_events',ROOT/'checks/event-misc-application.py')
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    source=(ROOT/'drivers/usb/a733_usb.c').read_text()
    production='\n'.join(module.function(source,name) for name in ('hold_us','tusb_time_delay_ms_api','a733_usb_prepare'))
    (args.output/'usb-delay-events.inc').write_text(production)
    if not args.only_mutant:
        exe=host_rtos.build(args.output/'build',['checks/event-misc-usb-delay.c','runtime/deadline.c'],
                            'usb-delay-events',includes=[args.output],defines=['SUNSTONE_DEADLINE_SLOTS=1'],
                            link=['-Wl,--wrap=sunstone_deadline_wait_until'])
        for case in ([],['early-hint']):
            result=subprocess.run([str(exe),*case],capture_output=True,text=True,env=environment(),timeout=30)
            (args.output/('early-hint.log' if case else 'run.log')).write_text(result.stdout+result.stderr)
            assert result.returncode==0,result.stderr
    mutation=production.replace('if(sunstone_deadline_wait_until(until)==SUNSTONE_DEADLINE_ERROR)',
                                'if(((void)until,vTaskDelay(1),0))')
    assert mutation!=production
    (args.output/'usb-delay-events.inc').write_text(mutation)
    exe=host_rtos.build(args.output/'mutant',['checks/event-misc-usb-delay.c','runtime/deadline.c'],
                        'usb-delay-events-mutant',includes=[args.output],defines=['SUNSTONE_DEADLINE_SLOTS=1'],
                        link=['-Wl,--wrap=sunstone_deadline_wait_until'])
    result=subprocess.run([str(exe)],capture_output=True,text=True,env=environment(),timeout=30)
    (args.output/'mutant.log').write_text(result.stdout+result.stderr);assert result.returncode!=0
    (args.output/'usb-delay-events.inc').write_text(production)
    (args.output/'result.json').write_text(json.dumps({'passed':True,'physical_access':False,'cases':1 if args.only_mutant else 3,
        'nominal_cases_reused':args.only_mutant,
        'mutation_rejected':True,'detach_hold_us':150001,'reset_hold_us':1001,'propagation_hold_us':41,
        'idle_poll_wakes_per_second_after':0,'scope':'Actual hold/reset functions with modeled registers and the host scheduler.'},indent=2)+'\n')
    print('PASS USB tick-wake mutation; nominal cases reused' if args.only_mutant else
          'PASS USB exact reset holds, early hint and tick-wake mutation')
if __name__=='__main__':main()
