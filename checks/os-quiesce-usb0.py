#!/usr/bin/env python3
"""Exercise USB0 return admission with the real registry and provider layout.

The owner functions are extracted unchanged from apps/os/usb0.c. Native USB
calls and task suspension are replaced at their boundary; no hardware is modeled
or qualified. A protected provider page catches reads after STARTED wins.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import subprocess

import host_rtos

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['apps/os/usb0.c', 'apps/os/quiesce.c', 'apps/os/quiesce.h',
                'apps/board/usb0_service.h', 'apps/os/grants.c', 'tools/session.py',
                'include/a733_native_usb.h', 'include/a733_usb_platform.h',
                'include/sunstone_crumb.h', 'include/sunstone_usb0_stop.h']


def function(text, name):
    match = re.search(r'^void ' + name + r'\([^;]*?\)\s*\{', text, re.M)
    assert match
    depth, end = 1, match.end()
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[match.start():end]


PRELUDE = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "a733_native_usb.h"
#include "sunstone_usb0_stop.h"
#include "quiesce.h"
static struct provider_container {struct a733_native_usb shared;} *provider_page;
#define providers (*provider_page)
static unsigned closing,failing,granted=1,fatal,parked,starts,hardware,ready,runs;
static uint32_t fatal_site,fatal_value;
static jmp_buf ending;
int os_cpu_return_requested(void){return closing;}
int os_cpu_return_failing(void){return failing;}
static int board_live(void *p){(void)p;return 1;}
static int os_granted(unsigned mask){assert(mask==64);return granted;}
#define OS_GRANT_USB0 64u
static void board_fail_at(uint32_t site,uint32_t value,const void *caller)
{assert(caller);++fatal;fatal_site=site;fatal_value=value;}
static _Noreturn void fixture_suspend(void *p){assert(!p);++parked;longjmp(ending,1);}
#define vTaskSuspend fixture_suspend
static void os_application_usb_ready(void){++ready;}
static void usb0_service_start(void);
static _Noreturn void usb0_service_run(void (*f)(void));
static void exchange(void){}
static void platform_exit(unsigned code){(void)code;abort();}
'''

CASES = r'''
static void usb0_service_start(void)
{
    assert(usb0_quiesce.registered&&atomic_load(&usb0_startup)==USB0_STARTED);
    ++starts;if(usb0_live())++hardware;
}
static _Noreturn void usb0_service_run(void (*f)(void))
{assert(f==exchange);++runs;longjmp(ending,2);}
static void begin_return(void)
{closing=1;assert(os_quiesce_register(&usb0_quiesce)==0);}
static void register_owner(void){assert(os_quiesce_register(&usb0_quiesce));}
static void dirty(unsigned field)
{
    struct a733_native_usb *p=&providers.shared;
    switch(field){
    case 0:p->usb_step=1;break;case 1:p->usb_rail_index=1;break;
    case 2:p->usb_step_result=-1;break;case 3:p->usb_io_denial=1;break;
    case 4:p->usb_provider_denial=1;break;case 5:atomic_store(&p->usb_prepared,1);break;
    case 6:p->usb.stage=1;break;case 7:p->usb.step=1;break;
    case 8:p->usb.fault=1;break;case 9:p->usb.held=1;break;
    case 10:atomic_store(&p->usb.busy,1);break;case 11:p->usb.wrote=1;break;
    case 12:p->usb.writes_attempted=1;break;case 13:p->usb.writes_completed=1;break;
    case 14:p->usb.poll_writes=1;break;case 15:p->usb.last_now=1;break;
    case 16:p->usb.wait_until=1;break;case 17:p->usb.last_address=1;break;
    case 18:p->usb.last_sample_us=1;break;case 19:p->usb.failure.kind=1;break;
    case 20:p->usb.token.id=1;break;case 21:p->usb_rail.id=1;break;
    case 22:p->usb_power.provider=(const struct a733_pmic *)(uintptr_t)1;break;
    default:assert(field<27);p->usb_request[field-23].id=1;break;
    }
}
int main(int argc,char **argv)
{
    assert(argc==2);unsigned test=(unsigned)strtoul(argv[1],NULL,0);
    size_t bytes=(sizeof(*provider_page)+4095)&~(size_t)4095;
    provider_page=mmap(NULL,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(provider_page!=MAP_FAILED);
    if(test==0){
        if(!setjmp(ending))board_usb0_task(NULL);
        assert(usb0_quiesce.registered&&starts==1&&hardware==1&&ready==1&&runs==1&&!fatal&&!parked);
    }else if(test==1){
        granted=0;if(!setjmp(ending))board_usb0_task(NULL);
        assert(!usb0_quiesce.registered&&!starts&&!hardware&&!ready&&parked==1);
    }else if(test==2||test==3){
        register_owner();providers.shared.usb.initialized=test==3;
        if(test==3)providers.shared.usb.self=(uintptr_t)&providers.shared.usb;
        begin_return();assert(os_quiesce_step(NULL)==OS_QUIESCE_DONE);
        assert(atomic_load(&usb0_startup)==USB0_SEALED&&!usb0_claim_start());
        assert(os_quiesce_step(NULL)==OS_QUIESCE_DONE&&!fatal&&!hardware);
    }else if(test==4||test==5){
        register_owner();if(test==4)assert(usb0_claim_start());else atomic_store(&usb0_startup,99);
        assert(!mprotect(provider_page,bytes,PROT_NONE));begin_return();
        assert(os_quiesce_step(NULL)==OS_QUIESCE_FAULT);
        assert(!fatal&&(usb0_quiesce.site&0xfff)==USB0_STOP_RETURN_ACTIVE);
        assert(!mprotect(provider_page,bytes,PROT_READ|PROT_WRITE));
    }else if(test>=10&&test<37){
        register_owner();dirty(test-10);begin_return();
        assert(os_quiesce_step(NULL)==OS_QUIESCE_FAULT&&!fatal);
        assert((usb0_quiesce.site&0xfff)==USB0_STOP_RETURN_PROOF);
        os_quiesce_complete(&usb0_quiesce);assert(os_quiesce_result(&usb0_quiesce)==OS_QUIESCE_FAULT);
    }else if(test==40||test==41||test==42){
        register_owner();assert(usb0_claim_start());
        if(test==41){closing=1;atomic_store(&usb0_quiesce.requested,1);atomic_store(&armed_classes,1u<<OS_QUIESCE_DEVICES);}
        else if(test==40)failing=closing=1;
        if(!setjmp(ending))stop(USB0_STOP_RECEIVE,-7);
        assert(parked==1&&!hardware);
        if(test==42)assert(fatal==1&&(fatal_site&0xfff)==USB0_STOP_RECEIVE&&fatal_value==(uint32_t)-7);
        else assert(!fatal&&os_quiesce_result(&usb0_quiesce)==OS_QUIESCE_FAULT&&usb0_quiesce.value==(uint32_t)-7);
    }else if(test==43||test==44){
        register_owner();providers.shared.usb_step=3;providers.shared.usb_rail_index=2;
        providers.shared.usb.failure.callback_result=-9;if(test==43)failing=closing=1;
        if(!setjmp(ending))board_usb0_provider_fail(NULL);
        assert(parked==1);
        uint32_t site=test==43?usb0_quiesce.site:fatal_site;
        uint32_t value=test==43?usb0_quiesce.value:fatal_value;
        assert((site&0xfff)==USB0_STOP_PROVIDER_RETAIN&&(site>>16&0x7fff)==0x203&&value==(uint32_t)-9);
        assert(fatal==(test==44));
    }else if(test==45||test==46){
        register_owner();assert(usb0_claim_start());
        if(test==45)failing=closing=1;
        else{closing=1;atomic_store(&usb0_quiesce.requested,1);atomic_store(&armed_classes,1u<<OS_QUIESCE_DEVICES);}
        if(!setjmp(ending))(void)usb0_live();
        assert(parked==1&&!fatal&&!hardware&&os_quiesce_result(&usb0_quiesce)==OS_QUIESCE_FAULT);
    }else if(test==47){
        register_owner();failing=closing=1;
        if(!setjmp(ending))stop(USB0_STOP_ASSERT,1);
        assert(!fatal&&os_quiesce_result(&usb0_quiesce)==OS_QUIESCE_FAULT);
        assert(os_quiesce_step(NULL)==OS_QUIESCE_FAULT);
    }else if(test==48){
        sealed=1;if(!setjmp(ending))board_usb0_task(NULL);
        assert(!starts&&!hardware&&parked==1&&fatal==1&&(fatal_site&0xfff)==USB0_STOP_REGISTRATION);
    }else abort();
    assert(!munmap(provider_page,bytes));return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    def load(name, path):
        spec = importlib.util.spec_from_file_location(name, path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module
    session = load('usb0_session', ROOT / 'tools/session.py')
    manifest = load('usb0_manifest', ROOT / 'tools/sunstone-manifest.py')
    device = session.quiesce_actors({'devices': manifest.DEVICES['usb-device']})
    host = session.quiesce_actors({'devices': manifest.DEVICES['usb-keyboard']})
    assert 'USB0' in device and 'USB' not in device
    assert 'USB' in host and 'USB0' not in host
    source = (ROOT / 'apps/os/usb0.c').read_text()
    body = source[source.index('enum { USB0_UNSTARTED'):source.index('/* The product ID names')]
    task = function(source, 'board_usb0_task')
    assert task.index('os_quiesce_register(&usb0_quiesce)') < task.index('usb0_claim_start') < task.index('usb0_service_start')
    assert '#define USB0_LIVE() usb0_live()' in source
    grant = (ROOT / 'apps/os/grants.c').read_text()
    assert re.search(r'\{OS_GRANT_USB0,[^}]*LIFETIME\(A733_LIFETIME_RETURN\)', grant)
    unit = PRELUDE + '\n' + (ROOT / 'apps/os/quiesce.c').read_text() + '\n' + body + '\n' + task + '\n' + CASES
    def build(name, text):
        path = out / (name+'.c')
        path.write_text(text)
        return host_rtos.build(out, [path], name, shim=False,
                               includes=[ROOT / 'apps/os'], flags=['-Wno-unused-function'])
    binary = build('usb0', unit)
    cases = [*range(6), *range(10, 37), *range(40, 49)]
    for case in cases:
        result = subprocess.run([str(binary), str(case)], capture_output=True, text=True)
        (out / ('case-%d.log' % case)).write_text(result.stdout+result.stderr)
        assert result.returncode == 0, (case, result.stderr)
    mutations = [
        ('remove-registration', 'os_quiesce_register(&usb0_quiesce))stop(USB0_STOP_REGISTRATION,0)',
         '1)stop(USB0_STOP_REGISTRATION,0)', 0),
        ('exit-in-window', 'if(!os_quiesce_fault(&usb0_quiesce,site,(uint32_t)result,__builtin_return_address(0)))',
         'platform_exit(6);if(!os_quiesce_fault(&usb0_quiesce,site,(uint32_t)result,__builtin_return_address(0)))', 40),
        ('read-started-provider', 'if(state==USB0_SEALED&&usb0_untouched())', 'if(usb0_untouched()&&state==USB0_SEALED)', 4),
        ('skip-untouched-proof', 'if(state==USB0_SEALED&&usb0_untouched())', 'if(state==USB0_SEALED)', 10),
    ]
    for name, old, new, case in mutations:
        assert unit.count(old) == 1, name
        mutant = build(name, unit.replace(old, new))
        result = subprocess.run([str(mutant), str(case)], capture_output=True, text=True)
        (out / (name+'.log')).write_text(result.stdout+result.stderr)
        assert result.returncode != 0, name
    record = {'result': 'PASS', 'hardware_access': False, 'cases': len(cases), 'mutants': len(mutations),
              'inputs': {p: hashlib.sha256((ROOT / p).read_bytes()).hexdigest() for p in [*CHECK_INPUTS, str(Path(__file__).relative_to(ROOT))]}}
    (out / 'result.json').write_text(json.dumps(record, indent=2)+'\n')
    print('PASS USB0: %d owner/registry cases, %d mutations; no hardware access' % (len(cases), len(mutations)))


if __name__ == '__main__':
    main()
