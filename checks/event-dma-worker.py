#!/usr/bin/env python3
"""Execute the native DMA worker's waits with queue and counter boundaries modeled."""
import argparse
import hashlib
import importlib.util
import json
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
    text = (ROOT / 'drivers/dma/dma_service.c').read_text()
    worker = extract.function(text, 'worker')
    # The two architectural register reads establish the owner's EL1 context.
    # The host supplies that accepted context; the worker body stays unchanged.
    worker = worker.replace('__asm__ volatile("mrs %0,CurrentEL\\n mrs %1,spsel" : "=r"(level),"=r"(stack));',
                            'level=4;stack=0;')
    assert '__asm__' not in worker
    functions = '\n'.join(extract.function(text, name) for name in ('progressing', 'irq_wait', 'native_wait', 'dma_service_send'))
    prelude = r'''
#include <assert.h>
#include <stdint.h>
#include <stdatomic.h>
#include <setjmp.h>
#include <stdio.h>
#define SUNSTONE_A733_OS 1
#define configNUMBER_OF_CORES 2
#define A733_DMA_CHANNELS 2
#define DMA_SERVICE_CLIENTS 2
#define DMA_SERVICE_NO_JOB UINT32_MAX
#define DMA_SERVICE_NO_RESULT 0
#define SUNSTONE_DEADLINE_EXPIRED 1
#define pdTRUE 1
#define pdFALSE 0
#define DMA_SERVICE_QUIESCE 1
#define portMAX_DELAY UINT32_MAX
#define configASSERT(x) assert(x)
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
#define SS_LATENCY_EVENT_CLAIM(a,b) ((void)0)
#define SS_LATENCY_EVENT_FINISH(a,b) ((void)0)
typedef unsigned TickType_t;
typedef unsigned BaseType_t;
enum { SERVICE_ACTIVE,SERVICE_WAITING,SERVICE_CHECKING,SERVICE_RETAINED,SERVICE_RETIRING,SERVICE_RETIRED };
enum dma_service_phase { DMA_SERVICE_CLOSED,DMA_SERVICE_OPENING,DMA_SERVICE_RUNNING,DMA_SERVICE_DRAINING,DMA_SERVICE_CLOSING };
enum dma_service_operation { DMA_SERVICE_STATUS,DMA_SERVICE_PERIOD_GET,DMA_SERVICE_REAP };
enum { DMA_SERVICE_I2S_STREAM,DMA_SERVICE_OK };
enum { A733_DMA_ACTIVE,A733_DMA_STOPPING };
enum { IRQ_EVENT=1u };
struct a733_dma_channel { unsigned status;uint64_t started,timeout,stop_started; };
struct a733_i2s_stream { unsigned profile_unused;struct { unsigned cyclic; } profile;uint64_t deadline,frames; };
struct dma_service_channel { enum dma_service_phase phase;unsigned owner,job;uint64_t job_ticket;unsigned outcome,holds_buffers; };
struct dma_service_request { enum dma_service_operation operation;unsigned job; };
struct dma_service_message { struct dma_service_request request;uint64_t ticket;unsigned client; };
struct dma_service_client { unsigned replies,jobs;uint64_t pending,next_ticket; };
struct dma_service_reply { uint64_t ticket,job_ticket,stream_frames;unsigned result,period,phase,outcome,owns_session,holds_buffers; };
struct dma_service {
    atomic_uint activation_state,quiesce_requested,retirement_collected,irq_pending,idle_waiting;
    struct { struct a733_dma_channel channels[2];uint64_t stop_timeout;unsigned events; } controller;
    struct dma_service_channel channels[2];
    struct { unsigned channel,kind; } jobs[2];unsigned job_count;
    struct dma_service_client clients[2];unsigned requests;
    struct { int (*rearm)(void *);void *cookie;unsigned fallback; } irq;
    unsigned irq_bound,irq_retained,irq_spurious,irq_armed,irq_events;
    unsigned initialized,retirement_mask,retirement_senders,worker;
};
static struct dma_service service;
static struct dma_service *instance=&service;
static unsigned selected,passes,waits,registered,publisher_wakes,received,replies;
static uint64_t now_us;
static struct { uint64_t waits,wakes,idle_us,idle_notifications,idle_timeouts,deadline_expiries; } dma_service_wake_record;
static jmp_buf parked;
static void dma_service_notify_clients(struct dma_service *s){assert(s==&service);}
static void quiesce_idle(struct dma_service *s){(void)s;assert(0);}
static int event_driven(struct dma_service *s,unsigned i){
 return i==0&&s->channels[i].phase==DMA_SERVICE_RUNNING&&s->controller.channels[i].status==A733_DMA_ACTIVE&&
    now_us<1000041&&!(selected==3&&!passes);
}
static unsigned poll_channel(struct dma_service *s,unsigned i){
    assert(s==&service);if(i)return 0;++passes;
    if(selected==0||selected==6)return 0;
    if(selected==3&&passes==1)return 3;
    if(selected==4&&passes==1){assert(now_us==1000041);s->controller.channels[0].status=A733_DMA_STOPPING;}
    return 1;
}
static void irq_observe(struct dma_service *s,unsigned pending){(void)s;assert(!pending);}
static int xQueueReceive(unsigned q,void *out,unsigned ticks){
 assert(!ticks);if(selected==9&&q==1&&!received){
  *(struct dma_service_message *)out=(struct dma_service_message){{DMA_SERVICE_STATUS,0},1,0};received=1;return 1;
 }return 0;
}
static int xQueueSend(unsigned q,const void *in,unsigned ticks){
 (void)in;assert(!ticks);if(selected==9){assert(q==2);++replies;return 1;}
 assert(selected>=7&&service.retirement_senders==1);return 0;
}
static struct dma_service_client *caller(struct dma_service *s,unsigned *slot){assert(s==&service);*slot=0;return &s->clients[0];}
static void xTaskNotifyGive(unsigned task){assert(task==service.worker);++publisher_wakes;}
static int execute(struct dma_service *s,const struct dma_service_message *m,unsigned *period)
{(void)period;assert(s==&service&&selected==9&&m->request.operation==DMA_SERVICE_STATUS);return DMA_SERVICE_OK;}
static struct a733_i2s_stream *dma_service_i2s_stream(struct dma_service *s,unsigned i){(void)s;(void)i;return NULL;}
static void notify_client(struct dma_service *s,unsigned client,unsigned event){assert(s==&service&&selected==9&&!client&&event==1);}
#define DMA_SERVICE_REPLY_EVENT 1
static unsigned ulTaskNotifyTake(unsigned clear,unsigned ticks){
    assert(clear&&ticks==portMAX_DELAY&&(selected==0||selected==6)&&passes==1&&registered==1);++waits;longjmp(parked,1);
}
static void vTaskSuspend(void *p){(void)p;assert(0);}
static int sunstone_deadline_register(void){++registered;return 1;}
static uint64_t sunstone_deadline_now_us(void){return now_us;}
static int sunstone_deadline_wait_until(uint64_t until){
    ++waits;
    if(selected==1||selected==3){assert(until==1000041&&passes==(selected==3?1u:0u));longjmp(parked,1);}
    if(selected==2||selected==5){assert(until==1041&&passes==1);longjmp(parked,1);}
    if(selected==9){assert(until==1000041&&!passes&&replies==1&&received==1);longjmp(parked,1);}
    assert(selected==4);
    if(waits==1){assert(until==1000041&&!passes);now_us=until;service.irq_armed=0;return 1;}
    assert(passes==1&&until==1001041);longjmp(parked,1);
}
'''
    cases = r'''
int main(void){
    for(selected=0;selected<7;++selected){
        service=(struct dma_service){0};registered=passes=waits=0;now_us=41;
        service.irq_bound=service.irq_armed=1;service.irq.fallback=1;service.controller.stop_timeout=240000;
        if(selected>=5)service.irq_bound=service.irq_armed=0;
        if(selected)service.channels[0].phase=DMA_SERVICE_RUNNING;
        service.controller.channels[0]=(struct a733_dma_channel){A733_DMA_ACTIVE,41*24,1000000*24,1000041*24};
        if(selected==2)service.controller.channels[0].status=A733_DMA_STOPPING;
        if(!setjmp(parked))worker(&service);
        assert(registered==1&&waits==(selected==4?2u:1u));
    }
    for(selected=7;selected<9;++selected){
        service=(struct dma_service){0};publisher_wakes=0;
        service.initialized=service.retirement_mask=service.worker=1;service.irq_bound=selected==8;
        atomic_store(&service.quiesce_requested,1);
        struct dma_service_request request={0};uint64_t ticket=0;
        assert(!dma_service_send(&service,&request,0,&ticket)&&!service.retirement_senders&&
            !service.clients[0].pending&&publisher_wakes==1);
    }
    selected=9;service=(struct dma_service){0};now_us=41;registered=passes=waits=received=replies=0;
    service.irq_bound=service.irq_armed=1;service.irq.fallback=1;service.requests=1;
    service.clients[0].replies=2;service.clients[0].jobs=1;service.job_count=1;service.jobs[0].kind=1;
    service.channels[0].phase=DMA_SERVICE_RUNNING;
    service.controller.channels[0]=(struct a733_dma_channel){A733_DMA_ACTIVE,41*24,1000000*24,0};
    if(!setjmp(parked))worker(&service);
    assert(!passes&&replies==1&&waits==1);
    puts("PASS native DMA worker: indefinite idle notification wait, exact transfer deadline, timed drain and ready batching");
}
'''
    variants = {'current': (functions, worker), 'tick-wake': (functions, worker.replace(
        'native_wait(s,active,event_driven_only);', '(void)ulTaskNotifyTake(pdTRUE,1);')),
        'lost-unbound-publisher': (functions.replace(
            'if(guarded&&atomic_load_explicit(&s->quiesce_requested,memory_order_acquire))',
            'if(guarded&&s->irq_bound&&atomic_load_explicit(&s->quiesce_requested,memory_order_acquire))'), worker),
        'completion-before-irq': (functions, worker.replace(
            's->irq_bound&&!(pending&IRQ_EVENT)&&event_driven(s,i)?1u:poll_channel(s,i)', 'poll_channel(s,i)')),
        'status-polls-before-irq': (functions, worker.replace(
            '&&(!s->irq_bound||!event_driven(s,s->jobs[message.request.job].channel))', '&&1'))}
    records = {}
    for name, (helpers, body) in variants.items():
        unit, binary = args.output / (name + '.c'), args.output / name
        unit.write_text(prelude + helpers + body + cases)
        command = ['clang-21', '-std=c11', '-O1', '-g', '-Wall', '-Wextra',
                   '-Wno-unused-function', '-fsanitize=address,undefined', str(unit), '-o', str(binary)]
        build = subprocess.run(command, capture_output=True, text=True)
        (args.output / (name + '-build.log')).write_text(build.stdout + build.stderr)
        assert build.returncode == 0, build.stdout + build.stderr
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        (args.output / (name + '.log')).write_text(result.stdout + result.stderr)
        records[name] = {'returncode': result.returncode, 'command': command,
                         'accepted': name == 'current', 'source_sha256': hashlib.sha256(unit.read_bytes()).hexdigest()}
        if name == 'current':
            assert result.returncode == 0, result.stdout + result.stderr
            print(result.stdout.strip())
        else:
            assert result.returncode != 0, name + ' mutation survived'
            print('PASS native DMA worker ' + name + ' mutation is rejected')
    (args.output / 'result.json').write_text(json.dumps({'passed': True, 'physical_access': False,
        'production_source_sha256': hashlib.sha256(text.encode()).hexdigest(), 'variants': records}, indent=2) + '\n')


if __name__ == '__main__':
    main()
