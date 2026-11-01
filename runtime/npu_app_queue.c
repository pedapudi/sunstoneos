#include "sunstone_npu_queue_service.h"
#include <string.h>
enum { UNBOUND, IDLE, OPEN_REQUEST, READY, CLOSE_REQUEST, CLOSED, FAILED };
static struct ss_npu_queue *bound;
#ifndef SS_NPU_QUEUE_AFTER_IDLE_CLEAR
#define SS_NPU_QUEUE_AFTER_IDLE_CLEAR(q) ((void)(q))
#endif
static int producer(struct ss_npu_queue *q)
{return q && q == bound && q->producer == q->backend.identity(q->context) && q->backend.on_core(q->context);}
static int ready(struct ss_npu_queue *q)
{return producer(q) && atomic_load_explicit(&q->state,memory_order_acquire)==READY;}
int ss_npu_queue_service_bind(struct ss_npu_queue *q,const struct ss_npu_queue_backend *b,void *ctx)
{
    if(bound||!q||!b||!b->identity||!b->on_core||!b->request||!b->open||!b->close||
       !b->start||!b->finish||!b->cache||!b->doorbell||!b->notify||!b->wait)return 0;
    q->backend=*b;q->context=ctx;q->job_limit=UINT64_MAX;bound=q;atomic_store(&q->state,IDLE);return 1;
}
int ss_npu_queue_open(unsigned depth,struct ss_npu_queue **out)
{
    struct ss_npu_queue *q=bound;
    if(!q||!out||!depth||depth>SS_NPU_QUEUE_MAX_DEPTH||!q->backend.on_core(q->context))return -1;
    unsigned s=atomic_load_explicit(&q->state,memory_order_acquire);
    if(s==IDLE){
        q->depth=depth;q->producer=q->backend.identity(q->context);
        if(!q->producer)return -1;
        atomic_store_explicit(&q->state,OPEN_REQUEST,memory_order_release);
        q->backend.request(q->context);return 0;
    }
    if(!producer(q)||depth!=q->depth)return -1;
    if(s==READY){*out=q;return 1;}return s==OPEN_REQUEST?0:-1;
}
static int within(struct ss_npu_tensor t,struct ss_npu_cache_span s)
{return t.data&&t.bytes&&!((uintptr_t)t.data&63)&&t.bytes<=SIZE_MAX-63&&
    ((t.bytes+63)&~(size_t)63)<=s.bytes&&(uintptr_t)t.data>=s.cpu&&
    (uintptr_t)t.data-s.cpu<=s.bytes-((t.bytes+63)&~(size_t)63);}
int ss_npu_queue_service_handle(struct ss_npu_queue *q,const struct ss_npu_tensor *in,unsigned ni,
    const struct ss_npu_tensor *out,unsigned no,struct ss_npu_cache_span read,struct ss_npu_cache_span write)
{
    if(!q||atomic_load(&q->state)!=OPEN_REQUEST||q->edges_admitted||q->handles==SS_NPU_QUEUE_MAX_HANDLES||
       !in||!out||!ni||!no||ni>SS_NPU_QUEUE_MAX_TENSORS||no>SS_NPU_QUEUE_MAX_TENSORS||
       !read.cpu||!write.cpu||!read.bytes||!write.bytes||((read.cpu|read.bytes|write.cpu|write.bytes)&63)||
       read.bytes>UINTPTR_MAX-read.cpu||write.bytes>UINTPTR_MAX-write.cpu)return 0;
    for(unsigned i=0;i<ni;++i)
        if(!in[i].data||!in[i].bytes||((uintptr_t)in[i].data&63)||in[i].bytes>SIZE_MAX-63)return 0;
    for(unsigned i=0;i<no;++i)if(!within(out[i],write))return 0;
    struct ss_npu_queue_handle *h=&q->handle[q->handles++];
    memcpy(h->input,in,ni*sizeof(*in));memcpy(h->output,out,no*sizeof(*out));
    h->inputs=ni;h->outputs=no;h->read=read;h->write=write;
    for(unsigned i=0;i<ni;++i)h->dirty[SS_NPU_INPUT][i].end=in[i].bytes;
    for(unsigned i=0;i<no;++i)h->dirty[SS_NPU_OUTPUT][i].end=out[i].bytes;
    return 1;
}
static int overlaps(struct ss_npu_cache_span a,struct ss_npu_cache_span b)
{return a.cpu<b.cpu+b.bytes&&b.cpu<a.cpu+a.bytes;}
int ss_npu_queue_service_edges(struct ss_npu_queue *q,const struct ss_npu_queue_edge *edges,size_t count)
{
    if(!q||atomic_load(&q->state)!=OPEN_REQUEST||q->edges_admitted||!q->handles||
       count>SS_NPU_QUEUE_MAX_HANDLES*SS_NPU_QUEUE_MAX_TENSORS||(!edges&&count))return 0;
    for(unsigned i=0;i<q->handles;++i){
        struct ss_npu_queue_handle *h=&q->handle[i];
        if(overlaps(h->read,h->write))return 0;
        for(unsigned j=0;j<i;++j){
            struct ss_npu_queue_handle *p=&q->handle[j];
            if(overlaps(h->read,p->read)||overlaps(h->read,p->write)||
               overlaps(h->write,p->read)||overlaps(h->write,p->write))return 0;
        }
        h->group=i;q->group[i].members=1u<<i;
    }
    for(size_t i=0;i<count;++i){
        const struct ss_npu_queue_edge *e=&edges[i];
        if(e->producer>=q->handles||e->consumer>=q->handles||e->producer==e->consumer)return 0;
        struct ss_npu_queue_handle *p=&q->handle[e->producer],*c=&q->handle[e->consumer];
        if(e->output>=p->outputs||e->input>=c->inputs||c->alias[e->input].handle||
           c->input[e->input].data!=p->output[e->output].data||
           c->input[e->input].bytes!=p->output[e->output].bytes)return 0;
        c->alias[e->input].handle=e->producer+1;c->alias[e->input].output=e->output;
        /* The output's initial dirty range owns this physical allocation. */
        c->dirty[SS_NPU_INPUT][e->input]=(struct ss_npu_dirty_range){0};
        unsigned from=c->group,to=p->group;
        if(from!=to){
            q->group[to].members|=q->group[from].members;q->group[from].members=0;
            for(unsigned j=0;j<q->handles;++j)if(q->handle[j].group==from)q->handle[j].group=to;
        }
    }
    for(unsigned i=0;i<q->handles;++i)
        for(unsigned j=0;j<q->handle[i].inputs;++j)
            if(!q->handle[i].alias[j].handle&&!within(q->handle[i].input[j],q->handle[i].read))return 0;
    q->edges_admitted=1;return 1;
}
void ss_npu_queue_service_fail(struct ss_npu_queue *q,int isr)
{
    if(q&&atomic_exchange_explicit(&q->state,FAILED,memory_order_acq_rel)!=FAILED)
        q->backend.notify(q->context,q->producer,isr);
}
int ss_npu_queue_service_step(struct ss_npu_queue *q)
{
    unsigned s=atomic_load_explicit(&q->state,memory_order_acquire);int r;
    if(s==FAILED)return -1;
    if(s!=OPEN_REQUEST&&s!=CLOSE_REQUEST)return 0;
    r=s==OPEN_REQUEST?q->backend.open(q->context,q->depth):q->backend.close(q->context);
    if(r==0)return 0;
    if(r<0||(s==OPEN_REQUEST&&(!q->handles||(!q->edges_admitted&&!ss_npu_queue_service_edges(q,NULL,0))))){ss_npu_queue_service_fail(q,0);return -1;}
    /* A concurrent failure can never be overwritten by readiness. */
    unsigned expected=s;
    if(!atomic_compare_exchange_strong_explicit(&q->state,&expected,s==OPEN_REQUEST?READY:CLOSED,
        memory_order_release,memory_order_relaxed))return -1;
    q->backend.notify(q->context,q->producer,0);return 1;
}
int ss_npu_queue_register(struct ss_npu_queue *q,unsigned entry,ss_npu_handle *out)
{if(!ready(q)||!out||entry>=q->handles)return 0;*out=entry+1;return 1;}
int ss_npu_queue_tensor(struct ss_npu_queue *q,ss_npu_handle handle,enum ss_npu_tensor_kind kind,
    unsigned index,struct ss_npu_tensor *out)
{
    if(!ready(q)||!out||!handle||handle>q->handles||(unsigned)kind>SS_NPU_OUTPUT)return 0;
    const struct ss_npu_queue_handle *h=&q->handle[handle-1];
    if(q->group[h->group].device_owned||index>=(kind==SS_NPU_INPUT?h->inputs:h->outputs))return 0;
    *out=kind==SS_NPU_INPUT?h->input[index]:h->output[index];return 1;
}
int ss_npu_queue_dirty(struct ss_npu_queue *q,ss_npu_handle handle,
    enum ss_npu_tensor_kind kind,unsigned index,size_t offset,size_t bytes)
{
    struct ss_npu_tensor t;
    if(!ss_npu_queue_tensor(q,handle,kind,index,&t)||!bytes||offset>t.bytes||bytes>t.bytes-offset)return 0;
    struct ss_npu_queue_handle *h=&q->handle[handle-1];
    if(kind==SS_NPU_INPUT&&h->alias[index].handle){
        unsigned output=h->alias[index].output;h=&q->handle[h->alias[index].handle-1];
        kind=SS_NPU_OUTPUT;index=output;
    }
    struct ss_npu_dirty_range *d=&h->dirty[kind][index];
    if(!d->end||offset<d->first)d->first=offset;
    if(offset+bytes>d->end)d->end=offset+bytes;
    return 1;
}
static void clean_dirty(struct ss_npu_queue *q,struct ss_npu_queue_handle *h,unsigned kind)
{
    unsigned count=kind==SS_NPU_INPUT?h->inputs:h->outputs;
    struct ss_npu_tensor *t=kind==SS_NPU_INPUT?h->input:h->output;
    for(unsigned i=0;i<count;++i){
        struct ss_npu_dirty_range *d=&h->dirty[kind][i];
        if(!d->end)continue;
        size_t first=d->first&~(size_t)63,end=(d->end+63)&~(size_t)63;
        q->backend.cache(q->context,(uintptr_t)t[i].data+first,end-first);
        d->first=d->end=0;
    }
}
int ss_npu_queue_submit(struct ss_npu_queue *q,ss_npu_handle handle,struct ss_npu_fence after,
    uint64_t timeout,struct ss_npu_fence *out)
{
    if(!ready(q))return -1;
    uint64_t tail=atomic_load_explicit(&q->published,memory_order_relaxed);
    if(!out||!handle||handle>q->handles||!timeout||timeout>1000000||tail==UINT64_MAX||tail>=q->job_limit||
       (after.value?(after.queue!=q||after.value>tail):after.queue!=NULL)||
       tail-atomic_load_explicit(&q->released,memory_order_acquire)>=q->depth)return 0;
    struct ss_npu_queue_handle *h=&q->handle[handle-1];
    struct ss_npu_queue_group *g=&q->group[h->group];
    if(!g->device_owned){
        /* Component membership is fixed at admission. Clean every CPU-dirty
         * member before starting any job that can touch a shared tensor. */
        for(unsigned i=0;i<q->handles;++i)if(g->members&(1u<<i)){
            clean_dirty(q,&q->handle[i],SS_NPU_INPUT);clean_dirty(q,&q->handle[i],SS_NPU_OUTPUT);
        }
        g->device_owned=1;
    }
    if(atomic_load_explicit(&q->state,memory_order_acquire)!=READY)return -1;
    q->entry[tail%q->depth]=(struct ss_npu_queue_entry){handle,after.value,timeout};
    g->last_use=tail+1;
    atomic_store_explicit(&q->published,tail+1,memory_order_seq_cst);
    *out=(struct ss_npu_fence){q,tail+1};
    /* SC exchanges and the consumer's SC clear/recheck forbid both sides
     * observing the pre-publication idle state. No producer syscall occurs
     * between queued jobs, including a chain published during an IRQ. */
    if(!atomic_exchange_explicit(&q->active,1,memory_order_seq_cst)&&q->backend.doorbell(q->context)!=1){
        ss_npu_queue_service_fail(q,0);return -1;
    }
    return atomic_load_explicit(&q->state,memory_order_acquire)==FAILED?-1:1;
}
int ss_npu_queue_service_progress(struct ss_npu_queue *q,int completed,int isr)
{
    if(!q||atomic_load_explicit(&q->state,memory_order_acquire)!=READY)return -1;
    if(completed){
        if(!q->consuming||q->backend.finish(q->context,q->consuming)!=1)goto fail;
        atomic_store_explicit(&q->completed,q->consuming,memory_order_release);q->consuming=0;
    }
    if(!q->consuming){
        uint64_t done=atomic_load_explicit(&q->completed,memory_order_relaxed);
        uint64_t tail=atomic_load_explicit(&q->published,memory_order_seq_cst);
        if(tail==done){
            atomic_store_explicit(&q->active,0,memory_order_seq_cst);
            SS_NPU_QUEUE_AFTER_IDLE_CLEAR(q);
            tail=atomic_load_explicit(&q->published,memory_order_seq_cst);
            if(tail!=done)atomic_exchange_explicit(&q->active,1,memory_order_seq_cst);
        }
        if(tail!=done){
            struct ss_npu_queue_entry e=q->entry[done%q->depth];
            if(!e.handle||e.handle>q->handles||e.after>done||!e.timeout||e.timeout>1000000)goto fail;
            q->consuming=done+1;
            if(q->backend.start(q->context,e.handle,q->consuming,e.after,e.timeout)!=1)goto fail;
        }
    }
    uint64_t waiter=atomic_load_explicit(&q->waiter,memory_order_acquire);
    if(waiter&&atomic_load_explicit(&q->completed,memory_order_acquire)>=waiter){
        atomic_store_explicit(&q->waiter,0,memory_order_release);
        q->backend.notify(q->context,q->producer,isr);
    }
    return 1;
fail:ss_npu_queue_service_fail(q,isr);return -1;
}
int ss_npu_queue_status(struct ss_npu_queue *q,struct ss_npu_fence f)
{
    if(!ready(q)||f.queue!=q||!f.value||f.value>atomic_load_explicit(&q->published,memory_order_acquire))return -1;
    return atomic_load_explicit(&q->completed,memory_order_acquire)>=f.value;
}
int ss_npu_queue_acquire_handle(struct ss_npu_queue *q,ss_npu_handle handle)
{
    if(!ready(q)||!handle||handle>q->handles)return -1;
    struct ss_npu_queue_group *g=&q->group[q->handle[handle-1].group];
    if(atomic_load_explicit(&q->completed,memory_order_acquire)<g->last_use)return 0;
    if(g->device_owned){
        for(unsigned h=0;h<q->handles;++h)if(g->members&(1u<<h)){
            struct ss_npu_queue_handle *member=&q->handle[h];
            for(unsigned i=0;i<member->outputs;++i)
                q->backend.cache(q->context,(uintptr_t)member->output[i].data,
                    (member->output[i].bytes+63)&~(size_t)63);
        }
        g->device_owned=0;
    }
    return 1;
}
int ss_npu_queue_acquire(struct ss_npu_queue *q,struct ss_npu_fence f)
{
    int r=ss_npu_queue_status(q,f);if(r!=1)return r;
    if(f.value<=atomic_load_explicit(&q->released,memory_order_relaxed))return -1;
    return ss_npu_queue_acquire_handle(q,q->entry[(f.value-1)%q->depth].handle);
}
int ss_npu_queue_release(struct ss_npu_queue *q,struct ss_npu_fence f)
{
    if(ss_npu_queue_status(q,f)!=1||f.value!=atomic_load_explicit(&q->released,memory_order_relaxed)+1)return 0;
    atomic_store_explicit(&q->released,f.value,memory_order_release);return 1;
}
int ss_npu_queue_wait(struct ss_npu_queue *q,struct ss_npu_fence f)
{
    int r=ss_npu_queue_status(q,f);if(r)return r;
    atomic_store_explicit(&q->waiter,f.value,memory_order_release);
    while(!(r=ss_npu_queue_status(q,f)))q->backend.wait(q->context);
    atomic_store_explicit(&q->waiter,0,memory_order_release);return r;
}
int ss_npu_queue_close(struct ss_npu_queue *q)
{
    if(!producer(q))return -1;
    unsigned s=atomic_load_explicit(&q->state,memory_order_acquire);
    if(s==CLOSED)return 1;if(s==CLOSE_REQUEST)return 0;if(s!=READY)return -1;
    if(atomic_load_explicit(&q->published,memory_order_acquire)!=atomic_load_explicit(&q->released,memory_order_acquire))return 0;
    for(unsigned i=0;i<q->handles;++i)if(q->group[i].device_owned)return 0;
    unsigned expected=READY;
    if(!atomic_compare_exchange_strong_explicit(&q->state,&expected,CLOSE_REQUEST,memory_order_acq_rel,memory_order_acquire))return -1;
    q->backend.request(q->context);return 0;
}
