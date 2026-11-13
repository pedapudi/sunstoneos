#!/usr/bin/env python3
"""Exercise the native DMA diagnostic consumer and its completion publication guard."""
import argparse
import importlib.util
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['apps/dma-events/main.c', 'apps/dma-events/report.h', 'apps/dma-events/manifest.json',
                'apps/os/dma.c', 'drivers/dma/dma_service.c', 'include/dma_service.h', 'include/sunstone_dma.h']


def execute(out, name, source, includes):
    unit, binary = out / (name + '.c'), out / name
    unit.write_text(source)
    command = ['clang-21', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Wno-unused-function',
               '-fsanitize=address,undefined', *['-I' + str(p) for p in includes], str(unit), '-o', str(binary)]
    subprocess.run(command, check=True)
    result = subprocess.run([str(binary), str(out / (name + '.bin'))], capture_output=True, text=True)
    (out / (name + '.log')).write_text(result.stdout + result.stderr)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(Path.home() / '.cache/sunstoneos/venv/bin/python3'), '-B',
                    str(ROOT / 'tools/sunstone-manifest.py'), 'emit', '--manifest',
                    str(ROOT / 'apps/dma-events/manifest.json'), '--output', str(out)], check=True,
                   capture_output=True, text=True)
    consumer = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "dma_events-app.h"
#include "sunstone_dma.h"
#include "report.h"
static jmp_buf ended;
static uint64_t time_us;
static unsigned selected,site,phase,pending,answered,jobs,snapshots;
static unsigned char source[4096],destination[4096];
static struct dma_event_result captured;
static const struct ss_grant grant={.main_core=6,.finish_by_us=5000000};
const struct ss_grant *ss_grant(void){return &grant;}
uint64_t ss_counter_ticks(void){return time_us*24;}
int ss_live(void){return 1;}
void ss_progress(void){}
int ss_clock_ready(void){return 1;}
int ss_throttled(void){return 0;}
int ss_clock_info(char *out,size_t n){const char *s="CLOCK cpu_l_mhz=1794 cpu_b_mhz=2002 cpu_l_mv=1000 cpu_b_mv=1050 ";assert(n>strlen(s));strcpy(out,s);return strlen(s);}
void ss_wait_until(uint64_t until){
    assert(until>time_us&&time_us<grant.finish_by_us);
    time_us+=250; /* A preemption gap cannot move the requested deadline. */
    if(!pending){time_us=until;return;}
    if(selected==2&&pending==6){time_us=until;ss_fail(0x757);}
    if(pending==3){memcpy(destination,source,sizeof(source));if(selected==1)destination[37]^=1;}
    answered=pending;pending=0;
}
static int operation(unsigned op){
    assert(!pending||pending==op);
    if(answered==op){answered=0;return 1;}
    pending=op;return 0;
}
int ss_dma_open(struct ss_dma_buffers *out){int r=operation(1);if(r)*out=(struct ss_dma_buffers){source,destination,4096,0};return r;}
int ss_dma_submit(void){return operation(2);}
int ss_dma_complete(void){int r=operation(3);if(r)++jobs;return r;}
int ss_dma_reap(void){return operation(4);}
int ss_dma_close(void){return operation(5);}
int ss_dma_retire(void){int r=operation(6);if(r)phase=1;return r;}
int ss_dma_owner_stats(struct ss_dma_owner_stats *out){
    ++snapshots;*out=(struct ss_dma_owner_stats){.waits=7,.wakes=3,.waiting=1,.retired=phase,.irqs=jobs};
    if(selected==3&&snapshots==2)++out->wakes;
    if(selected==4&&jobs)out->deadline_expiries=1;
    if(selected==5)out->irqs=0;
    if(selected==6)out->irqs=jobs==1?0:jobs;
    return 1;
}
void ss_activity_begin(void){}
void ss_activity_end(int success){assert(success);}
_Noreturn void ss_fail(uint32_t code){site=code;longjmp(ended,1);}
_Noreturn void ss_fail_value(uint32_t code,uint32_t value){assert(value==37);ss_fail(code);}
_Noreturn void ss_finish(const void *data,size_t bytes){assert(bytes==sizeof(captured));memcpy(&captured,data,bytes);longjmp(ended,2);}
'''
    consumer += '\n#include "' + str(ROOT / 'apps/dma-events/main.c') + '"\n'
    consumer += r'''
int main(int argc,char **argv){
    assert(argc==2);
    for(selected=0;selected<7;++selected){
        time_us=1;site=phase=pending=answered=jobs=snapshots=0;
        int result=setjmp(ended);if(!result)dma_events_main(&grant);
        if(!selected){assert(result==2&&captured.jobs==2&&captured.compared_bytes==8192&&
            captured.idle_end_us-captured.idle_begin_us==1000000&&captured.stopped.retired&&snapshots==7&&
            captured.job_irqs[0]==1&&captured.job_irqs[1]==1);
            FILE *f=fopen(argv[1],"wb");assert(f&&fwrite(&captured,sizeof(captured),1,f)==1&&fclose(f)==0);}
        else assert(result==1&&site==(selected==1?0xd0a:selected==2?0x757:selected==3?0xd07:0xd10));
    }
    puts("PASS native DMA consumer: exact output, no-event idle, corrupt byte, retained timeout, idle wake, job expiry and missing IRQ");
}
'''
    includes = [out, ROOT / 'include', ROOT / 'apps/dma-events']
    run = execute(out, 'consumer', consumer, includes)
    assert run.returncode == 0, run.stdout + run.stderr
    print(run.stdout.strip())
    original = (ROOT / 'apps/dma-events/main.c').read_text()
    for name, old in [('missing-job-irq', 'completed.irqs<=started.irqs||'),
                      ('missing-job-expiry', 'completed.deadline_expiries!=started.deadline_expiries')]:
        assert old in original
        mutant = out / (name + '-consumer.c')
        mutant.write_text(original.replace(old, '0||' if old.endswith('||') else '0', 1))
        source = consumer.replace(str(ROOT / 'apps/dma-events/main.c'), str(mutant))
        changed = execute(out, name, source, includes)
        assert changed.returncode != 0, name + ' mutation survived'
        print('PASS native DMA consumer ' + name + ' mutation is rejected')
    spec = importlib.util.spec_from_file_location('decoder', ROOT / 'apps/dma-events/host.py')
    decoder = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(decoder)
    payload = (out / 'consumer.bin').read_bytes()
    assert decoder.decode(payload)['job_irqs'] == 2
    for offset in (0, 8, decoder.HEAD.size + decoder.SNAPSHOT.size + 8,
                   decoder.HEAD.size + 2 * decoder.SNAPSHOT.size + 56 + 4,
                   decoder.HEAD.size + 3 * decoder.SNAPSHOT.size):
        changed = bytearray(payload)
        changed[offset] ^= 1
        try:
            decoder.decode(changed)
        except ValueError:
            continue
        raise AssertionError('DMA decoder accepted a changed result field at ' + str(offset))
    print('PASS DMA decoder consumes the native result and refuses five changed fields')
    spec = importlib.util.spec_from_file_location('extract', ROOT / 'checks/os-quiesce-dma.py')
    extract = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(extract)
    text = (ROOT / 'apps/os/dma.c').read_text()
    stats_body = extract.function(text, 'ss_dma_owner_stats')
    stats_prelude = r'''
#include <assert.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "sunstone_dma.h"
_Static_assert(sizeof(struct ss_dma_owner_stats)==64&&offsetof(struct ss_dma_owner_stats,waiting)==56&&
    offsetof(struct ss_dma_owner_stats,retired)==60,"public DMA owner ABI");
typedef void *TaskHandle_t;
enum { A733_DMA_POOL_EMPTY,A733_DMA_POOL_WAITING,A733_DMA_POOL_READY,A733_DMA_POOL_FAILED };
struct dma_service_wait_record { atomic_uint_fast64_t waits,wakes,idle_us,idle_notifications,idle_timeouts,deadline_expiries; };
static struct dma_service_wait_record dma_service_wake_record;
static struct { struct { unsigned admitted;atomic_uint state;struct { unsigned initialized;atomic_uint_fast64_t irq_deliveries;
    atomic_uint idle_waiting; } service; } pool; } dma;
static struct { void *task; } ss_application;
static unsigned dma_quiesce,return_ready,worker_retired;
static TaskHandle_t current;
static TaskHandle_t xTaskGetCurrentTaskHandle(void){return current;}
static int os_quiesce_requested(const unsigned *q){assert(q==&dma_quiesce);return dma_quiesce;}
static int os_dma_return_ready(void){return return_ready;}
static int dma_service_retired(const void *service){assert(service==&dma.pool.service);return worker_retired;}
static int caller(void){return current==ss_application.task&&!return_ready&&!dma_quiesce;}
'''
    stats_cases = r'''
static void refused(struct ss_dma_owner_stats *out){
    struct ss_dma_owner_stats before=*out;assert(!ss_dma_owner_stats(out)&&!memcmp(&before,out,sizeof(before)));
}
int main(void){
    ss_application.task=current=(void *)1;dma.pool.admitted=dma.pool.service.initialized=1;
    atomic_store(&dma.pool.state,A733_DMA_POOL_READY);worker_retired=1;
    atomic_store(&dma_service_wake_record.waits,11);atomic_store(&dma_service_wake_record.wakes,7);
    atomic_store(&dma_service_wake_record.idle_us,1000000);atomic_store(&dma_service_wake_record.idle_notifications,3);
    atomic_store(&dma_service_wake_record.deadline_expiries,2);atomic_store(&dma.pool.service.irq_deliveries,5);
    atomic_store(&dma.pool.service.idle_waiting,1);
    struct ss_dma_owner_stats out;
    assert(ss_dma_owner_stats(&out)&&out.waits==11&&out.wakes==7&&out.idle_us==1000000&&
        out.idle_notifications==3&&!out.idle_timeouts&&out.deadline_expiries==2&&out.irqs==5&&out.waiting&&!out.retired);
    return_ready=1;assert(ss_dma_owner_stats(&out)&&out.retired);return_ready=0;
    memset(&out,0xa5,sizeof(out));
    current=(void *)2;refused(&out);current=(void *)1;
    dma_quiesce=1;refused(&out);dma_quiesce=0;
    ss_application.task=current=NULL;refused(&out);ss_application.task=current=(void *)1;
    assert(!ss_dma_owner_stats(NULL));
    dma.pool.admitted=0;refused(&out);dma.pool.admitted=1;
    dma.pool.service.initialized=0;refused(&out);dma.pool.service.initialized=1;
    atomic_store(&dma.pool.state,A733_DMA_POOL_WAITING);refused(&out);
    atomic_store(&dma.pool.state,A733_DMA_POOL_EMPTY);refused(&out);
    atomic_store(&dma.pool.state,99);refused(&out);
    atomic_store(&dma.pool.state,A733_DMA_POOL_FAILED);assert(ss_dma_owner_stats(&out)&&!out.retired&&out.irqs==5);
    puts("PASS public DMA owner statistics: full retirement, caller/quiesce/initialization guards, failed state and unchanged refusal output");
}
'''
    stats_variants = {'owner-stats': stats_body,
        'owner-stats-wrong-caller': stats_body.replace('xTaskGetCurrentTaskHandle()!=(TaskHandle_t)ss_application.task', '0'),
        'owner-stats-quiesce': stats_body.replace('os_quiesce_requested(&dma_quiesce)', '0'),
        'owner-stats-early-retirement': stats_body.replace('.retired=os_dma_return_ready()!=0', '.retired=dma_service_retired(&dma.pool.service)!=0'),
        'owner-stats-post-retirement': stats_body.replace('!ss_application.task||xTaskGetCurrentTaskHandle()!=(TaskHandle_t)ss_application.task', '!caller()')}
    for name, body in stats_variants.items():
        run = execute(out, name, stats_prelude + body + stats_cases, [ROOT / 'include'])
        assert (run.returncode == 0) == (name == 'owner-stats'), run.stdout + run.stderr
        print(run.stdout.strip() if name == 'owner-stats' else 'PASS ' + name + ' mutation is rejected')
    cpp = subprocess.run(['clang-21', '-std=c++17', '-x', 'c++', '-fsyntax-only', '-I' + str(ROOT / 'include'), '-'],
                         input='#include <stddef.h>\n#include "sunstone_dma.h"\nstatic_assert(offsetof(struct ss_dma_owner_stats,retired)==60);\n',
                         text=True, capture_output=True)
    (out / 'owner-stats-cpp.log').write_text(cpp.stdout + cpp.stderr)
    assert cpp.returncode == 0, cpp.stderr
    functions = '\n'.join(extract.function(text, n) for n in ('client_event', 'request', 'ss_dma_complete'))
    prelude = r'''
#include <assert.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
enum { DMA_SERVICE_PROGRESS_EVENT=2,DMA_SERVICE_REPLY_EVENT=1,A733_DMA_POOL_READY=1,A733_DMA_POOL_FAILED=2 };
enum { DMA_SERVICE_BUSY=1,DMA_SERVICE_OK=2,DMA_SERVICE_READY=3,DMA_SERVICE_RUNNING=4,DMA_SERVICE_DRAINING=5,DMA_SERVICE_RESULT=6,DMA_SERVICE_COMPLETE=7 };
enum dma_service_operation { DMA_SERVICE_STATUS,DMA_SERVICE_SUBMIT };
#define A733_SMP_COUNTER_HZ 24000000
struct dma_service_request { unsigned operation,job;uint64_t timeout; };
struct dma_service_reply { unsigned result,phase,outcome,holds_buffers; };
static struct { struct { atomic_uint state;unsigned service; } pool;uint64_t ticket;
 unsigned operation,awaiting_completion;atomic_uint_fast64_t events;uint64_t request_event,completion_event; } dma;
static unsigned sends,notifies,race,finished,caller_ok=1;
static void os_application_wake_main(void){++notifies;}
static int caller(void){return caller_ok;}
static int dma_service_send(unsigned *service,const struct dma_service_request *r,unsigned wait,uint64_t *ticket)
{assert(service==&dma.pool.service&&r->operation==DMA_SERVICE_STATUS&&!wait);*ticket=++sends;return 1;}
static int dma_service_collect(unsigned *service,uint64_t ticket,struct dma_service_reply *out,unsigned wait);
'''
    end = r'''
static int dma_service_collect(unsigned *service,uint64_t ticket,struct dma_service_reply *out,unsigned wait){
 assert(service==&dma.pool.service&&ticket==sends&&!wait);
 client_event(&dma,DMA_SERVICE_REPLY_EVENT);if(race){race=0;client_event(&dma,DMA_SERVICE_PROGRESS_EVENT);}
 *out=(struct dma_service_reply){DMA_SERVICE_OK,finished?DMA_SERVICE_RESULT:DMA_SERVICE_RUNNING,DMA_SERVICE_COMPLETE,1};return 1;
}
int main(void){
 memset(&dma,0,sizeof(dma));atomic_store(&dma.pool.state,A733_DMA_POOL_READY);
 assert(ss_dma_complete()==0&&sends==1&&notifies==1);
 for(unsigned i=0;i<1000;++i)assert(ss_dma_complete()==0&&sends==1);
 caller_ok=0;assert(ss_dma_complete()==-1&&sends==1);caller_ok=1;
 atomic_store(&dma.pool.state,A733_DMA_POOL_FAILED);assert(ss_dma_complete()==-1&&sends==1);
 atomic_store(&dma.pool.state,A733_DMA_POOL_READY);assert(ss_dma_complete()==0&&sends==1);
 finished=1;client_event(&dma,DMA_SERVICE_PROGRESS_EVENT);assert(ss_dma_complete()==1&&sends==2);
 memset(&dma,0,sizeof(dma));atomic_store(&dma.pool.state,A733_DMA_POOL_READY);sends=notifies=finished=0;race=1;
 assert(ss_dma_complete()==0&&sends==1);assert(ss_dma_complete()==0&&sends==2);
 for(unsigned i=0;i<1000;++i)assert(ss_dma_complete()==0&&sends==2);
 puts("PASS DMA adapter: a reply cannot reissue STATUS; raced device publication remains visible");
}
'''
    variants = {'adapter': functions, 'reply-progress-mutation': functions.replace(
        'if(event&DMA_SERVICE_PROGRESS_EVENT)', 'if(event)')}
    for name, body in variants.items():
        run = execute(out, name, prelude + body + end, [])
        assert (run.returncode == 0) == (name == 'adapter'), run.stdout + run.stderr
        print(run.stdout.strip() if name == 'adapter' else 'PASS DMA reply-as-progress mutation is rejected')
    (out / 'result.json').write_text(json.dumps({'passed': True, 'physical_access': False,
        'consumer_cases': 7, 'adapter_cases': 2, 'decoder_mutants': 5, 'mutants_rejected': 3,
        'public_owner_stats_verified': True, 'stats_mutants_rejected': 4}, indent=2) + '\n')


if __name__ == '__main__':
    main()
