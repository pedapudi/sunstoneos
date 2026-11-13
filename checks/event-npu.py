#!/usr/bin/env python3
"""Exercise NPU idle and IRQ waits with the real FreeRTOS and deadline runtime."""
import argparse
import json
import importlib.util
import struct
from pathlib import Path
import subprocess
import host_rtos
from harness import environment

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['checks/event-npu-service.c', 'drivers/npu/npu_service.c',
                'include/npu_service.h', 'drivers/npu/a733_npu_controller.c',
                'runtime/deadline.c', 'apps/os/npu.c', 'apps/os/npu-client.c',
                'apps/os/cpu-return-host.py', 'platform/a733/npu_native.c',
                'drivers/npu/a733_npu_startup.c', 'include/a733_pll_lock.h']

def holds(output):
    native = (ROOT / 'platform/a733/npu_native.c').read_text()
    delay = native[native.index('static void delay(void *cookie, unsigned us)'):native.index('\nstatic int publish')]
    startup = (ROOT / 'drivers/npu/a733_npu_startup.c').read_text()
    pll = startup[startup.index('static int pll_pause('):startup.index('\n#else\n#define pll_wait')]
    lock_now = startup[startup.index('static int lock_now('):startup.index('\nstatic int lock_wait(')]
    controller = (ROOT / 'drivers/npu/a733_npu_controller.c').read_text()
    reset = controller[controller.index('static int reset('):controller.index('\nint a733_npu_open(')]
    deadline = controller[controller.index('static int deadline('):controller.index('\nstatic int check_configuration(')]
    unit = output / 'holds.c'
    unit.write_text(r'''
#include "host_rtos.h"
#include "sunstone_deadline.h"
#include "a733_pll_lock.h"
#include "a733_npu_controller.h"
#include "a733_npu_startup.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <setjmp.h>
struct a733_npu_native { unsigned unused; };
static unsigned stopped;
static jmp_buf fault;
static uint64_t microseconds(void *n){(void)n;return stopped?0:host_time_us();}
static _Noreturn void fatal(struct a733_npu_native *n){(void)n;longjmp(fault,1);}
''' + delay + '\n' + pll + r'''
static unsigned reset_writes;
static int fail(struct a733_npu_controller *c,enum a733_npu_fault why){c->state=A733_NPU_FAULT;c->fault=why;return 0;}
static int write_reg(struct a733_npu_controller *c,unsigned offset,uint32_t value){(void)c;(void)offset;(void)value;++reset_writes;return 1;}
static int read_reg(struct a733_npu_controller *c,unsigned offset,uint32_t *value){(void)c;(void)offset;*value=0;return 1;}
static int verify(struct a733_npu_controller *c,unsigned offset,uint32_t want,uint32_t mask){(void)c;(void)offset;(void)want;(void)mask;return 1;}
static int idle(struct a733_npu_controller *c){(void)c;return 1;}
static void reset_delay(void *cookie,unsigned us){struct a733_npu_native n={0};(void)cookie;delay(&n,us);}
''' + deadline + '\n' + reset + r'''
static uint64_t last_read;
static unsigned reads;
static int pll_read(void *cookie,uint32_t *out){
    (void)cookie;
    if(reads)assert(host_time_us()-last_read>=A733_PLL_LOCK_GAP_US+1);
    last_read=host_time_us();++reads;*out=A733_PLL_LOCK_BIT|7;return 1;
}
static int pll_now(void *cookie,uint64_t *out){(void)cookie;*out=host_time_us();return 1;}
static int now(struct a733_npu_startup *s,uint64_t *out){if(out)*out=host_time_us();return host_time_us()<s->config.deadline_us;}
static uint64_t spin_clock(void *cookie){(void)cookie;return host_time_us();}
struct startup_lock_sample { struct a733_npu_startup *startup; uint32_t last; };
''' + lock_now + r'''
static void early(void *owner){xTaskNotifyGive(owner);}
static void test(void *unused){
    (void)unused;assert(sunstone_deadline_register());
    struct a733_npu_native n={0};
    const unsigned intervals[]={3,10,20,50,100,1000};
    for(unsigned i=0;i<sizeof(intervals)/sizeof(*intervals);++i){
        const uint64_t start=host_time_us();
        host_run_at(start+1,early,xTaskGetCurrentTaskHandle());
        delay(&n,intervals[i]);
        assert(host_time_us()==start+intervals[i]+1);
    }
    const uint64_t start=host_time_us();
    assert(pll_wait(pll_read,pll_now,NULL,7));
    assert(reads==3&&host_time_us()-start==29);
    struct a733_npu_startup startup={.config={.deadline_us=host_time_us()+10},.ops={.spin_us=spin_clock}};
    struct startup_lock_sample sample={.startup=&startup};
    reads=0;
    host_timer_hold_until(host_time_us()+100);
    assert(!pll_wait(pll_read,lock_now,&sample,7)&&reads==1);
    host_timer_hold_until(0);
    struct a733_npu_controller controller={.state=A733_NPU_COLD,
        .start=host_time_us(),.timeout=10,.ops={.microseconds=microseconds,.delay=reset_delay}};
    host_timer_hold_until(host_time_us()+100);
    assert(!reset(&controller)&&controller.fault==A733_NPU_TIMEOUT&&reset_writes==5);
    host_timer_hold_until(0);
    stopped=1;
    if(!setjmp(fault)){delay(&n,10);assert(0);}
    puts("PASS NPU vendor holds use compare deadlines, tolerate early events, preserve PLL sample gaps and reject a stopped source clock");
}
int main(void){host_timer_handler(sunstone_deadline_irq);host_rtos_run(test,NULL,2,1000000);return 0;}
''')
    exe = host_rtos.build(output / 'holds', [unit, 'runtime/deadline.c'], 'holds',
                          defines=['SUNSTONE_A733_OS=1', 'SUNSTONE_DEADLINE_SLOTS=4'])
    done = host_rtos.run(exe)
    (output / 'holds.log').write_text(done.stdout + done.stderr)
    assert done.returncode == 0, (done.stdout, done.stderr)
    print(done.stdout.strip())

def capture():
    path = ROOT / 'apps/os/cpu-return-host.py'
    spec = importlib.util.spec_from_file_location('event_npu_capture', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    legacy = struct.pack('<8Q', 1, 20, 3, 4, 5, 0, 0, 0)
    assert 'npu_owner_wakes' not in module.decode_published({'npu-register': legacy}, {})
    raw = legacy + struct.pack('<18Q', *([4, 3, 1, 2, 0, 2000000] * 3))
    decoded = module.decode_published({'npu-register': raw}, {})['npu_owner_wakes']
    assert set(decoded) == {'service', 'bootstrap', 'mailbox'}
    assert all(v['idle_wakes_per_second'] == 1 for v in decoded.values())

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    units = ['checks/event-npu-service.c', 'drivers/npu/a733_npu_controller.c',
             'drivers/npu/a733_npu_mmu.c', 'runtime/deadline.c']
    defines = ['SUNSTONE_A733_OS=1', 'SUNSTONE_DEADLINE_SLOTS=16']
    flags = ['-ffunction-sections', '-fdata-sections']
    exe = host_rtos.build(args.output, units, 'event-npu', defines=defines,
                          flags=flags, link=['-Wl,--gc-sections'])
    for case in ('0', '1', '2'):
        done = subprocess.run([str(exe), case], capture_output=True, text=True, env=environment())
        (args.output / ('case-' + case + '.log')).write_text(done.stdout + done.stderr)
        assert done.returncode == 0, (case, done.stdout, done.stderr)
        print(done.stdout.strip())
    # Restore tick pacing in the real runtime worker. The idle assertion must
    # reject it even though notifications still deliver completions correctly.
    source = (ROOT / 'checks/event-npu-service.c').read_text()
    production = (ROOT / 'drivers/npu/npu_service.c').read_text()
    old = '((void)ulTaskNotifyTake(pdTRUE,portMAX_DELAY),SUNSTONE_DEADLINE_NOTIFIED);'
    assert production.count(old) == 1
    mutant = args.output / 'tick-service.c'
    mutant.write_text(production.replace(old, '((void)ulTaskNotifyTake(pdTRUE,1),SUNSTONE_DEADLINE_NOTIFIED);'))
    fixture = args.output / 'tick-fixture.c'
    fixture.write_text(source.replace('#include "../drivers/npu/npu_service.c"', '#include "' + str(mutant.resolve()) + '"'))
    bad = host_rtos.build(args.output / 'mutant', [fixture, *units[1:]], 'event-npu-tick',
                          defines=defines, flags=flags, link=['-Wl,--gc-sections'])
    done = subprocess.run([str(bad), '1'], capture_output=True, text=True, env=environment())
    (args.output / 'tick-mutation.log').write_text(done.stdout + done.stderr)
    assert done.returncode != 0 and 'npu_service_wakes.waits==1' in done.stderr, done.stderr
    holds(args.output)
    capture()
    (args.output / 'result.json').write_text(json.dumps({
        'passed': True, 'physical_access': False, 'cases': 3, 'tick_mutation_refused': True,
        'idle_window_us': 2000137, 'no_event_idle_wakes_per_second': 0,
        'source_baseline_active_wakes_per_second': 1000,
        'source_baseline_ready_wakes_per_second': 1,
        'completion_irq_wakes': 1, 'timeout_us': 1237}, indent=2) + '\n')

if __name__ == '__main__':
    main()
