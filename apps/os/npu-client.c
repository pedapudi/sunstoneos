/* One concrete application of the task-bound API. The full catalogue remains
 * selectable through LOAD; this default caller compares one linked vector.
 * It never substitutes for any mimus model or accelerator dispatch hook. */
#include "os.h"
#include "npu.h"
#include "../board/application.h"
#include <string.h>
static struct os_npu_report published;
static uint8_t pattern_byte(size_t i){return (uint8_t)(OS_NPU_OUTPUT_PATTERN>>8*(i&3));}
/* Whole 64-bit words where aligned, then the tail bytes. */
static void fill(uint8_t *p,size_t bytes)
{
    const uint64_t word=OS_NPU_OUTPUT_PATTERN*UINT64_C(0x100000001);
    size_t i=0;
    if(!((uintptr_t)p&7))for(;i+8<=bytes;i+=8)memcpy(p+i,&word,8);
    for(;i<bytes;++i)p[i]=pattern_byte(i);
}
static uint32_t kept(const uint8_t *p,size_t bytes)
{uint32_t n=0;for(size_t i=0;i<bytes;++i)n+=p[i]==pattern_byte(i);return n;}
static void header(struct os_npu_report *r)
{
    r->magic=OS_NPU_REPORT_MAGIC;r->version=1;r->bytes=OS_NPU_REPORT_BYTES;
    r->epoch=board.session.epoch;r->generation=board.session.generation;
}
void os_npu_report_copy(uint8_t out[OS_NPU_REPORT_BYTES])
{
    if(atomic_load_explicit(&published.finished,memory_order_acquire)){memcpy(out,&published,OS_NPU_REPORT_BYTES);return;}
    struct os_npu_report pending={0};header(&pending);memcpy(out,&pending,OS_NPU_REPORT_BYTES);
}
void os_npu_client_task(void *unused)
{
    (void)unused;struct os_npu_report report={0},*r=&report;
    const struct os_session *s=os_session();
    header(r);r->model=os_npu_client_model;r->jobs_selected=s->npu.jobs;r->pattern=OS_NPU_OUTPUT_PATTERN;
    for(;;) {
        int opened=os_npu_open();
        if(opened==1)break;
        if(opened<0||!board_live(NULL))goto retained;
        if(os_npu_stopped()){r->refused=1;goto done;}
        (void)ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(25));
    }
    if(os_npu_stopping()){r->refused=1;goto close;}
    if(os_npu_load(r->model)!=1)goto retained;
    for(unsigned job=0;job<s->npu.jobs;++job) {
        if(os_npu_stopping()){r->refused=1;break;}
        for(unsigned i=0;i<os_npu_input_count;++i) {
            struct npu_runtime_tensor tensor;
            if(!os_npu_tensor(NBG_INPUT,i,&tensor)||tensor.bytes!=os_npu_inputs[i].bytes)goto retained;
            memcpy(tensor.data,os_npu_inputs[i].data,tensor.bytes);
        }
        for(unsigned i=0;i<os_npu_output_count;++i) {
            struct npu_runtime_tensor tensor;
            if(!os_npu_tensor(NBG_OUTPUT,i,&tensor)||tensor.bytes!=os_npu_references[i].bytes)goto retained;
            fill(tensor.data,tensor.bytes);
        }
        int submitted=os_npu_submit(1000000);
        if(!submitted){r->refused=1;break;}
        if(submitted<0)goto retained;
        struct storage_digest digest;storage_digest_init(&digest);
        unsigned mismatch=0;uint32_t bytes=0,pattern=0;
        for(unsigned i=0;i<os_npu_output_count;++i) {
            struct npu_runtime_tensor tensor;
            if(!os_npu_tensor(NBG_OUTPUT,i,&tensor)||tensor.bytes!=os_npu_references[i].bytes)goto retained;
            if(!storage_digest_add(&digest,tensor.data,tensor.bytes))goto retained;
            if(memcmp(tensor.data,os_npu_references[i].data,tensor.bytes))mismatch=1;
            bytes+=(uint32_t)tensor.bytes;pattern+=kept(tensor.data,tensor.bytes);
        }
        storage_digest_end(&digest,r->last_digest);++r->jobs;r->mismatches+=mismatch;
        r->output_bytes=bytes;r->pattern_bytes=pattern;
    }
close:
    if(os_npu_close()!=1)goto retained;
    while(!os_npu_stopped()) {
        if(!board_live(NULL))goto retained;
        (void)ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(25));
    }
done:
    /* One publication: the copy completes before finished becomes visible. */
    r->stopped=os_npu_stopped();
    memcpy((uint8_t *)&published+16,(const uint8_t *)r+16,OS_NPU_REPORT_BYTES-16);
    memcpy(&published,r,12);
    atomic_store_explicit(&published.finished,1,memory_order_release);vTaskSuspend(NULL);return;
retained:
    /* Every NPU resource stays held; board_fail ends the lifetime. */
    r->retained=1;board_fail(NULL);goto done;
}
