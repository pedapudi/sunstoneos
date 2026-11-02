/* One concrete application of the task-bound API. Every linked catalogue
 * entry is selectable through LOAD; this caller compares one selected vector.
 * The client exercises generic model sessions and accelerator dispatch. */
#include "os.h"
#include "npu.h"
#include "application.h"
#include "sunstone_npu_service.h"
#include "sunstone_crumb.h"
#include "../board/application.h"
#include <string.h>
struct os_npu_report os_npu_published_report;
#define published os_npu_published_report
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
/* A failure return before the service ran publishes the report that says so
 * (OS_NPU_REPORT_NOT_STARTED), once and only when the service itself reports
 * that it never ran. A client report that exists is never replaced. */
void os_npu_report_unstarted(uint64_t epoch,uint64_t generation)
{
    if(!os_npu_never_ran()||atomic_load_explicit(&published.finished,memory_order_acquire))return;
    struct os_npu_report r={0};
    r.magic=OS_NPU_REPORT_MAGIC;r.version=1;r.bytes=OS_NPU_REPORT_BYTES;r.epoch=epoch;r.generation=generation;
    r.stopped=1;
    memcpy((uint8_t *)&published+16,(const uint8_t *)&r+16,OS_NPU_REPORT_BYTES-16);
    memcpy(&published,&r,12);
    atomic_store_explicit(&published.finished,OS_NPU_REPORT_NOT_STARTED,memory_order_release);
}
int os_npu_complete(void)
{
    return atomic_load_explicit(&published.finished,memory_order_acquire)&&
        published.stopped&&!published.retained&&os_npu_stopped();
}
static uint32_t sdk_jobs,sdk_output_bytes;
/* The existing privileged slot owner services the application's mailbox. */
static unsigned sdk_loaded,sdk_model;
static int sdk_run(unsigned model,const void *input,size_t input_bytes,
                   void *output,size_t output_bytes,uint64_t timeout)
{
    if(model>=os_npu_model_count)return 0;
    struct npu_runtime_tensor t;
    int r;
    if(!sdk_loaded||sdk_model!=model||!os_npu_tensor(NBG_INPUT,0,&t)) {
        sdk_loaded=0;r=os_npu_load(model);if(r!=1)return r;
        sdk_model=model;sdk_loaded=1;
    }
    size_t bytes=0;
    for(unsigned i=0;os_npu_tensor(NBG_INPUT,i,&t);++i) {
        if(t.bytes>SIZE_MAX-bytes)return 0;bytes+=t.bytes;
    }
    if(bytes!=input_bytes)return 0;
    bytes=0;
    for(unsigned i=0;os_npu_tensor(NBG_OUTPUT,i,&t);++i) {
        if(t.bytes>SIZE_MAX-bytes)return 0;bytes+=t.bytes;
    }
    if(bytes!=output_bytes)return 0;
    size_t offset=0;
    for(unsigned i=0;os_npu_tensor(NBG_INPUT,i,&t);++i) {
        memcpy(t.data,(const uint8_t *)input+offset,t.bytes);offset+=t.bytes;
    }
    /* Guard notes retain the local model in aux and copied extent in value. */
    if(offset!=input_bytes) {
        board_fail_note(SUNSTONE_CRUMB_FAIL_SITE(NPU)|((model&0x7ffu)<<16),
            (uint32_t)offset,SUNSTONE_CRUMB_HERE());
        return -1;
    }
    r=os_npu_submit(timeout);if(r!=1)return r;
    offset=0;
    for(unsigned i=0;os_npu_tensor(NBG_OUTPUT,i,&t);++i) {
        memcpy((uint8_t *)output+offset,t.data,t.bytes);offset+=t.bytes;
    }
    if(offset!=output_bytes) {
        board_fail_note(SUNSTONE_CRUMB_FAIL_SITE(NPU)|((model&0x7ffu)<<16),
            (uint32_t)offset,SUNSTONE_CRUMB_HERE());
        return -1;
    }
    ++sdk_jobs;sdk_output_bytes=(uint32_t)output_bytes;
    return 1;
}
static unsigned sdk_closed;
static int sdk_close(void)
{int r=os_npu_close();if(r==1)sdk_closed=1;return r;}
int os_npu_sdk_bind(const struct npu_runtime *runtime)
{
    if(!runtime||runtime->count!=os_npu_model_count)return 0;
    for(unsigned i=0;i<os_npu_model_count;++i) {
        const struct nbg_assembly_layout *p=&runtime->plans[i];
        const struct ss_npu_model *m=&os_npu_model_info[i];
        if(m->inputs!=p->inputs||m->outputs!=p->outputs)return 0;
        size_t inputs=0,outputs=0;
        for(unsigned j=0;j<p->inputs;++j)inputs+=p->buffers[NBG_ALLOC_INPUT+j].payload_bytes;
        for(unsigned j=0;j<p->outputs;++j)outputs+=p->buffers[NBG_ALLOC_INPUT+p->inputs+j].payload_bytes;
        if(inputs!=m->input_bytes||outputs!=m->output_bytes)return 0;
    }
    const struct ss_npu_backend backend={sdk_run,sdk_close,os_npu_model_info,os_npu_model_count};
    return ss_npu_service_bind(&backend);
}
void os_npu_client_task(void *unused)
{
    (void)unused;struct os_npu_report report={0},*r=&report;
    const struct os_session *s=os_session();
    header(r);r->model=os_npu_client_model;r->jobs_selected=s->npu.jobs;r->pattern=OS_NPU_OUTPUT_PATTERN;
    for(;;) {
        if(os_npu_progress()!=1)goto retained;
        int opened=os_npu_open();
        if(opened==1)break;
        if(opened<0||!board_live(NULL))goto retained;
        if(os_npu_stopped()){r->refused=1;goto done;}
        (void)ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(25));
    }
    if(ss_application.manifest) {
        /* Application completion must follow ss_npu_close. Thermal stop also
         * closes this slot through its sole owner. */
        while(!os_npu_stopping()) {
            if(os_npu_progress()!=1||ss_npu_service_step()<0)goto retained;
#ifdef SUNSTONE_A733_OS_NPU_QUEUE
            if(os_npu_queue_sdk_step()<0)goto retained;
            if(os_npu_queue_sdk_closed()){sdk_closed=1;goto wait_stop;}
#endif
            if(sdk_closed)goto wait_stop;
            if(os_npu_stopped())goto done;
            (void)ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
        }
#ifdef SUNSTONE_A733_OS_NPU_QUEUE
        os_npu_queue_sdk_fail();
#endif
        ss_npu_service_fail();
        if(!os_npu_stopped())goto close;
        goto done;
    }
    if(os_npu_stopping()){r->refused=1;goto close;}
    /* A thermal stop after the check above makes LOAD refuse. */
    const int loaded=os_npu_load(os_npu_client_entry);
    if(!loaded){r->refused=1;goto close;}
    if(loaded<0)goto retained;
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
wait_stop:
    while(!os_npu_stopped()) {
        if(os_npu_progress()!=1)goto retained;
        (void)ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(25));
    }
done:
    if(ss_application.manifest) {
        r->jobs=sdk_jobs;r->output_bytes=sdk_output_bytes;r->pattern=0;
        if(!sdk_closed)ss_npu_service_fail();
    }
    /* One publication: the copy completes before finished becomes visible. */
    r->stopped=os_npu_stopped();
    memcpy((uint8_t *)&published+16,(const uint8_t *)r+16,OS_NPU_REPORT_BYTES-16);
    memcpy(&published,r,12);
    atomic_store_explicit(&published.finished,1,memory_order_release);vTaskSuspend(NULL);return;
retained:
    /* Every NPU resource stays held; board_fail ends the lifetime. */
#ifdef SUNSTONE_A733_OS_NPU_QUEUE
    os_npu_queue_sdk_fail();
#endif
    ss_npu_service_fail();r->retained=1;board_fail(NULL);goto done;
}
