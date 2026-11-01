/* SPDX-License-Identifier: MIT */
#include "queue_gemm-app.h"
#include "gpu_queue.h"
#ifdef SUNSTONE_GPU_GEMM_CONTROL
#include "control-program.h"
#else
#include "program.h"
#endif
#include "FreeRTOS.h"
#include "task.h"
#define ROWS 13u
#define COLUMNS 11u
#define DEPTH 19u
#define JOBS 4u
#define WAIT_TICKS 500u
#define GUARD_WORDS 16u
struct queue_gemm_result {
    uint32_t jobs, rows, columns, depth, checksum, sequence, local_bytes, guards;
    uint32_t kick_to_done_us[JOBS];
};
_Static_assert(sizeof(struct queue_gemm_result)==48,"result contract");
static int32_t a_value(unsigned row,unsigned k,unsigned sequence)
{return (int32_t)((row*3+k+sequence)%7)-3;}
static int32_t b_value(unsigned k,unsigned col,unsigned sequence)
{return (int32_t)((k+col*5+sequence*2)%9)-4;}
/* All test integers have magnitude below 2^24 and are exactly representable.
 * Construct normal FP32 bits without requiring CPU floating-point registers. */
static uint32_t fp32_integer(int32_t value)
{
    if(!value)return 0;
    uint32_t sign=value<0?UINT32_C(0x80000000):0;
    uint32_t magnitude=(uint32_t)(value<0?-value:value),exponent=0;
    for(uint32_t at=magnitude;at>1;at>>=1)++exponent;
    return sign|((exponent+127)<<23)|((magnitude<<(23-exponent))&UINT32_C(0x7fffff));
}
void queue_gemm_main(const struct ss_grant *grant)
{
    if(!(grant->devices&SS_DEVICE_GPU)||grant->region_count!=2||
       grant->regions[QUEUE_GEMM_REGION_INPUT].bytes<4*(ROWS*DEPTH+DEPTH*COLUMNS)||
       grant->regions[QUEUE_GEMM_REGION_OUTPUT].bytes<4*(ROWS*COLUMNS+2*GUARD_WORDS))ss_fail(1);
    uint32_t *input=grant->regions[QUEUE_GEMM_REGION_INPUT].base;
    const volatile uint32_t *output=grant->regions[QUEUE_GEMM_REGION_OUTPUT].base;
    const volatile uint32_t *guard=grant->regions[QUEUE_GEMM_REGION_OUTPUT].base;
    uint32_t guard_before[2*GUARD_WORDS];
    for(unsigned i=0;i<GUARD_WORDS;++i) {
        guard_before[i]=guard[i];guard_before[GUARD_WORDS+i]=guard[GUARD_WORDS+ROWS*COLUMNS+i];
    }
    const struct gpu_queue_context_config config={
        .job_us=200000,.program=&gpu_queue_program,.lease_count=2,
        .lease={{.region=QUEUE_GEMM_REGION_OUTPUT},{.region=QUEUE_GEMM_REGION_INPUT}}
    };
    struct gpu_queue_context context;
    struct gpu_queue_lease_info info[2];
    if(gpu_queue_context_create(&config,WAIT_TICKS,&context,info)!=GPU_QUEUE_OK)ss_fail(2);
    if(info[0].address!=(uintptr_t)output||info[1].address!=(uintptr_t)input)ss_fail(3);
    struct gpu_queue_job job={
        .sequence=1,.bindings=2,.argument_bytes=12,.groups={3,2,1},
        .binding={{.lease=0,.address=4*GUARD_WORDS,.sync_bytes=4*(ROWS*COLUMNS+2*GUARD_WORDS)},
                  {.lease=1,.sync_bytes=4*(ROWS*DEPTH+DEPTH*COLUMNS)}}
    };
    const uint32_t dimensions[3]={ROWS,COLUMNS,DEPTH};
    for(unsigned i=0;i<12;++i)job.arguments[i]=(unsigned char)(dimensions[i/4]>>(8*(i%4)));
    struct queue_gemm_result result={.rows=ROWS,.columns=COLUMNS,.depth=DEPTH,.local_bytes=4*gpu_queue_program.local_words};
    for(unsigned sequence=1;sequence<=JOBS;++sequence) {
        for(unsigned row=0;row<ROWS;++row)for(unsigned k=0;k<DEPTH;++k)
            input[row*DEPTH+k]=fp32_integer(a_value(row,k,sequence));
        for(unsigned k=0;k<DEPTH;++k)for(unsigned col=0;col<COLUMNS;++col)
            input[ROWS*DEPTH+k*COLUMNS+col]=fp32_integer(b_value(k,col,sequence));
        job.sequence=sequence;
        ss_progress();
        TickType_t began=xTaskGetTickCount();
        int submitted;
        while((submitted=gpu_queue_submit(context,&job))==GPU_QUEUE_PARKED) {
            if(ss_live()<0||xTaskGetTickCount()-began>=WAIT_TICKS)break;
            ss_progress();vTaskDelay(1);
        }
        if(submitted!=GPU_QUEUE_OK)ss_fail(0x400u|(sequence<<4)|(unsigned)submitted);
        struct gpu_queue_record record;
        if(gpu_queue_wait(context,&record,WAIT_TICKS)!=GPU_QUEUE_OK||
           record.result!=GPU_QUEUE_OK||record.sequence!=sequence)ss_fail(5);
        uint32_t checksum=0;
        for(unsigned row=0;row<ROWS;++row)for(unsigned col=0;col<COLUMNS;++col) {
            int32_t expected=0;
            for(unsigned k=0;k<DEPTH;++k)expected+=a_value(row,k,sequence)*b_value(k,col,sequence);
            uint32_t value=output[GUARD_WORDS+row*COLUMNS+col];
            if(value!=fp32_integer(expected))ss_fail(6);
            checksum+=(uint32_t)expected;
        }
        for(unsigned i=0;i<GUARD_WORDS;++i)
            if(guard[i]!=guard_before[i]||guard[GUARD_WORDS+ROWS*COLUMNS+i]!=guard_before[GUARD_WORDS+i])ss_fail(8);
        result.jobs=sequence;result.sequence=record.sequence;result.checksum=checksum;
        result.guards=2*GUARD_WORDS;result.kick_to_done_us[sequence-1]=record.kick_to_done_us;
    }
    if(gpu_queue_close(context,WAIT_TICKS)!=GPU_QUEUE_OK)ss_fail(7);
    ss_finish(&result,sizeof(result));
}
