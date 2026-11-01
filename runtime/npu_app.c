#include "sunstone_app.h"
#include "sunstone_npu_service.h"
#include <stdatomic.h>
#include <string.h>
/* A single admitted application owns this mailbox. The request buffers remain
 * application-owned but unavailable to it until completion. */
enum { UNBOUND, IDLE, RUN, CLOSE, DONE, FAILED, CLOSED };
static struct {
    atomic_uint state;
    struct ss_npu_backend backend;
    void (*notify)(void *);
    void *event_context;
    unsigned model,operation;
    const void *input;
    void *output;
    size_t input_bytes,output_bytes;
    uint64_t timeout;
    int result;
} box;
static void notify_owner(void)
{if(box.notify)box.notify(box.event_context);}
#ifndef SS_NPU_BEFORE_TRANSITION
#define SS_NPU_BEFORE_TRANSITION() ((void)0)
#endif
static int transition(unsigned expected,unsigned desired)
{
    SS_NPU_BEFORE_TRANSITION();
    return atomic_compare_exchange_strong_explicit(&box.state,&expected,desired,
        memory_order_acq_rel,memory_order_acquire);
}
int ss_npu_service_bind(const struct ss_npu_backend *backend)
{
    if(!backend||!backend->run||!backend->close||atomic_load(&box.state)!=UNBOUND)return 0;
    if(!backend->models||!backend->count)return 0;
    for(unsigned i=0;i<backend->count;++i) {
        const struct ss_npu_model *m=&backend->models[i];
        if(!m->name[0]||!memchr(m->name,0,sizeof(m->name))||!m->inputs||!m->outputs||
           !m->input_bytes||m->input_bytes>SS_NPU_MAX_TENSOR_BYTES||
           !m->output_bytes||m->output_bytes>SS_NPU_MAX_TENSOR_BYTES)return 0;
        for(unsigned j=0;j<i;++j)if(!strcmp(m->name,backend->models[j].name))return 0;
    }
    box.backend=*backend;atomic_store_explicit(&box.state,IDLE,memory_order_release);return 1;
}
unsigned ss_npu_model_count(void)
{
    unsigned state=atomic_load_explicit(&box.state,memory_order_acquire);
    return state==UNBOUND||state==FAILED||state==CLOSED?0:box.backend.count;
}
int ss_npu_model_info(unsigned index,struct ss_npu_model *model)
{
    if(!model||index>=ss_npu_model_count())return 0;
    *model=box.backend.models[index];return 1;
}
int ss_npu_model_find(const char *name,unsigned *index)
{
    if(!name||!index)return 0;
    unsigned count=ss_npu_model_count();
    for(unsigned i=0;i<count;++i)if(!strcmp(name,box.backend.models[i].name)){*index=i;return 1;}
    return 0;
}
int ss_npu_service_bind_event(void (*notify)(void *),void *context)
{
    if(!notify||box.notify||atomic_load(&box.state)!=UNBOUND)return 0;
    box.notify=notify;box.event_context=context;return 1;
}
void ss_npu_service_fail(void)
{
    if(atomic_exchange_explicit(&box.state,FAILED,memory_order_release)!=FAILED)
        notify_owner();
}
int ss_npu_run(unsigned model,const void *input,size_t input_bytes,
               void *output,size_t output_bytes,uint64_t timeout_us)
{
    unsigned state=atomic_load_explicit(&box.state,memory_order_acquire);
    if(state==UNBOUND||state==FAILED||state==CLOSED)return -1;
    if(!input||!output||!input_bytes||!output_bytes||input_bytes>SS_NPU_MAX_TENSOR_BYTES||
       output_bytes>SS_NPU_MAX_TENSOR_BYTES||!timeout_us||timeout_us>1000000)return -1;
    if(state==IDLE) {
        if(model>=box.backend.count||input_bytes!=box.backend.models[model].input_bytes||
           output_bytes!=box.backend.models[model].output_bytes)return -2;
        box.model=model;box.input=input;box.input_bytes=input_bytes;
        box.output=output;box.output_bytes=output_bytes;box.timeout=timeout_us;box.operation=RUN;
        if(!transition(IDLE,RUN))return -1;
        notify_owner();return 0;
    }
    if(box.operation!=RUN||box.model!=model||box.input!=input||box.input_bytes!=input_bytes||
       box.output!=output||box.output_bytes!=output_bytes||box.timeout!=timeout_us)return -1;
    if(state!=DONE)return 0;
    int result=box.result;return transition(DONE,IDLE)?result:-1;
}
int ss_npu_close(void)
{
    unsigned state=atomic_load_explicit(&box.state,memory_order_acquire);
    if(state==CLOSED)return 1;
    if(state==UNBOUND||state==FAILED)return -1;
    if(state==IDLE){
        box.operation=CLOSE;if(!transition(IDLE,CLOSE))return -1;
        notify_owner();return 0;
    }
    if(box.operation!=CLOSE)return -1;
    if(state!=DONE)return 0;
    int result=box.result;
    return transition(DONE,result==1?CLOSED:FAILED)?result:-1;
}
int ss_npu_service_step(void)
{
    unsigned state=atomic_load_explicit(&box.state,memory_order_acquire);
    if(state==FAILED)return -1;
    if(state!=RUN&&state!=CLOSE)return 0;
    int result=state==RUN?box.backend.run(box.model,box.input,box.input_bytes,
        box.output,box.output_bytes,box.timeout):box.backend.close();
    if(result<0){ss_npu_service_fail();return -1;}
    /* Public completion distinguishes a rejected operation from pending. */
    box.result=result==1?1:-2;
    unsigned expected=state;
    if(!atomic_compare_exchange_strong_explicit(&box.state,&expected,DONE,memory_order_release,memory_order_relaxed))return -1;
    return 1;
}
