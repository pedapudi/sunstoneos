/* One concrete application of the task-bound API. The full catalogue remains
 * selectable through LOAD; this default caller compares one linked vector.
 * It never substitutes for any mimus model or accelerator dispatch hook. */
#include "os.h"
#include "npu.h"
#include "../board/application.h"
#include <string.h>
struct os_npu_result os_npu_result;
void os_npu_client_task(void *unused)
{
    (void)unused;struct os_npu_result *r=&os_npu_result;
    const struct os_session *s=os_session();
    r->model=os_npu_client_model;
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
        int submitted=os_npu_submit(1000000);
        if(!submitted){r->refused=1;break;}
        if(submitted<0)goto retained;
        struct storage_digest digest;storage_digest_init(&digest);
        unsigned mismatch=0;
        for(unsigned i=0;i<os_npu_output_count;++i) {
            struct npu_runtime_tensor tensor;
            if(!os_npu_tensor(NBG_OUTPUT,i,&tensor)||tensor.bytes!=os_npu_references[i].bytes)goto retained;
            if(!storage_digest_add(&digest,tensor.data,tensor.bytes))goto retained;
            if(memcmp(tensor.data,os_npu_references[i].data,tensor.bytes))mismatch=1;
        }
        storage_digest_end(&digest,r->last_digest);++r->jobs;r->mismatches+=mismatch;
    }
close:
    if(os_npu_close()!=1)goto retained;
    while(!os_npu_stopped()) {
        if(!board_live(NULL))goto retained;
        (void)ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(25));
    }
done:
    atomic_store_explicit(&r->finished,1,memory_order_release);vTaskSuspend(NULL);return;
retained:
    r->retained=1;board_fail(NULL);goto done;
}
