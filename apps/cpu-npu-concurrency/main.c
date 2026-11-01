/* Finite, checked float32 arithmetic on every CPU with an optional NPU peer.
 * Service and IRQ ownership remain with the common OS device path. */
#include "sunstone_app.h"
#include "FreeRTOS.h"
#include "task.h"
#include "receipt.h"
#include "gemm.h"
#include <stdatomic.h>
#include <string.h>
#ifdef SUNSTONE_A733_OS_NPU
#include "vectors.h"
#endif
static struct cpu_npu_matrix matrix[8];
static struct cpu_npu_receipt receipt;
static StaticTask_t tasks[8];
static StackType_t stacks[8][2048];
static atomic_uint start_epoch,stop_epoch,ack[8];
/* Set once the clock owner has finished its grant, after the boot switch saw
 * NORMAL with every sensor read. The OS holds ss_throttled() at 1 until then. */
static atomic_uint throttle_armed;
struct cpu_npu_counter { _Alignas(64) atomic_uint_fast64_t value; };
static struct cpu_npu_counter chunks[8];
static uint64_t work_cutoff;
#ifdef SUNSTONE_A733_OS_NPU
static atomic_uint pending_overlap;
static void overlap_kick(void)
{
    unsigned pending=atomic_load_explicit(&pending_overlap,memory_order_acquire);
    if(!pending)return;
    for(unsigned c=0;c<8;++c)
        if(atomic_load_explicit(&chunks[c].value,memory_order_acquire)<=receipt.job[pending-1].a[c])return;
    /* Comparing the same generation prevents a preempted old-job worker from
     * consuming the next job's notification. Each receipt's A is immutable. */
    if(atomic_compare_exchange_strong_explicit(&pending_overlap,&pending,0,
        memory_order_acq_rel,memory_order_acquire))ss_npu_observer_kick();
}
#endif
/* First fault remains available in copied image RAM and never gets replaced. */
struct { atomic_uint claimed;uint64_t stage,core,cell,job,observed_word,expected_word;float observed,expected; } cpu_npu_first_fault;

static _Noreturn void fault(unsigned stage,unsigned core,unsigned job,uint64_t observed,uint64_t expected)
{
    unsigned clear=0;
    if(atomic_compare_exchange_strong(&cpu_npu_first_fault.claimed,&clear,1)){
        cpu_npu_first_fault.stage=stage;cpu_npu_first_fault.core=core;
        cpu_npu_first_fault.job=job;cpu_npu_first_fault.observed_word=observed;
        cpu_npu_first_fault.expected_word=expected;
        ss_fail_value((stage<<4)|core|(job<<16),(uint32_t)observed);
    }
    /* Only the claim winner records the fault, even if it was preempted. */
    for(;;)vTaskSuspend(NULL);
}
static void require(int condition,unsigned stage,unsigned core)
{if(!condition)fault(stage,core,0,0,1);}
static void live(unsigned core)
{
    int state=ss_live();if(state<0)fault(0x81,core,0,(uint32_t)state,1);
    /* Startup throttling precedes the first complete sensor set; it is not a
     * thermal event. Judge load only after the grant is complete. */
    if(atomic_load_explicit(&throttle_armed,memory_order_acquire)){
        int throttled=ss_throttled();if(throttled)fault(0x99,core,0,(uint32_t)throttled,0);}
    unsigned actual=portGET_CORE_ID();if(actual!=core)fault(0x9a,core,0,actual,core);
    ss_progress();
}
static void unit(unsigned core)
{
    struct cpu_npu_cpu *r=&receipt.cpu[core];
    unsigned cell=0;float observed=0,expected=0;
    uint64_t first=ss_counter_ticks();
    if(!cpu_npu_gemm(&matrix[core],core,&r->digest,&cell,&observed,&expected)){
        uint32_t got,want;memcpy(&got,&observed,sizeof(got));memcpy(&want,&expected,sizeof(want));
        unsigned clear=0;
        if(atomic_compare_exchange_strong(&cpu_npu_first_fault.claimed,&clear,1)){
            cpu_npu_first_fault.stage=0x82;cpu_npu_first_fault.core=core;
            cpu_npu_first_fault.cell=cell;cpu_npu_first_fault.observed=observed;
            cpu_npu_first_fault.expected=expected;cpu_npu_first_fault.observed_word=got;
            cpu_npu_first_fault.expected_word=want;
            ss_fail_value(0x820|core|(cell<<16),got);
        }
        for(;;)vTaskSuspend(NULL);
    }
    if(!r->chunks)r->first=first;
    r->last=ss_counter_ticks();++r->chunks;
    atomic_store_explicit(&chunks[core].value,r->chunks,memory_order_release);
#ifdef SUNSTONE_A733_OS_NPU
    overlap_kick();
#endif
}
static void stopped(unsigned core,unsigned epoch)
{
    receipt.cpu[core].stop=epoch;receipt.cpu[core].ack=epoch;
    atomic_store_explicit(&ack[core],epoch,memory_order_release);
}
static void worker(void *argument)
{
    unsigned core=(unsigned)(uintptr_t)argument;
    while(!atomic_load_explicit(&start_epoch,memory_order_acquire)){live(core);vTaskDelay(1);}
    for(;;){
        unsigned stop=atomic_load_explicit(&stop_epoch,memory_order_acquire);
        if(stop){stopped(core,stop);for(;;)vTaskSuspend(NULL);}
        live(core);
        /* No new unit starts after the absolute cutoff. Main publishes the
         * one stop epoch; a worker waiting for it makes no arithmetic writes. */
        if(ss_counter_ticks()>=work_cutoff){vTaskDelay(1);continue;}
        unit(core);
    }
}
static int at_top(void)
{
    char text[256];
    const char prefix[]="CLOCK cpu_l_mhz=1794 cpu_b_mhz=2002 cpu_l_mv=1000 cpu_b_mv=1050 ";
    return ss_clock_info(text,sizeof(text))>=(int)sizeof(prefix)-1&&
        !memcmp(text,prefix,sizeof(prefix)-1)&&strstr(text," dsu_mhz=1274 ")!=NULL;
}
#ifdef SUNSTONE_A733_OS_NPU
static unsigned observation;
static uint8_t output[8];
static size_t input_bytes,output_bytes;
static unsigned selected;
/* The OS owns both busy reads and all private controller/runtime identities.
 * This callback only snapshots/publishes ordinary application memory. */
static int observe(void *cookie,unsigned event,const struct ss_npu_observation *sample)
{
    (void)cookie;
    if(observation>=CPU_NPU_JOBS)fault(0x84,6,observation,observation,CPU_NPU_JOBS);
    struct cpu_npu_job *j=&receipt.job[observation];
    if(event==SS_NPU_OBSERVE_START){
        j->ticket=sample->ticket;j->token=sample->token;j->client=sample->client;j->model=sample->model;
        j->trigger=sample->trigger_ticks;j->first_busy=sample->sample_ticks;
        j->first_idle=sample->idle;j->first_state=sample->state;
        for(unsigned c=0;c<8;++c)j->a[c]=atomic_load_explicit(&chunks[c].value,memory_order_acquire);
        j->a_end=ss_counter_ticks();
        atomic_store_explicit(&pending_overlap,observation+1,memory_order_release);return 0;
    }
    if(event==SS_NPU_OBSERVE_DONE){
        atomic_store_explicit(&pending_overlap,0,memory_order_release);
        j->irq=sample->irq_ticks;j->irq_count=sample->irq_count;++observation;return 0;
    }
    if(event==SS_NPU_OBSERVE_CONFIRMED){
        j->last_busy=sample->sample_ticks;j->last_idle=sample->idle;j->last_state=sample->state;
        receipt.overlap_mask|=UINT64_C(1)<<observation;return 0;
    }
    if(!j->first_busy||j->last_busy)return 0;
    uint64_t candidate[8],since=ss_counter_ticks();
    for(unsigned c=0;c<8;++c){
        candidate[c]=atomic_load_explicit(&chunks[c].value,memory_order_acquire);
        if(candidate[c]<=j->a[c])return 0;
    }
    j->b_start=since;memcpy(j->b,candidate,sizeof(candidate));return 1;
}
static void vectors(void)
{
    struct ss_npu_model model;
    require(ss_npu_model_find("shufflenet-v2-published",&selected)==1&&
        ss_npu_model_info(selected,&model)==1,0x85,6);
    input_bytes=cpu_npu_input_bytes;output_bytes=cpu_npu_reference_bytes;
    require(input_bytes==model.input_bytes&&output_bytes==model.output_bytes&&
        input_bytes<=SS_NPU_MAX_TENSOR_BYTES&&output_bytes<=sizeof(output),0x85,6);
}

#endif
void cpu_npu_load_init(void)
{
    const struct ss_grant *g=ss_grant();
    require(g&&g->main_core==6&&g->worker_cores==0xbf,0x86,6);
#ifdef SUNSTONE_A733_OS_NPU
    require(ss_npu_observe(observe,NULL)==1,0x84,6);
#endif
    for(unsigned c=0;c<8;++c){receipt.cpu[c].core=c;cpu_npu_matrix_init(&matrix[c],c);}
    for(unsigned c=0;c<8;++c)if(c!=6)
        require(xTaskCreateStaticAffinitySet(worker,"cpu-gemm",2048,(void *)(uintptr_t)c,
            2,stacks[c],&tasks[c],(UBaseType_t)1<<c)!=NULL,0x86,c);
}
void cpu_npu_load_main(const struct ss_grant *grant)
{
    while(!ss_clock_ready()){live(6);vTaskDelay(1);}
    require(at_top(),0x87,6);receipt.initial_top=1;
    atomic_store_explicit(&throttle_armed,1,memory_order_release);
#ifdef SUNSTONE_A733_OS_NPU
    vectors();receipt.mode=1;receipt.target=CPU_NPU_JOBS;
#endif
    uint64_t now=ss_counter_ticks();
    require(grant->finish_by_us>now/24+2000000,0x88,6);
    work_cutoff=(grant->finish_by_us-2000000)*24;
    if(work_cutoff-now>120000000)work_cutoff=now+120000000;
    receipt.start=now;ss_activity_begin();
    atomic_store_explicit(&start_epoch,1,memory_order_release);
#ifdef SUNSTONE_A733_OS_NPU
    for(unsigned job=0;job<CPU_NPU_JOBS;++job){
        uint64_t admit=ss_counter_ticks();
        if(admit>=work_cutoff||work_cutoff-admit<52800000)fault(0x89,6,job,work_cutoff>admit?work_cutoff-admit:0,52800000);
        memset(output,0xa5,sizeof(output));
        for(;;){
            uint64_t now=ss_counter_ticks();if(now>=work_cutoff)fault(0x8e,6,job,now,work_cutoff);live(6);
            int ran=ss_npu_run(selected,cpu_npu_input,input_bytes,output,output_bytes,100000);
            if(ran<0)fault(0x8f,6,job,(uint32_t)ran,0);
            if(ran==1)break;
            unit(6);
        }
        struct cpu_npu_job *j=&receipt.job[job];
        if(observation!=job+1)fault(0x90,6,job,observation,job+1);
        if(!j->last_busy)fault(0x91,6,job,receipt.overlap_mask,(UINT64_C(1)<<(job+1))-1);
        if(j->irq_count!=1)fault(0x92,6,job,j->irq_count,1);
        if(!j->irq)fault(0x98,6,job,0,1);
        if(j->irq<=j->last_busy)fault(0x93,6,job,j->last_busy-j->irq,0);
        for(unsigned byte=0;byte<output_bytes;++byte)
            if(output[byte]!=cpu_npu_reference[byte])fault(0x94,6,job|(byte<<8),
                output[byte]|(uint32_t)cpu_npu_reference[byte]<<8,cpu_npu_reference[byte]);
        receipt.job[job].output_bytes=output_bytes;receipt.job[job].match=1;
        memcpy(&receipt.job[job].output,output,output_bytes);++receipt.completed;
    }
#endif
    while(ss_counter_ticks()<work_cutoff){live(6);unit(6);}
    atomic_store_explicit(&stop_epoch,1,memory_order_release);stopped(6,1);
    for(unsigned c=0;c<8;++c){
        while(atomic_load_explicit(&ack[c],memory_order_acquire)!=1){
            live(6);if(ss_counter_ticks()/24>=grant->finish_by_us)fault(0x95,6,c,atomic_load(&ack[c]),1);vTaskDelay(1);
        }
        if(!receipt.cpu[c].chunks)fault(0x96,c,0,0,1);receipt.ack_mask|=UINT64_C(1)<<c;
    }
    receipt.stop=ss_counter_ticks();receipt.stop_epoch=1;
    require(at_top(),0x87,6);receipt.final_top=1;ss_activity_end(1);
#ifdef SUNSTONE_A733_OS_NPU
    for(;;){
        require(ss_counter_ticks()/24<grant->finish_by_us,0x8c,6);live(6);
        int closed=ss_npu_close();if(closed<0)fault(0x97,6,0,(uint32_t)closed,0);
        if(closed==1)break;vTaskDelay(1);
    }
    receipt.sdk_closed=1;
#endif
    receipt.magic=CPU_NPU_MAGIC;receipt.version=1;receipt.bytes=sizeof(receipt);
    receipt.core_mask=0xff;receipt.edge=16;
    ss_finish(&receipt,sizeof(receipt));
}
void platform_puts(const char *text){(void)text;ss_fail(0x8d0);}
