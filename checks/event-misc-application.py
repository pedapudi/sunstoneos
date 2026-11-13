#!/usr/bin/env python3
"""Exercise the application's production event waits with the host kernel."""
import argparse
import json
from pathlib import Path
import subprocess
import host_rtos
from harness import environment

ROOT=Path(__file__).resolve().parents[1]

def function(text,name):
    start=text.index(name+'(')
    start=text.rfind('\n',0,start)+1
    opening=text.index('{',start)
    depth=1
    end=opening+1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}')
        end+=1
    return text[start:end]+'\n'

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',required=True,type=Path)
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    source=(ROOT/'apps/os/application.c').read_text()
    names=('os_wake','os_wait_begin','os_wait_end','application_wait_until_unless','application_wait_for')
    production='\n'.join(function(source,name) for name in names)
    (args.output/'application-events.inc').write_text(production)
    exe=host_rtos.build(args.output/'build',['checks/event-misc-application.c','runtime/deadline.c'],
                        'application-events',includes=[args.output],link=['-Wl,--wrap=sunstone_deadline_wait_until'])
    for case in ([],['race']):
        result=subprocess.run([str(exe),*case],text=True,capture_output=True,env=environment(),timeout=30)
        (args.output/('race.log' if case else 'idle.log')).write_text(result.stdout+result.stderr)
        assert result.returncode==0,result.stderr
    # Reintroduce a tick-driven predicate check in the production wait.
    mutation=production.replace('if(application_wait_until_unless(until,done)==SUNSTONE_DEADLINE_ERROR)ss_fail(failure);',
                                'vTaskDelay(1);')
    assert mutation!=production
    (args.output/'application-events.inc').write_text(mutation)
    mutant=host_rtos.build(args.output/'mutant',['checks/event-misc-application.c','runtime/deadline.c'],
                           'application-events-mutant',includes=[args.output],flags=['-Wno-unused-function'],
                           link=['-Wl,--wrap=sunstone_deadline_wait_until'])
    result=subprocess.run([str(mutant)],text=True,capture_output=True,env=environment(),timeout=30)
    (args.output/'mutant.log').write_text(result.stdout+result.stderr)
    assert result.returncode!=0,'tick-wake mutation passes'
    (args.output/'application-events.inc').write_text(production)
    (args.output/'result.json').write_text(json.dumps({'passed':True,'physical_access':False,'cases':2,
        'idle_poll_wakes_per_second_before':1000,'idle_poll_wakes_per_second_after':0,
        'scope':'Real application wait functions and deadline runtime; modeled producer and session.',
        'mutation_rejected':True},indent=2)+'\n')
    print('PASS application idle, publication race and tick-wake mutation')

if __name__=='__main__':main()
