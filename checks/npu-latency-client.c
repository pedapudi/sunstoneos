/* Execute the actual diagnostic producer. Scheduler, hardware results and
 * native adapter effects are modeled; this is not an inference check. */
#include "../apps/npu-finite/queue-client.c"
static struct npu_runtime fixture_runtime;
static unsigned char fixture_input[64] __attribute__((aligned(64))),fixture_output[64] __attribute__((aligned(64)));
#if SUNSTONE_NPU_LATENCY_SEQUENCE || SUNSTONE_NPU_DEPENDENT_CHAIN
static unsigned char fixture_command[NPU_RUNTIME_COMMAND_BYTES] __attribute__((aligned(64)));
static unsigned char fixture_other_command[NPU_RUNTIME_COMMAND_BYTES] __attribute__((aligned(64)));
#if SUNSTONE_NPU_DEPENDENT_CHAIN
static unsigned char fixture_dependent_output[64] __attribute__((aligned(64)));
#endif
#endif
static struct npu_queue_job *fixture_job;
static struct npu_runtime_queue *fixture_last_return;
int fixture_actual_cache(enum aarch64_cache_op,uintptr_t,size_t);
static const unsigned char reference[4]={1,2,3,4},input[4]={5,6,7,8};
#if SUNSTONE_NPU_DEPENDENT_CHAIN
static const unsigned char dependent_reference[4]={9,8,7,6};
#else
#define dependent_reference reference
#endif
const struct npu_finite_network npu_finite_networks[NPU_FINITE_NETWORKS]={
    {input,input,reference,4,4,4},{input,reference,dependent_reference,4,4,4}};
struct a733_observe_identity a733_observe_identity;
atomic_uint a733_npu_application_failed;
unsigned fixture_case,fixture_locks,fixture_calls,fixture_caches,fixture_claimed,fixture_releases,fixture_waits;
unsigned fixture_closed,fixture_returned,fixture_status;
static uint64_t fixture_now=1000000;
static uint64_t fixture_client_cpu,fixture_worker_cpu;
char fixture_log[16384];size_t fixture_log_bytes;
static void verify(int condition)
{if(!condition)platform_panic("fixture assertion",99);}
uint64_t a733_smp_counter(void){fixture_client_cpu+=10;return fixture_now+=10;}
uint64_t ulTaskGetRunTimeCounter(const TaskHandle_t task)
{verify(!task||task==fixture_runtime.service.worker);return task?fixture_worker_cpu:fixture_client_cpu;}
unsigned a733_npu_application_ready(void){return 1;}
struct npu_runtime *a733_npu_application_runtime(void){return &fixture_runtime;}
void platform_puts(const char *p)
{
    while(*p){verify(fixture_log_bytes+1<sizeof fixture_log);fixture_log[fixture_log_bytes++]=*p++;}
    fixture_log[fixture_log_bytes]=0;
}
void platform_hex(uint64_t value)
{
    char text[19]="0x";static const char hex[]="0123456789abcdef";
    for(unsigned i=0;i<16;++i)text[2+i]=hex[(value>>(60-4*i))&15];text[18]=0;platform_puts(text);
}
__attribute__((noinline)) _Noreturn void platform_exit(unsigned status){fixture_status=status;for(;;){}}
__attribute__((noinline)) _Noreturn void platform_panic(const char *message,unsigned code){(void)message;fixture_status=code;for(;;){}}
__attribute__((noinline)) void vTaskSuspend(TaskHandle_t task){(void)task;for(;;){}}
__attribute__((noinline)) _Noreturn void npu_finite_smc(unsigned status){fixture_status=status;for(;;){}}
unsigned aarch64_device_cache_stride(void){return 64;}
void vTaskDelay(TickType_t ticks){verify(!fixture_locks);fixture_now+=ticks*24000;}
void vPortEnterCritical(void){}
void vTaskEnterCritical(void){}
void a733_npu_application_fail(unsigned reason){atomic_store(&a733_npu_application_failed,reason);}
int npu_job_run(unsigned model,const void *in,size_t ni,void *out,size_t no,uint64_t timeout)
{
    verify(!model&&in==input&&ni==4&&no==4&&timeout==1000000&&!fixture_claimed);
    ++fixture_calls;fixture_runtime.doorbell_ticks=fixture_now+1000+current_job%100;fixture_now+=20000;
    fixture_client_cpu+=100;fixture_worker_cpu+=200;
    if((fixture_case==2&&fixture_calls==1)||(fixture_case==3&&current_job==3))return -1;
    memcpy(out,reference,4);if(fixture_case==1&&current_job==4)((unsigned char *)out)[0]=9;
    ++fixture_runtime.counters.jobs;atomic_fetch_add(&fixture_runtime.service.irq_completions,1);return 1;
}
int npu_runtime_tensor(const struct npu_runtime *rt,unsigned slot,enum nbg_section_id section,unsigned index,struct npu_runtime_tensor *tensor)
{
    verify(rt==&fixture_runtime&&slot<2&&!fixture_claimed);
#if SUNSTONE_NPU_DEPENDENT_CHAIN
    if(slot){if(index)return 0;*tensor=(struct npu_runtime_tensor){.data=section==NBG_INPUT?(fixture_case==12?fixture_input:fixture_output):fixture_dependent_output,.bytes=4};return 1;}
#endif
    if(index)return 0;
    *tensor=(struct npu_runtime_tensor){.data=section==NBG_INPUT?fixture_input+(fixture_case==9):fixture_output,.bytes=4};return 1;
}
int npu_runtime_queue_claim(struct npu_runtime_queue *q,struct npu_runtime *rt)
{verify(q==active_queue&&rt==&fixture_runtime&&fixture_calls==1&&!fixture_claimed);q->claimed=1;q->runtime=rt;fixture_claimed=1;return 1;}
uint64_t npu_runtime_queue_lock(void){verify(!fixture_locks);fixture_locks=1;return 123;}
void npu_runtime_queue_unlock(uint64_t saved)
{
    verify(saved==123&&fixture_locks);fixture_locks=0;
    if(fixture_case==10&&active_queue->queue.submitted>atomic_load(&active_queue->queue.completed)&&fixture_job->held) {
        /* An immediate IRQ lands after DAIF restore and before the caller's
         * submit-end timestamp. Its elapsed ticks overlap the submit phase. */
        fixture_now+=5000;active_queue->irq_cpu_ticks+=5000;memcpy(fixture_output,reference,4);
        atomic_store(&active_queue->queue.completed,active_queue->queue.submitted);
    }
}
int npu_queue_register(struct npu_queue *q,struct npu_queue_command *c,const struct a733_npu_command *hardware)
{verify(!fixture_locks&&q==&active_queue->queue&&(hardware==&fixture_runtime.commands[1]||hardware==&fixture_runtime.commands[2]));c->owner=q;return 1;}
int aarch64_device_cache(enum aarch64_cache_op operation,uintptr_t at,size_t bytes)
{
    verify(!fixture_locks);
#if SUNSTONE_NPU_LATENCY_SEQUENCE || SUNSTONE_NPU_DEPENDENT_CHAIN
    if(at==(uintptr_t)fixture_command||at==(uintptr_t)fixture_other_command){verify(operation==AARCH64_CACHE_CLEAN&&bytes==sizeof fixture_command);return fixture_actual_cache(operation,at,bytes);}
#endif
#if SUNSTONE_NPU_DEPENDENT_CHAIN
    if(at==(uintptr_t)fixture_dependent_output){verify(bytes==64&&operation!=AARCH64_CACHE_CLEAN);++fixture_caches;return fixture_actual_cache(operation,at,bytes);}
#endif
    verify(bytes==64);++fixture_caches;
    if(operation==AARCH64_CACHE_CLEAN)verify(at==(uintptr_t)fixture_input);
    else verify(at==(uintptr_t)fixture_output);
    if(fixture_case==5&&current_job==3&&operation==AARCH64_CACHE_INVALIDATE)return 0;
    return fixture_actual_cache(operation,at,bytes);
}
int npu_queue_submit(struct npu_queue *q,struct npu_queue_job *job,struct npu_queue_command *c,
    struct npu_queue_fence after,uint64_t timeout,struct npu_queue_fence *out)
{
    verify(fixture_locks&&q==&active_queue->queue&&c->owner==q&&((!after.queue&&!after.value)||(after.queue==q&&after.value==q->submitted))&&timeout==1000000&&!job->held);
    verify(!memcmp(fixture_input,input,4)&&fixture_output[0]==0xa5&&fixture_output[1]==0x3c);
    if(fixture_case==3&&current_job==3)return -1;
    fixture_now+=5+current_job%100;
    fixture_runtime.doorbell_ticks=fixture_case==11?fixture_now-100:fixture_now;
    ++q->submitted;job->held=1;fixture_job=job;*out=(struct npu_queue_fence){q,q->submitted};return 1;
}
int npu_runtime_queue_wait(struct npu_runtime_queue *q,struct npu_queue_fence fence)
{
    verify(!fixture_locks&&q==active_queue&&fence.value==q->queue.submitted&&fixture_job->held);++fixture_waits;
    if(atomic_load(&q->queue.completed)==q->queue.submitted)return 1;
    fixture_now+=5000;
    if((fixture_case==4&&current_job==3)||(fixture_case==6&&current_job==1000))return -1;
    q->irq_cpu_ticks+=7;
    memcpy(fixture_output,reference,4);
#if SUNSTONE_NPU_DEPENDENT_CHAIN
    if(current_job>=1000)memcpy(fixture_dependent_output,dependent_reference,4);
#endif
    if((fixture_case==1&&current_job==4)||(fixture_case==7&&current_job>=1000))fixture_output[0]=9;
    atomic_store(&q->queue.completed,q->queue.submitted);return 1;
}
int npu_queue_release(struct npu_queue *q,struct npu_queue_job *job)
{verify(fixture_locks&&q==&active_queue->queue&&job->held&&(current_job>=1000||fixture_caches%3==0));job->held=0;++fixture_releases;return 1;}
int npu_runtime_queue_return(struct npu_runtime_queue *q)
{
    verify(!fixture_locks&&q==active_queue&&!fixture_job->held);
    if(q==&direct)verify(fixture_releases==1000);
#if SUNSTONE_NPU_DEPENDENT_CHAIN
    else verify(fixture_releases==(q==&dependent_queue[0]?1002:1003));
#else
    else verify(fixture_releases==1001);
#endif
    ++fixture_returned;if(fixture_last_return!=q){fixture_last_return=q;return 0;}
    fixture_runtime.counters.jobs+=q->queue.submitted;
    atomic_fetch_add(&fixture_runtime.service.irq_completions,(unsigned)q->queue.submitted);
    q->claimed=0;fixture_claimed=0;return 1;
}
int npu_job_close(void)
{verify(!fixture_claimed&&fixture_runtime.counters.jobs==NPU_STARTUP_JOBS);fixture_closed=1;fixture_runtime.slot[0].closed=1;atomic_store(&fixture_runtime.slot[0].state,NPU_RUNTIME_CLOSED);
#if SUNSTONE_NPU_DEPENDENT_CHAIN
 atomic_store(&fixture_runtime.slot[1].state,NPU_RUNTIME_CLOSED);
#endif
 fixture_runtime.service.controller.state=A733_NPU_CLOSED;return 1;}
unsigned a733_npu_application_stopped(void){return fixture_closed;}
unsigned a733_npu_application_quiesce(void){return fixture_closed;}
unsigned a733_npu_application_quiescent(void){return fixture_closed;}
int npu_service_irq_stopped(const struct npu_service *s){verify(s==&fixture_runtime.service);return fixture_closed;}
unsigned npu_service_irq_completions(const struct npu_service *s){return atomic_load(&s->irq_completions);}
int npu_runtime_reference_step(struct npu_runtime *rt,struct npu_runtime_slot *s)
{verify(rt==&fixture_runtime&&s==&rt->slot[0]&&!fixture_claimed&&!fixture_locks&&s->span==0&&s->offset==0&&s->crc==UINT32_MAX);return 1;}
void fixture_entry(void)
{
    a733_observe_identity=(struct a733_observe_identity){7,8,9,0,480000000};
    verify(!fixture_actual_cache(AARCH64_CACHE_CLEAN,(uintptr_t)fixture_input,4));
    verify(!fixture_actual_cache(AARCH64_CACHE_CLEAN,(uintptr_t)fixture_input+1,64));
    fixture_runtime.slot[0].opened=1;atomic_store(&fixture_runtime.service.irq_completions,1);
    fixture_runtime.service.worker=(TaskHandle_t)&fixture_runtime.service.worker;
    fixture_runtime.slot[0].layout.inputs=1;fixture_runtime.slot[0].layout.outputs=1;
    fixture_runtime.slot[0].targets[NBG_ALLOC_INPUT]=(struct nbg_assembly_buffer){fixture_input+(fixture_case==9),fixture_case==8?4:64,0};
    fixture_runtime.slot[0].targets[NBG_ALLOC_INPUT+1]=(struct nbg_assembly_buffer){fixture_output,64,0};
#if SUNSTONE_NPU_LATENCY_SEQUENCE || SUNSTONE_NPU_DEPENDENT_CHAIN
    const uint32_t tail[]={0x08010e02,0x30000701,0x48000000,0x30000701,0x08010e03,0xc23,0x08010e01,0x40,0x10000000,0,0,0,0,0};
    memcpy(fixture_command,tail,sizeof tail);
    fixture_runtime.slot[0].command.cpu=(uintptr_t)fixture_command;
    fixture_runtime.slot[0].command.bytes=sizeof fixture_command;
    fixture_runtime.slot[0].stream_bytes=sizeof tail;
#if SUNSTONE_NPU_DEPENDENT_CHAIN
    fixture_runtime.slots=2;fixture_runtime.resident=1;
    fixture_runtime.slot[0].targets[NBG_ALLOC_INPUT+1].device=0x30000000;
    fixture_runtime.slot[1].targets[NBG_ALLOC_INPUT]=(struct nbg_assembly_buffer){fixture_output,64,0x30000000};
    fixture_runtime.slot[1].layout.inputs=1;fixture_runtime.slot[1].layout.outputs=1;
    fixture_runtime.slot[1].targets[NBG_ALLOC_INPUT+1]=(struct nbg_assembly_buffer){fixture_dependent_output,64,0};
    memcpy(fixture_other_command,tail,sizeof tail);
    fixture_runtime.slot[1].command.cpu=(uintptr_t)fixture_other_command;
    fixture_runtime.slot[1].command.bytes=sizeof fixture_other_command;
    fixture_runtime.slot[1].stream_bytes=sizeof tail;
#endif
#endif
    npu_latency_client(NULL);
}
/* Compile the actual receipt writer under a separate fixture entry. */
#define platform_exit npu_latency_receipt_exit
#include "../apps/npu-finite/return.c"
