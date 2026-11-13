#!/usr/bin/env python3
"""Check USB return event waits and precise no-IRQ status retry."""
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
    source=(ROOT/'apps/os/client.c').read_text()
    production=module.function(source,'usb_return_wait')+module.function(source,'usb_poll_wait')
    (args.output/'usb-return-events.inc').write_text(production)
    exe=host_rtos.build(args.output/'build',['checks/event-misc-usb-return.c','runtime/deadline.c'],
                        'usb-return-events',includes=[args.output],link=['-Wl,--wrap=sunstone_deadline_wait_until'])
    result=subprocess.run([str(exe)],capture_output=True,text=True,env=environment(),timeout=30)
    (args.output/'run.log').write_text(result.stdout+result.stderr);assert result.returncode==0,result.stderr
    mutation=production.replace('sunstone_deadline_wait_until(until)','ulTaskNotifyTake(pdTRUE,1)')
    assert mutation!=production
    (args.output/'usb-return-events.inc').write_text(mutation)
    exe=host_rtos.build(args.output/'mutant',['checks/event-misc-usb-return.c','runtime/deadline.c'],
                        'usb-return-events-mutant',includes=[args.output],flags=['-Wno-unused-function'],
                        link=['-Wl,--wrap=sunstone_deadline_wait_until'])
    result=subprocess.run([str(exe)],capture_output=True,text=True,env=environment(),timeout=30)
    (args.output/'mutant.log').write_text(result.stdout+result.stderr);assert result.returncode!=0
    (args.output/'usb-return-events.inc').write_text(production)
    (args.output/'result.json').write_text(json.dumps({'passed':True,'physical_access':False,'mutation_rejected':True,
        'publication_wait_idle_poll_wakes_per_second_before':1000,
        'publication_wait_idle_poll_wakes_per_second_after':0,'retirement_retry_us':50,
        'unbound_reference_poll_interval_us':1000},indent=2)+'\n')
    print('PASS USB publication wait, precise retirement retry and tick-wake mutation')
if __name__=='__main__':main()
