#!/usr/bin/env python3
"""Run the DMA owner's GIC retirement wait with a modeled absolute compare."""
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
    spec = importlib.util.spec_from_file_location('quiesce', ROOT / 'checks/os-quiesce-dma.py')
    extract = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(extract)
    text = (ROOT / 'apps/os/dma.c').read_text()
    body = extract.function(text, 'mask_timely') + '\n' + extract.function(text, 'mask')
    prelude = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef void *TaskHandle_t;
struct spi { int unused; };
struct dma_service_i2s_irq { struct spi spi; unsigned masked; };
static struct { struct dma_service_i2s_irq irq; struct { struct { TaskHandle_t worker; } service; } pool;
    unsigned mask_fault; int mask_result; uint64_t mask_deadline,mask_now; } dma;
static struct { struct { struct { int counter; } shared; struct { uint64_t deadline_us; } session; }; } providers_unused;
static struct { struct { int counter; struct { void *mutex[2]; } locks; } shared; } providers;
static struct { struct { uint64_t deadline_us; } session; } board;
#define A733_PLATFORM_LOCK_COUNT 2
static uint64_t now_us,return_deadline;
static unsigned calls,waits,registered,stalled,notification;
static int register_ok=1,wait_ok=1;
static uint64_t os_cpu_return_deadline_us(void){return return_deadline;}
static int board_live(void *p){(void)p;return 1;}
static int a733_counter_clock_now(void *p,uint64_t *out){(void)p;*out=now_us;return 1;}
static int a733_task_context_owner_el1(TaskHandle_t t){return t==(void *)1;}
static TaskHandle_t xSemaphoreGetMutexHolder(void *p){(void)p;return NULL;}
static int gicv3_spi_mask(struct spi *s){assert(s==&dma.irq.spi);return 1;}
static int gicv3_spi_masked(struct spi *s){assert(s==&dma.irq.spi);++calls;return !stalled&&calls>=3;}
static int sunstone_deadline_register(void){++registered;return register_ok;}
static int sunstone_deadline_wait_until(uint64_t d){
    ++waits;assert(d>now_us&&d<=dma.mask_deadline);
    if(notification){notification=0;now_us+=17;return 0;}
    now_us=d;return wait_ok?1:-1;
}
static void vTaskDelay(unsigned n){(void)n;exit(88);}
static void taskENTER_CRITICAL(void){}
static void taskEXIT_CRITICAL(void){}
'''
    cases = r'''
static void reset(void){
    dma.irq.masked=dma.mask_fault=0;dma.pool.service.worker=(void *)1;
    now_us=127;return_deadline=5000;board.session.deadline_us=10000;
    calls=waits=registered=stalled=notification=0;register_ok=wait_ok=1;
}
int main(void){
    reset();assert(mask(&dma.irq)==1&&dma.irq.masked&&calls==3&&waits==2&&registered==1&&now_us==2127);
    reset();notification=1;assert(mask(&dma.irq)==1&&now_us==1144&&waits==2);
    reset();stalled=1;return_deadline=1570;
    assert(mask(&dma.irq)==-1&&!dma.irq.masked&&dma.mask_fault==3&&now_us==1570&&waits==2);
    reset();register_ok=0;assert(mask(&dma.irq)==-1&&!calls&&dma.mask_fault==7);
    reset();wait_ok=0;assert(mask(&dma.irq)==-1&&!dma.irq.masked&&dma.mask_fault==7);
    puts("PASS DMA retirement: absolute retry, notification, exact expiry and retained failure");
}
'''
    for name, source in [('current', body), ('tick-mutation', body.replace(
            'if(sunstone_deadline_wait_until(retry)<0){dma.mask_fault=7;return -1;}', 'vTaskDelay(1);'))]:
        unit = args.output / (name + '.c')
        binary = args.output / name
        unit.write_text(prelude + source + cases)
        subprocess.run(['clang-21', '-std=c11', '-O1', '-g', '-Wall', '-Wextra',
                        '-Wno-unused-function', '-Wno-unused-variable', '-fsanitize=address,undefined',
                        str(unit), '-o', str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        if name == 'current':
            assert result.returncode == 0, result.stdout + result.stderr
            print(result.stdout.strip())
        else:
            assert result.returncode != 0, 'scheduler tick mutation survived'
            print('PASS DMA scheduler tick mutation is rejected')


if __name__ == '__main__':
    main()
