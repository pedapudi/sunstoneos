#!/usr/bin/env python3
"""Execute the native DMA pool bootstrap with modeled platform and timer boundaries."""
import argparse
import importlib.util
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    spec = importlib.util.spec_from_file_location('extract', ROOT / 'checks/os-quiesce-dma.py')
    extract = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(extract)
    source = (ROOT / 'platform/a733/dma_pool_session.c').read_text()
    body = extract.function(source, 'bootstrap')
    prelude = r'''
#include <assert.h>
#include <stdint.h>
#include <stdatomic.h>
#include <setjmp.h>
#include <stdio.h>
#define SUNSTONE_A733_OS 1
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
enum a733_dma_platform_result { A733_DMA_PLATFORM_PENDING,A733_DMA_PLATFORM_READY,A733_DMA_PLATFORM_RETAINED };
enum dma_service_result { DMA_SERVICE_OK,DMA_SERVICE_BUSY };
enum { A733_DMA_POOL_WAITING,A733_DMA_POOL_READY,A733_DMA_POOL_FAILED };
struct a733_dma_pool_session {
    struct { atomic_uint_fast64_t return_deadline;void *clock;struct { unsigned stage; } platform;
        struct { struct { uint64_t deadline_us; } allowed; } handoff; } startup;
    unsigned service;
    atomic_uint state,retire_requested,retired;
};
static struct a733_dma_pool_session pool,*instance=&pool;
static uint64_t time_us;
static unsigned clock_token;
static unsigned selected,registered,waits,polls,stops,notifications,suspended;
static jmp_buf ending;
static int parent(struct a733_dma_pool_session *s){return s==instance;}
static _Noreturn void fail(struct a733_dma_pool_session *s){assert(s==instance);longjmp(ending,2);}
static int a733_counter_clock_now(void *clock,uint64_t *out){assert(clock==&clock_token);*out=time_us;return 1;}
static enum a733_dma_platform_result a733_dma_startup_poll(void *p){
    assert(p==&pool.startup);++polls;
    if(polls==1){pool.startup.platform.stage=1;return A733_DMA_PLATFORM_PENDING;}
    if(polls==2){assert(!time_us);return A733_DMA_PLATFORM_PENDING;}
    assert(time_us==21);return A733_DMA_PLATFORM_READY;
}
static enum dma_service_result dma_service_activate(unsigned *s){assert(s==&pool.service);return DMA_SERVICE_OK;}
static void dma_service_notify_clients(unsigned *s){assert(s==&pool.service);++notifications;}
static int dma_service_retired(unsigned *s){assert(s==&pool.service);return 1;}
static unsigned ulTaskNotifyTake(unsigned clear,unsigned ticks){
    assert(clear&&ticks==portMAX_DELAY&&time_us==21&&notifications==1);
    atomic_store(&pool.retire_requested,1);return 1;
}
static enum a733_dma_platform_result a733_dma_startup_retire(void *s,uint64_t d){
    assert(s==&pool.startup&&d==50021);return ++stops==1?A733_DMA_PLATFORM_PENDING:A733_DMA_PLATFORM_READY;
}
static void vTaskSuspend(void *p){assert(!p);++suspended;longjmp(ending,1);}
static int sunstone_deadline_register(void){++registered;return 1;}
static uint64_t sunstone_deadline_now_us(void){return time_us;}
static uint64_t a733_dma_platform_sample_at(void *p){assert(p==&pool.startup.platform);return 21;}
static int sunstone_deadline_wait_until(uint64_t until){
    ++waits;
    if(selected){assert(until==10&&waits==1);time_us=until;return 1;}
    assert(until==(waits==1?21u:1021u));time_us=until;return 1;
}
static void vTaskDelay(unsigned ticks){(void)ticks;assert(0);}
'''
    cases = r'''
int main(void){
    for(selected=0;selected<2;++selected){
        pool=(struct a733_dma_pool_session){0};time_us=0;
        pool.startup.clock=&clock_token;
        registered=waits=polls=stops=notifications=suspended=0;
        pool.startup.handoff.allowed.deadline_us=selected?10:100000;
        if(!setjmp(ending))bootstrap(&pool);
        assert(registered==1&&suspended==1);
        if(!selected)assert(polls==3&&stops==2&&waits==2&&notifications==2&&atomic_load(&pool.retired));
        else assert(polls==2&&!stops&&waits==1&&notifications==1&&atomic_load(&pool.state)==A733_DMA_POOL_FAILED);
    }
    puts("PASS native DMA pool: ready steps run together, precise reset settle, idle retirement request and bounded expiry");
}
'''
    for name, unit_body in [('current', body), ('tick-mutation', body.replace(
            'sunstone_deadline_wait_until(sample)', '(vTaskDelay(1),0)'))]:
        unit, binary = args.output / (name + '.c'), args.output / name
        unit.write_text(prelude + unit_body + cases)
        subprocess.run(['clang-21', '-std=c11', '-O1', '-g', '-Wall', '-Wextra',
                        '-Wno-unused-function', '-fsanitize=address,undefined', str(unit), '-o', str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        if name == 'current':
            assert result.returncode == 0, result.stdout + result.stderr
            print(result.stdout.strip())
        else:
            assert result.returncode != 0, 'tick-paced reset settle survived'
            print('PASS native DMA pool tick-wake mutation is rejected')


if __name__ == '__main__':
    main()
