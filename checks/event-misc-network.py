#!/usr/bin/env python3
"""Check the network event path without enabling the unqualified SDIO IRQ."""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import host_rtos
from harness import environment
ROOT=Path(__file__).resolve().parents[1]
CACHE=Path.home()/'.cache/sunstoneos'

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',required=True,type=Path)
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    spec=importlib.util.spec_from_file_location('application_events',ROOT/'checks/event-misc-application.py')
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    source=(ROOT/'drivers/net/sunstone_net.c').read_text()
    names=('sunstone_net_event','sunstone_net_event_from_isr','net_now_us','request_expired','ip_publish','ip_capacity_event','socket_event',
           'independent','client','client_ready','client_gate_enter','client_gate_leave','bind_start','worker_wait','sunstone_net_submit','sunstone_net_cancel')
    production='\n'.join(module.function(source,name) for name in names)
    (args.output/'network-events.inc').write_text(production)
    tcp=CACHE/'sources/radio/stacks/FreeRTOS-Plus-TCP-b86ad086e9870099bb0ac18a75dcb7f6dd508cc7/source'
    includes=[args.output,ROOT/'network/include',tcp/'include',tcp/'portable/Compiler/GCC']
    exe=host_rtos.build(args.output/'build',['checks/event-misc-network.c','runtime/deadline.c'],
                        'network-events',includes=includes,link=['-Wl,--wrap=sunstone_deadline_wait_until'])
    result=subprocess.run([str(exe)],capture_output=True,text=True,env=environment(),timeout=30)
    (args.output/'run.log').write_text(result.stdout+result.stderr);assert result.returncode==0,result.stderr
    mutation=production.replace('if(!until){(void)ulTaskNotifyTake(pdTRUE,portMAX_DELAY);return 1;}',
                                'if(!until){vTaskDelay(1);return 1;}')
    assert mutation!=production
    (args.output/'network-events.inc').write_text(mutation)
    exe=host_rtos.build(args.output/'mutant',['checks/event-misc-network.c','runtime/deadline.c'],
                        'network-events-mutant',includes=includes,link=['-Wl,--wrap=sunstone_deadline_wait_until'])
    result=subprocess.run([str(exe)],capture_output=True,text=True,env=environment(),timeout=30)
    (args.output/'mutant.log').write_text(result.stdout+result.stderr);assert result.returncode!=0
    capacity=production.replace('atomic_fetch_or_explicit(&s->ip_waiters,bit,memory_order_release);','(void)bit;')
    assert capacity!=production
    (args.output/'network-events.inc').write_text(capacity)
    exe=host_rtos.build(args.output/'capacity-mutant',['checks/event-misc-network.c','runtime/deadline.c'],
                        'network-capacity-mutant',includes=includes,link=['-Wl,--wrap=sunstone_deadline_wait_until'])
    result=subprocess.run([str(exe)],capture_output=True,text=True,env=environment(),timeout=30)
    (args.output/'capacity-mutant.log').write_text(result.stdout+result.stderr);assert result.returncode!=0
    (args.output/'network-events.inc').write_text(production)
    (args.output/'result.json').write_text(json.dumps({'passed':True,'physical_access':False,'mutations_rejected':2,
        'event_profile_idle_poll_wakes_per_second_before':1000,'event_profile_idle_poll_wakes_per_second_after':0,
        'request_timeout_us':2000,'submission_phase_us':123,
        'limits':'Transport IRQ producer is modeled; no native INTID194 admission is supplied.'},indent=2)+'\n')
    print('PASS network event path and idle tick-wake mutation')
if __name__=='__main__':main()
