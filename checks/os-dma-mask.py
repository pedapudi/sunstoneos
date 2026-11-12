#!/usr/bin/env python3
"""Exercise the native DMA retirement mask callback with timed GIC replies."""
import argparse
import json
import re
import subprocess
from pathlib import Path
from harness import CLANG, SANITIZE, environment

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['apps/os/dma.c', 'apps/os/cpu-return.h', 'include/a733_task_context.h',
                'drivers/dma/dma_service.c', 'platform/a733/shared_locks.c']


def function(text, name):
    match = re.search(r'^static int ' + name + r'\([^;]*?\)\s*\{', text, re.M)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[match.start():end]


PRELUDE = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef void *TaskHandle_t;
enum { A733_PLATFORM_LOCK_COUNT=4 };
struct dma_service_i2s_irq { unsigned spi,masked; };
static struct { struct { struct { TaskHandle_t worker; } service; } pool;
    struct dma_service_i2s_irq irq; unsigned mask_fault; int mask_result;
    uint64_t mask_deadline,mask_now; } dma;
static struct { struct { struct { void *mutex[4]; } locks; unsigned counter; } shared; } providers;
static struct { struct { uint64_t deadline_us; } session; } board;
static uint64_t now,deadline;
static unsigned masks,reads,delays,ready_after,invalid_context,held,clock_bad,late_mask,late_read,extend;
static unsigned alive,begin_return,critical,critical_entries,late_handler,handler_calls;
static int mask_result,read_fault;
static int a733_task_context_owner_el1(TaskHandle_t t)
{return !invalid_context&&t==dma.pool.service.worker&&t;}
static TaskHandle_t xSemaphoreGetMutexHolder(void *m)
{return held==(uintptr_t)m?dma.pool.service.worker:NULL;}
static uint64_t os_cpu_return_deadline_us(void){return deadline;}
static int board_live(void *p){assert(!p);return alive;}
static int a733_counter_clock_now(void *c,uint64_t *v)
{assert(c==&providers.shared.counter);*v=now++;return !clock_bad;}
static int gicv3_spi_mask(void *s)
{assert(s==&dma.irq.spi&&now<dma.mask_deadline);++masks;if(late_mask)now=deadline;return mask_result;}
static int gicv3_spi_masked(void *s)
{assert(s==&dma.irq.spi&&now<dma.mask_deadline);++reads;if(late_read)now=deadline;return read_fault?-1:reads>ready_after;}
static int sunstone_deadline_register(void){return 1;}
static int sunstone_deadline_wait_until(uint64_t until)
{
    assert(until>now&&until<=dma.mask_deadline&&!invalid_context&&!held&&!dma.irq.masked&&!critical);
    assert(reads&&reads<=ready_after&&now<dma.mask_deadline);
    ++delays;now=until;if(extend)deadline+=1000000;
    if(begin_return)deadline=2500;
    return 1;
}
static void taskENTER_CRITICAL(void)
{assert(!critical);if(late_handler){++handler_calls;dma.irq.masked=1;}critical=1;++critical_entries;}
static void taskEXIT_CRITICAL(void)
{assert(critical);critical=0;if(late_handler){++handler_calls;dma.irq.masked=1;}}
'''
TEST = r'''
static void reset(void)
{
    memset(&dma,0,sizeof(dma));dma.pool.service.worker=(void *)1;
    for(unsigned i=0;i<4;++i)providers.shared.locks.mutex[i]=(void *)(uintptr_t)(i+1);
    now=0;deadline=10000;masks=reads=delays=ready_after=invalid_context=held=clock_bad=late_mask=late_read=extend=0;
    board.session.deadline_us=60000000;alive=1;begin_return=critical=critical_entries=late_handler=handler_calls=0;
    mask_result=1;read_fault=0;
}
int main(void)
{
    reset();assert(mask(&dma.irq)==1&&masks==1&&reads==1&&!delays&&dma.irq.masked&&critical_entries==1&&!critical);
    reset();ready_after=3;assert(mask(&dma.irq)==1&&masks==1&&reads==4&&delays==3&&dma.irq.masked);
    reset();ready_after=100;deadline=2500;assert(mask(&dma.irq)==-1&&masks==1&&reads==3&&delays==3&&!dma.irq.masked&&dma.mask_fault==3);
    unsigned count=masks+reads;assert(mask(&dma.irq)==-1&&masks+reads==count);
    reset();ready_after=100;deadline=2500;extend=1;
    assert(mask(&dma.irq)==-1&&dma.mask_deadline==2500&&delays==3&&!dma.irq.masked);
    reset();deadline=board.session.deadline_us=0;assert(mask(&dma.irq)==-1&&!masks&&!reads&&!delays&&dma.mask_fault==4);
    reset();deadline=0;ready_after=3;assert(mask(&dma.irq)==1&&delays==3&&dma.mask_deadline==board.session.deadline_us);
    reset();deadline=0;ready_after=100;begin_return=1;
    assert(mask(&dma.irq)==-1&&dma.mask_deadline==2500&&delays==3&&!dma.irq.masked);
    reset();deadline=0;alive=0;assert(mask(&dma.irq)==-1&&!masks&&!reads&&dma.mask_fault==6);
    reset();late_handler=1;assert(mask(&dma.irq)==1&&dma.irq.masked&&handler_calls==2&&critical_entries==1&&!critical);
    reset();now=deadline;assert(mask(&dma.irq)==-1&&!masks&&!reads&&!delays&&dma.mask_fault==3);
    reset();invalid_context=1;assert(mask(&dma.irq)==-1&&!masks&&!reads&&!delays&&dma.mask_fault==5);
    for(unsigned i=1;i<=4;++i){reset();held=i;assert(mask(&dma.irq)==-1&&!masks&&!reads&&!delays&&dma.mask_fault==5);}
    reset();assert(mask(NULL)==-1&&!masks&&!reads&&dma.mask_fault==5);
    reset();clock_bad=1;assert(mask(&dma.irq)==-1&&!masks&&!reads&&dma.mask_fault==3);
    reset();late_mask=1;assert(mask(&dma.irq)==-1&&masks==1&&!reads&&!delays&&dma.mask_fault==3);
    reset();late_read=1;assert(mask(&dma.irq)==-1&&masks==1&&reads==1&&!dma.irq.masked&&dma.mask_fault==3);
    reset();mask_result=0;assert(mask(&dma.irq)==0&&masks==1&&!reads&&dma.mask_fault==1);
    reset();read_fault=1;assert(mask(&dma.irq)==-1&&reads==1&&!delays&&dma.mask_fault==2);
    puts("PASS DMA IRQ retirement: delayed RWP, shared deadline, no access after expiry, task and mutex admission");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    text = (ROOT / 'apps/os/dma.c').read_text()
    callback = function(text, 'mask_timely') + '\n' + function(text, 'mask')
    variants = {'normal': callback, 'no-yield': callback.replace('if(sunstone_deadline_wait_until(retry)<0)', '(void)retry; if(0)'),
                'missing-deadline-admitted': callback.replace('if(!dma.mask_deadline)', 'if(0)'),
                'unsynchronized-publication': callback.replace('taskENTER_CRITICAL();', '').replace('taskEXIT_CRITICAL();', '')}
    results = {}
    for name, source in variants.items():
        c = args.output / (name + '.c')
        c.write_text(PRELUDE + source + TEST)
        binary = args.output / name
        command = [str(CLANG), '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                   '-Wno-unused-function', *SANITIZE, str(c), '-o', str(binary)]
        result = subprocess.run(command, text=True, capture_output=True, env=environment())
        (args.output / (name + '-build.log')).write_text(result.stdout + result.stderr)
        assert result.returncode == 0, result.stderr
        result = subprocess.run([str(binary)], text=True, capture_output=True, env=environment(), timeout=10)
        (args.output / (name + '-run.log')).write_text(result.stdout + result.stderr)
        assert (result.returncode == 0) == (name == 'normal'), (name, result.stdout, result.stderr)
        if name != 'normal':
            assert 'Assertion' in result.stderr
        results[name] = dict(returncode=result.returncode, command=command)
    (args.output / 'result.json').write_text(json.dumps(results, indent=2) + '\n')
    print('PASS DMA retirement mask; three compiled mutants rejected')


if __name__ == '__main__':
    main()
