#!/usr/bin/env python3
"""Execute the production lock admission with controlled RTOS waits and time."""
import argparse
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['platform/a733/shared_locks.c', 'include/a733_platform_locks.h',
                'drivers/pinctrl/a733_pins_native.c', 'platform/a733/native_usb.c', 'apps/os/cpu-return.c']

SHIM = r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
typedef unsigned TickType_t;
typedef unsigned TaskHandle_t;
enum a733_platform_lock { A733_PLATFORM_REGISTER_LOCK, A733_PLATFORM_I2C_LOCK,
 A733_PLATFORM_I2C1_LOCK, A733_PLATFORM_I2C11_LOCK, A733_PLATFORM_LOCK_COUNT };
struct a733_platform_locks {
 unsigned mutex[4]; atomic_uint retained;
 int (*now)(void *, uint64_t *); void *context;
 uint64_t (*deadline)(void *); unsigned initialized;
};
#define configTICK_RATE_HZ 1000
#define pdTRUE 1
#define configASSERT assert
static struct a733_platform_locks locks;
static unsigned holder[4], takes, gives, failures, context_ok, clock_ok;
static unsigned change_limit, late, retain_on_take, backward;
static uint64_t clock_us, limit_us;
static int a733_task_context_any_no_el_check(void) { return context_ok; }
static TaskHandle_t xTaskGetCurrentTaskHandle(void) { return 1; }
static unsigned xSemaphoreGetMutexHolder(unsigned mutex) { return holder[mutex]; }
static int xSemaphoreTake(unsigned mutex, unsigned wait)
{
 assert(wait <= 1); ++takes;
 if(change_limit)limit_us=2500;
 if(retain_on_take)atomic_store(&locks.retained,1);
 if(backward)clock_us=0;
 else clock_us += late ? 4000 : wait ? 1000 : 0;
 if(failures){--failures; return 0;}
 holder[mutex]=1; return 1;
}
static int xSemaphoreGive(unsigned mutex) { assert(holder[mutex]==1); holder[mutex]=0; ++gives; return 1; }
static int clock_now(void *unused, uint64_t *now) { (void)unused; *now=clock_us; return clock_ok; }
static uint64_t dynamic_limit(void *unused) { (void)unused; return limit_us; }
static uint64_t os_cpu_return_deadline_us(void) { return limit_us; }
struct a733_native_usb { int counter; struct a733_platform_locks locks; int (*live)(void *); void *context; };
static int always_live(void *unused) { (void)unused; return 1; }
struct a733_pins_locks { int (*now)(void *,uint64_t *); void *cookie; };
struct a733_pins_native { struct a733_pins_locks locks; };
static unsigned io_accesses;
static int a733_counter_clock_now(void *unused,uint64_t *now) { return clock_now(unused,now); }
static int a733_pins_native_address(uintptr_t address,unsigned writing) { (void)writing; return address==0x300b000; }
static uint32_t a733_mmio_read32_dsb_sy_both(uintptr_t address) { assert(address==0x300b000); ++io_accesses; return 7; }
static void a733_mmio_write32_dsb_sy_both(uintptr_t address,uint32_t value) { assert(address==0x300b000 && value==7); ++io_accesses; }
static void reset(void)
{
 locks=(struct a733_platform_locks){.mutex={0,1,2,3},.now=clock_now,.deadline=dynamic_limit,.initialized=1};
 for(unsigned i=0;i<4;++i)holder[i]=0;
 takes=gives=failures=change_limit=late=retain_on_take=backward=0;
 clock_us=100; limit_us=0; context_ok=clock_ok=1;
}
'''

CASES = r'''
int main(void)
{
 reset(); failures=5;
 assert(a733_platform_lock_step(&locks,0,60000000)==0 && takes==1 && !holder[0]);
 reset(); failures=2;
 assert(a733_platform_lock_enter(&locks,0,60000000)==1 && takes==3 && holder[0]==1);
 reset(); failures=100; change_limit=1;
 assert(a733_platform_lock_enter(&locks,0,60000000)==0 && takes==3 && clock_us==2100 && !holder[0]);
 reset(); change_limit=late=1;
 assert(a733_platform_lock_step(&locks,0,60000000)==-1 && gives==1 && !holder[0]);
 reset(); limit_us=clock_us;
 assert(a733_platform_lock_step(&locks,0,60000000)==-1 && !takes);
 reset(); assert(a733_platform_lock_step(&locks,0,60000000)==1 && holder[0]==1);
 reset(); failures=1;
 assert(a733_platform_lock_step(&locks,0,500)==0 && takes==1 && clock_us==100);
 reset(); context_ok=0; assert(a733_platform_lock_step(&locks,0,60000000)==-1 && !takes);
 reset(); clock_ok=0; assert(a733_platform_lock_step(&locks,0,60000000)==-1 && !takes);
 reset(); holder[2]=1; assert(a733_platform_lock_step(&locks,0,60000000)==-1 && !takes);
 reset(); atomic_store(&locks.retained,1); assert(a733_platform_lock_step(&locks,0,60000000)==-1 && !takes);
 reset(); retain_on_take=1; assert(a733_platform_lock_step(&locks,0,60000000)==-1 && gives==1 && !holder[0]);
 reset(); backward=1; assert(a733_platform_lock_step(&locks,0,60000000)==-1 && gives==1 && !holder[0]);
 reset(); assert(a733_platform_lock_live(&locks)==1);
 limit_us=100; assert(a733_platform_lock_live(&locks)==0);
 limit_us=101; assert(a733_platform_lock_live(&locks)==1);
 clock_ok=0; assert(a733_platform_lock_live(&locks)==0);
 struct a733_native_usb shared={0};
 struct a733_pins_native pins={.locks={return_pin_now,&shared}};
 uint32_t value=0;
 reset(); limit_us=clock_us;
 assert(!native_read(&pins,0x300b000,&value) && !native_write(&pins,0x300b000,7) && !io_accesses);
 limit_us=clock_us+1;
 assert(native_read(&pins,0x300b000,&value) && native_write(&pins,0x300b000,7) && io_accesses==2 && value==7);
 clock_us=limit_us;
 assert(!native_read(&pins,0x300b000,&value) && !native_write(&pins,0x300b000,7) && io_accesses==2);
 reset(); shared=(struct a733_native_usb){.locks=locks,.live=always_live}; failures=2;
 assert(enter(&shared,60000000)==1 && takes==3 && holder[0]==1);
 puts("PASS 21 shared-lock and native pin deadline cases");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    source = (ROOT / 'platform/a733/shared_locks.c').read_text()
    source = source[source.index('static uint64_t lock_deadline('):source.index('void a733_platform_lock_leave(')]
    os_source = (ROOT / 'apps/os/cpu-return.c').read_text()
    source += os_source[os_source.index('static int return_pin_now('):os_source.index('int os_cpu_return_requested(')]
    pins = (ROOT / 'drivers/pinctrl/a733_pins_native.c').read_text()
    source += pins[pins.index('static int native_read('):pins.index('int a733_pins_native_init(')]
    native = (ROOT / 'platform/a733/native_usb.c').read_text()
    source += native[native.index('static int enter('):native.index('static void leave(')]

    def run(text, name):
        unit, exe = args.output / (name + '.c'), args.output / name
        unit.write_text(SHIM + text + CASES)
        subprocess.run(['clang-21', '-std=c11', '-O1', '-g', '-Wall', '-Wextra',
                        '-fsanitize=address,undefined', str(unit), '-o', str(exe)], check=True, capture_output=True)
        return subprocess.run([str(exe)], capture_output=True, text=True)

    real = run(source, 'real')
    assert real.returncode == 0, real.stderr
    mutants = [source.replace('lock_enter(p,lock,deadline,1)', 'lock_enter(p,lock,deadline,0)'),
               source.replace('deadline=lock_deadline(p,deadline);', ''),
               source.replace('configASSERT(xSemaphoreGive(p->mutex[lock]) == pdTRUE);', ''),
               source.replace('!n->locks.now(n->locks.cookie,&now) || ', ''),
               source.replace('&&a733_platform_lock_enter(&((struct a733_native_usb *)p)->locks,A733_PLATFORM_REGISTER_LOCK,deadline);',
                              '&&a733_platform_lock_step(&((struct a733_native_usb *)p)->locks,A733_PLATFORM_REGISTER_LOCK,deadline)==1;')]
    for i, mutant in enumerate(mutants):
        assert mutant != source and run(mutant, 'mutant-' + str(i)).returncode != 0
    (args.output / 'result.json').write_text(json.dumps({'passed': True, 'physical_access': False,
                                                       'cases': 21, 'mutants_rejected': len(mutants)}, indent=2) + '\n')
    print(real.stdout.strip() + '; five mutants rejected')


if __name__ == '__main__':
    main()
