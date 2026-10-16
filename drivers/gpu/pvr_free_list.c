/* SPDX-License-Identifier: MIT
 * Free-list layout, reserve accounting and recovery adapted from pvr_free_list.c
 * and pvr_rogue_fwif.h. Copyright (c) 2023 Imagination Technologies Ltd.
 * See LICENSE.pvr.
 */
#include "pvr_free_list.h"
#include <string.h>

enum { ZERO_TABLE, ALLOCATE, ZERO_PAGE, PUBLISH_PAGE, INSERT, INIT_RECORD,
       REBUILD, READ_CURRENT, WRITE_STACK, CLEAR_COUNTS, RECOVER_TARGETS, SEND, WAIT };
static int span(const void *p, size_t n)
{ return p && n && n <= UINTPTR_MAX - (uintptr_t)p; }
static int apart(const void *a, size_t n, const void *b, size_t m)
{ return (uintptr_t)a + n <= (uintptr_t)b || (uintptr_t)b + m <= (uintptr_t)a; }
static void put(unsigned char *p, uint64_t value, unsigned n)
{ for (unsigned i=0;i<n;++i) p[i]=(unsigned char)(value>>(8*i)); }
static uint32_t get(const unsigned char *p)
{ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static unsigned reserve(const struct pvr_free_list_state *l, unsigned pages)
{
    uint64_t n=(uint64_t)pages*l->config.threshold/100;
    if(n>l->config.grow_pages)n=l->config.grow_pages;
    return (unsigned)n & ~3u;
}
static enum pvr_free_list_result fail(struct pvr_free_lists *s,unsigned error)
{
    if(s->phase!=PVR_FREE_LIST_FAILED) { s->phase=PVR_FREE_LIST_FAILED; s->error=error; }
    (void)pvr_ccb_abort(s->queue); return PVR_FREE_LIST_FAULT;
}
int pvr_free_lists_init(struct pvr_free_lists *s,const struct pvr_free_list_config *configs,unsigned count,
    unsigned minimum,const struct pvr_free_list_ops *ops,void *ctx,struct pvr_ccb *q,
    uint64_t now,uint64_t timeout)
{
    if(!span(s,sizeof(*s)) || s->phase!=PVR_FREE_LIST_NEW || !count || count>PVR_FREE_LISTS ||
       !minimum || minimum>PVR_FREE_LIST_MAX_PAGES || !span(configs,count*sizeof(*configs)) ||
       !span(ops,sizeof(*ops)) || !span(q,sizeof(*q)) || !timeout || timeout>INT64_MAX ||
       timeout>UINT64_MAX-now || !q->epoch ||
       (q->phase!=PVR_CCB_PREPARED && q->phase!=PVR_CCB_ACTIVE) ||
       !apart(s,sizeof(*s),configs,count*sizeof(*configs)) || !apart(s,sizeof(*s),ops,sizeof(*ops)) ||
       !apart(s,sizeof(*s),q,sizeof(*q)) || !ops->allocate || !ops->publish_page ||
       !ops->read || !ops->write || !ops->publish || !ops->recover_targets) return 0;
    for(unsigned i=0;i<count;++i) {
        const struct pvr_free_list_config *c=&configs[i];
        size_t bytes=(size_t)c->maximum_pages*sizeof(*c->pages);
        if(c->resource.kind!=PVR_GPU_RESOURCE_FREELIST || !c->resource.owner || !c->resource.token ||
           c->maximum_pages>PVR_FREE_LIST_MAX_PAGES || c->initial_pages<minimum ||
           c->initial_pages>c->maximum_pages || c->grow_pages>c->maximum_pages || c->threshold>100 ||
           ((c->initial_pages|c->maximum_pages|c->grow_pages)&3) ||
           (c->initial_pages<c->maximum_pages ? !c->grow_pages : c->grow_pages!=0) ||
           !c->firmware_address || (c->firmware_address&7) || c->firmware_address>UINT32_MAX-63 ||
           (c->gpu_address&15) || c->gpu_address>=(UINT64_C(1)<<40) ||
           4*(uint64_t)c->maximum_pages>(UINT64_C(1)<<40)-c->gpu_address ||
           !span(c->pages,bytes) || !apart(c->pages,bytes,s,sizeof(*s)) ||
           !apart(c->pages,bytes,q,sizeof(*q)) || !apart(c->pages,bytes,ops,sizeof(*ops)) ||
           !apart(c->pages,bytes,configs,count*sizeof(*configs))) return 0;
        for(unsigned j=0;j<i;++j) {
            const struct pvr_free_list_config *d=&configs[j];
            if(c->resource.id==d->resource.id || c->resource.token==d->resource.token ||
               (c->gpu_address < d->gpu_address+4*(uint64_t)d->maximum_pages &&
                d->gpu_address < c->gpu_address+4*(uint64_t)c->maximum_pages) ||
               !apart(c->pages,bytes,d->pages,(size_t)d->maximum_pages*sizeof(*d->pages)) ||
               ((uint64_t)c->firmware_address < (uint64_t)d->firmware_address+64 &&
                (uint64_t)d->firmware_address < (uint64_t)c->firmware_address+64)) return 0;
        }
        for(unsigned p=0;p<c->maximum_pages;++p)
            if(c->pages[p].cpu || c->pages[p].physical || c->pages[p].lease) return 0;
    }
    struct pvr_free_lists result={0};
    result.ops=*ops; result.context=ctx; result.queue=q; result.epoch=q->epoch;
    result.timeout=timeout; result.last_now=now; result.deadline=now+timeout; result.count=count;
    for(unsigned i=0;i<count;++i)result.lists[i].config=configs[i];
    result.phase=PVR_FREE_LIST_BUILDING; result.stage=ZERO_TABLE; result.goal=configs[0].initial_pages;
    *s=result; return 1;
}
static unsigned find(const struct pvr_free_lists *s,const struct pvr_gpu_event_resource *r)
{
    if(!r || r->kind!=PVR_GPU_RESOURCE_FREELIST)return s->count;
    for(unsigned i=0;i<s->count;++i) {
        const struct pvr_gpu_event_resource *a=&s->lists[i].config.resource;
        if(a->id==r->id && a->owner==r->owner && a->token==r->token)return i;
    }
    return s->count;
}
int pvr_free_lists_event(void *ctx,const struct pvr_gpu_event *e,
    const struct pvr_gpu_event_resource *const *targets,unsigned count)
{
    struct pvr_free_lists *s=ctx;
    if(!s || !e || s->phase==PVR_FREE_LIST_NEW || s->phase==PVR_FREE_LIST_FAILED)return 0;
    if(s->busy) { (void)fail(s,PVR_FREE_LIST_REENTRY); return 0; }
    if(s->phase==PVR_FREE_LIST_BUILDING || s->request_count==PVR_FREE_LIST_REQUESTS ||
       s->queue->epoch!=s->epoch || s->queue->phase!=PVR_CCB_ACTIVE ||
       s->timeout>UINT64_MAX-s->last_now || count>PVR_GPU_EVENTS_RECONSTRUCT || (count&&!targets))return 0;
    if((e->kind==PVR_GPU_EVENT_GROW ? count!=1 :
        e->kind!=PVR_GPU_EVENT_RECONSTRUCT || count!=e->data.reconstruct.count))return 0;
    struct pvr_free_list_request r={.event=*e,.deadline=s->last_now+s->timeout,.count=count};
    for(unsigned i=0;i<count;++i) {
        r.lists[i]=find(s,targets[i]);
        uint32_t id=e->kind==PVR_GPU_EVENT_GROW ? e->data.id : e->data.reconstruct.ids[i];
        if(r.lists[i]==s->count || targets[i]->id!=id)return 0;
    }
    s->requests[(s->request_head+s->request_count)%PVR_FREE_LIST_REQUESTS]=r;
    ++s->request_count; return 1;
}
static int write_publish(struct pvr_free_lists *s,enum pvr_free_list_memory memory,
                         unsigned offset,const void *data,size_t bytes)
{
    uint64_t token=s->lists[s->list].config.resource.token;
    if(s->ops.write(s->context,token,memory,offset,data,bytes)!=1) { fail(s,PVR_FREE_LIST_MEMORY_ERROR); return 0; }
    if(s->phase==PVR_FREE_LIST_FAILED)return 0;
    if(s->ops.publish(s->context,token,memory,offset,bytes)!=1) { fail(s,PVR_FREE_LIST_PUBLICATION); return 0; }
    return s->phase!=PVR_FREE_LIST_FAILED;
}
static int page_valid(struct pvr_free_lists *s,unsigned index)
{
    const struct pvr_free_list_page *p=&s->lists[s->list].config.pages[index];
    if(!span(p->cpu,4096) || ((uintptr_t)p->cpu&4095) || !p->physical || (p->physical&4095) ||
       p->physical>=(UINT64_C(1)<<36) || !p->lease || !apart(p->cpu,4096,s,sizeof(*s)) ||
       !apart(p->cpu,4096,s->queue,sizeof(*s->queue)))return 0;
    for(unsigned i=0;i<s->count;++i) {
        const struct pvr_free_list_state *l=&s->lists[i];
        if(!apart(p->cpu,4096,l->config.pages,(size_t)l->config.maximum_pages*sizeof(*p)))return 0;
        for(unsigned j=0;j<l->acquired;++j) {
            if(i==s->list && j==index)continue;
            const struct pvr_free_list_page *a=&l->config.pages[j];
            if(p->physical==a->physical || p->lease==a->lease || !apart(p->cpu,4096,a->cpu,4096))return 0;
        }
    }
    return 1;
}
static void response_grow(struct pvr_free_lists *s,unsigned delta)
{
    struct pvr_free_list_state *l=&s->lists[s->list];
    memset(s->response,0,sizeof(s->response)); put(s->response,PVR_CCB_MAGIC|110u,4);
    put(s->response+8,l->config.firmware_address,4); put(s->response+12,delta,4);
    put(s->response+16,l->active,4); put(s->response+20,l->ready,4); s->stage=SEND;
}
static void response_reconstruct(struct pvr_free_lists *s)
{
    const struct pvr_free_list_request *r=&s->requests[s->request_head];
    memset(s->response,0,sizeof(s->response)); put(s->response,PVR_CCB_MAGIC|112u,4);
    put(s->response+8,r->count,4);
    for(unsigned i=0;i<r->count;++i)put(s->response+12+4*i,r->event.data.reconstruct.ids[i],4);
    s->stage=SEND;
}
static enum pvr_free_list_result step(struct pvr_free_lists *s,uint64_t now)
{
    if(s->phase==PVR_FREE_LIST_IDLE) {
        if(!s->request_count)return PVR_FREE_LIST_OK;
        const struct pvr_free_list_request *r=&s->requests[s->request_head];
        s->deadline=r->deadline; s->request_index=0; s->phase=PVR_FREE_LIST_WORKING;
        if(!r->count) { response_reconstruct(s); return PVR_FREE_LIST_AGAIN; }
        s->list=r->lists[0]; s->cursor=0;
        struct pvr_free_list_state *l=&s->lists[s->list];
        if(r->event.kind==PVR_GPU_EVENT_RECONSTRUCT)s->stage=REBUILD;
        else {
            l->current=l->active; l->ready=0; s->old_active=l->active;
            if(!l->config.grow_pages || l->config.grow_pages>l->config.maximum_pages-l->active)
                response_grow(s,0);
            else { s->goal=l->active+l->config.grow_pages; s->stage=ALLOCATE; }
        }
        return PVR_FREE_LIST_AGAIN;
    }
    struct pvr_free_list_state *l=&s->lists[s->list];
    unsigned char data[PVR_FREE_LIST_CHUNK*4]={0};
    switch(s->stage) {
    case ZERO_TABLE: {
        unsigned n=l->config.maximum_pages-s->cursor; if(n>PVR_FREE_LIST_CHUNK)n=PVR_FREE_LIST_CHUNK;
        if(!write_publish(s,PVR_FREE_LIST_TABLE,s->cursor*4,data,n*4))return PVR_FREE_LIST_FAULT;
        s->cursor+=n; if(s->cursor==l->config.maximum_pages) { s->cursor=0; s->stage=ALLOCATE; }
        break;
    }
    case ALLOCATE: {
        if(l->acquired>=s->goal) { s->cursor=l->active; s->stage=INSERT; break; }
        unsigned slot=l->acquired;
        struct pvr_free_list_page page={0};
        int result=s->ops.allocate(s->context,s->epoch,l->config.resource.owner,l->config.resource.token,slot,&page);
        /* Preserve returned ownership before inspecting result or making any
         * other callback. The current list/slot also identify a failed attempt. */
        l->config.pages[slot]=page;
        if(result || page.cpu || page.physical || page.lease)++l->acquired;
        if(s->phase==PVR_FREE_LIST_FAILED)return PVR_FREE_LIST_FAULT;
        if(result==0 && !page.cpu && !page.physical && !page.lease) {
            if(s->phase==PVR_FREE_LIST_BUILDING)return fail(s,PVR_FREE_LIST_ALLOCATION);
            response_grow(s,0); break;
        }
        if(result!=1 || !page_valid(s,slot))return fail(s,PVR_FREE_LIST_ALLOCATION);
        s->stage=ZERO_PAGE; break;
    }
    case ZERO_PAGE:
        memset(l->config.pages[l->acquired-1].cpu,0,4096); s->stage=PUBLISH_PAGE; break;
    case PUBLISH_PAGE:
        if(s->ops.publish_page(s->context,&l->config.pages[l->acquired-1])!=1)return fail(s,PVR_FREE_LIST_PUBLICATION);
        if(s->phase==PVR_FREE_LIST_FAILED)return PVR_FREE_LIST_FAULT;
        s->stage=ALLOCATE; break;
    case INSERT: {
        unsigned n=s->goal-s->cursor; if(n>PVR_FREE_LIST_CHUNK)n=PVR_FREE_LIST_CHUNK;
        for(unsigned i=0;i<n;++i)put(data+i*4,l->config.pages[s->cursor+i].physical>>12,4);
        unsigned offset=(l->config.maximum_pages-s->goal+s->cursor-l->active)*4;
        if(!write_publish(s,PVR_FREE_LIST_TABLE,offset,data,n*4))return PVR_FREE_LIST_FAULT;
        s->cursor+=n;
        if(s->cursor==s->goal) {
            l->active=s->goal; l->ready=reserve(l,l->active); l->current=l->active-l->ready;
            if(s->phase==PVR_FREE_LIST_BUILDING)s->stage=INIT_RECORD;
            else response_grow(s,l->active-s->old_active);
        }
        break;
    }
    case INIT_RECORD:
        put(data,l->config.gpu_address,8);
        put(data+8,(l->config.gpu_address+4*(uint64_t)(l->config.maximum_pages-l->current))&~UINT64_C(15),8);
        put(data+16,l->current-1,4); put(data+20,l->config.maximum_pages,4); put(data+24,l->config.grow_pages,4);
        put(data+28,l->current,4); put(data+40,l->config.resource.id,4); put(data+48,l->ready,4);
        if(!write_publish(s,PVR_FREE_LIST_FIRMWARE,0,data,64))return PVR_FREE_LIST_FAULT;
        if(++s->list==s->count) { s->list=0; s->phase=PVR_FREE_LIST_IDLE; return PVR_FREE_LIST_OK; }
        s->goal=s->lists[s->list].config.initial_pages; s->cursor=0; s->stage=ZERO_TABLE; break;
    case REBUILD: {
        if(s->cursor==l->active) { s->stage=READ_CURRENT; break; }
        unsigned block=s->cursor<l->config.initial_pages ? 0 : l->config.initial_pages+
            ((s->cursor-l->config.initial_pages)/l->config.grow_pages)*l->config.grow_pages;
        unsigned size=block ? l->config.grow_pages : l->config.initial_pages;
        unsigned n=block+size-s->cursor; if(n>PVR_FREE_LIST_CHUNK)n=PVR_FREE_LIST_CHUNK;
        for(unsigned i=0;i<n;++i)put(data+4*i,l->config.pages[s->cursor+i].physical>>12,4);
        unsigned offset=(l->config.maximum_pages-block-size+s->cursor-block)*4;
        if(!write_publish(s,PVR_FREE_LIST_TABLE,offset,data,n*4))return PVR_FREE_LIST_FAULT;
        s->cursor+=n; break;
    }
    case READ_CURRENT:
        if(s->ops.read(s->context,l->config.resource.token,PVR_FREE_LIST_FIRMWARE,28,data,4)!=1)
            return fail(s,PVR_FREE_LIST_MEMORY_ERROR);
        if(s->phase==PVR_FREE_LIST_FAILED)return PVR_FREE_LIST_FAULT;
        s->firmware_current=get(data);
        if(s->firmware_current>l->active)return fail(s,PVR_FREE_LIST_MEMORY_ERROR);
        l->current=l->active-l->ready; s->stage=WRITE_STACK; break;
    case WRITE_STACK:
        put(data,s->firmware_current-1,4);
        if(!write_publish(s,PVR_FREE_LIST_FIRMWARE,16,data,4))return PVR_FREE_LIST_FAULT;
        s->stage=CLEAR_COUNTS; break;
    case CLEAR_COUNTS:
        if(!write_publish(s,PVR_FREE_LIST_FIRMWARE,32,data,8))return PVR_FREE_LIST_FAULT;
        s->stage=RECOVER_TARGETS; break;
    case RECOVER_TARGETS: {
        if(s->ops.recover_targets(s->context,l->config.resource.token)!=1)return fail(s,PVR_FREE_LIST_RECOVERY);
        if(s->phase==PVR_FREE_LIST_FAILED)return PVR_FREE_LIST_FAULT;
        const struct pvr_free_list_request *r=&s->requests[s->request_head];
        if(++s->request_index==r->count)response_reconstruct(s);
        else { s->list=r->lists[s->request_index]; s->cursor=0; s->stage=REBUILD; }
        break;
    }
    case SEND: {
        enum pvr_ccb_result r=pvr_ccb_submit(s->queue,s->response,now,s->deadline,&s->response_token);
        if(r==PVR_CCB_AGAIN)break;
        if(r!=PVR_CCB_OK)return fail(s,PVR_FREE_LIST_QUEUE_ERROR);
        s->stage=WAIT; break;
    }
    case WAIT: {
        uint32_t status;
        enum pvr_ccb_result r=pvr_ccb_collect(s->queue,s->response_token,&status);
        if(r==PVR_CCB_AGAIN)break;
        if(r!=PVR_CCB_OK || status!=1)return fail(s,PVR_FREE_LIST_QUEUE_ERROR);
        s->request_head=(s->request_head+1)%PVR_FREE_LIST_REQUESTS; --s->request_count;
        s->phase=PVR_FREE_LIST_IDLE; return s->request_count ? PVR_FREE_LIST_AGAIN : PVR_FREE_LIST_OK;
    }
    default:return fail(s,PVR_FREE_LIST_INVALID);
    }
    return PVR_FREE_LIST_AGAIN;
}
enum pvr_free_list_result pvr_free_lists_poll(struct pvr_free_lists *s,uint64_t now)
{
    if(!s || s->phase==PVR_FREE_LIST_NEW || s->phase==PVR_FREE_LIST_FAILED)return PVR_FREE_LIST_FAULT;
    if(s->busy)return fail(s,PVR_FREE_LIST_REENTRY);
    if(s->queue->epoch!=s->epoch ||
       (s->queue->phase!=PVR_CCB_PREPARED && s->queue->phase!=PVR_CCB_ACTIVE))return fail(s,PVR_FREE_LIST_QUEUE_ERROR);
    if(now<s->last_now || now<s->queue->last_now)return fail(s,PVR_FREE_LIST_CLOCK);
    s->last_now=now;
    uint64_t deadline=s->phase==PVR_FREE_LIST_IDLE && s->request_count ? s->requests[s->request_head].deadline : s->deadline;
    if((s->phase!=PVR_FREE_LIST_IDLE || s->request_count) && now>=deadline)return fail(s,PVR_FREE_LIST_TIMEOUT);
    s->busy=1; enum pvr_free_list_result result=step(s,now); s->busy=0; return result;
}
