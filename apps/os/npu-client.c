/* The OS side of the application's NPU service. The image links no graph: the application registers each one
 * (ss_npu_register_graph) and the runtime admits it by its structure. */
#include "os.h"
#include "npu.h"
#include "application.h"
#include "sunstone_npu_service.h"
#include "sunstone_crumb.h"
#include "../board/application.h"
#include <string.h>
struct os_npu_report os_npu_published_report;
#define published os_npu_published_report
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
static const struct npu_runtime *os_npu_runtime_for_sdk; /* set by os_npu_sdk_bind: its count grows with registered graphs */
static int sdk_register(const void *graph,size_t bytes,unsigned *entry)
{return os_npu_register(graph,bytes,entry)?1:0;}
/* The existing privileged slot owner services the application's mailbox. */
static unsigned sdk_loaded,sdk_model;
static int sdk_run(unsigned model,const void *input,size_t input_bytes,
                   void *output,size_t output_bytes,uint64_t timeout)
{
    if(!os_npu_runtime_for_sdk||model>=atomic_load_explicit(&os_npu_runtime_for_sdk->count,memory_order_acquire))return 0;
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
    r=os_npu_submit(timeout);if(r!=1)return r; /* 2: the job failed with an MMU exception and the NPU was recovered */
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
static void sdk_fault(struct ss_npu_fault_info *out)
{
    out->mmu_status=os_npu_last_fault.mmu_status;out->mmu_address=os_npu_last_fault.mmu_address;
    out->canary_crc=os_npu_last_fault.canary_crc;out->marks=os_npu_last_fault.marks;
    out->window_ms=os_npu_last_fault.window_ms;
}
static unsigned sdk_closed;
/* A model's description from the plan the runtime decoded when it admitted the graph. */
static int sdk_info(unsigned index,struct ss_npu_model *out)
{
    const struct npu_runtime *rt=os_npu_runtime_for_sdk;
    if(!rt||!out||index>=atomic_load_explicit(&rt->count,memory_order_acquire))return 0;
    const struct nbg_assembly_layout *p=&rt->plans[index];
    size_t inputs=0,outputs=0;
    for(unsigned j=0;j<p->inputs;++j)inputs+=p->buffers[NBG_ALLOC_INPUT+j].payload_bytes;
    for(unsigned j=0;j<p->outputs;++j)outputs+=p->buffers[NBG_ALLOC_INPUT+p->inputs+j].payload_bytes;
    out->inputs=p->inputs;out->outputs=p->outputs;out->input_bytes=inputs;out->output_bytes=outputs;
    return 1;
}
static int sdk_close(void)
{int r=os_npu_close();if(r==1)sdk_closed=1;return r;}
int os_npu_sdk_bind(const struct npu_runtime *runtime)
{
    if(!runtime)return 0;
    const struct ss_npu_backend backend={sdk_run,sdk_close,NULL,0,sdk_info};
    os_npu_runtime_for_sdk=runtime;
    return ss_npu_service_bind_fault(sdk_fault)&&ss_npu_service_bind_register(sdk_register)&&ss_npu_service_bind(&backend);
}
/* Every path that retains the NPU resources names its reason: a record with only "retained" cannot be diagnosed. The site
 * carries the line and the detail a number that is unique to the statement. */
#define RETAIN(detail) do{board_fail_note(SUNSTONE_CRUMB_FAIL_SITE(NPU),(detail),SUNSTONE_CRUMB_HERE());goto retained;}while(0)
void os_npu_client_task(void *unused)
{
    (void)unused;struct os_npu_report report={0},*r=&report;
    const struct os_session *s=os_session();
    header(r);r->model=os_npu_client_model;r->jobs_selected=s->npu.jobs;r->pattern=OS_NPU_OUTPUT_PATTERN;
    for(;;) {
        if(os_npu_progress()!=1)RETAIN(0x601);
        int opened=os_npu_open();
        if(opened==1)break;
        if(opened<0||!board_live(NULL))RETAIN(0x602);
        if(os_npu_stopped()){r->refused=1;goto done;}
        (void)ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(25));
    }
    if(ss_application.manifest) {
        /* Application completion must follow ss_npu_close. Thermal stop also
         * closes this slot through its sole owner. */
        while(!os_npu_stopping()) {
            if(os_npu_progress()!=1||ss_npu_service_step()<0)RETAIN(0x603);
#ifdef SUNSTONE_A733_OS_NPU_QUEUE
            if(os_npu_queue_sdk_step()<0)RETAIN(0x604);
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
    /* An image without an application manifest has no client: the service opens and closes. */
    r->refused=1;
close:
    if(os_npu_close()!=1)RETAIN(0x60b);
wait_stop:
    while(!os_npu_stopped()) {
        if(os_npu_progress()!=1)RETAIN(0x60c);
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
    atomic_store_explicit(&published.finished,1,memory_order_release);os_application_wake_main();vTaskSuspend(NULL);return;
retained:
    /* Every NPU resource stays held; board_fail ends the lifetime. */
#ifdef SUNSTONE_A733_OS_NPU_QUEUE
    os_npu_queue_sdk_fail();
#endif
    ss_npu_service_fail();r->retained=1;board_fail(NULL);goto done;
}
