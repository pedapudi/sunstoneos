/* SPDX-License-Identifier: Apache-2.0 */
#include "a733_maintenance_watchdog_internal.h"
#include "a733_storage_update_native.h"
#include "a733_counter_clock.h"
#include "a733_platform_locks.h"
#include "a733_range.h"
#include <string.h>

struct a733_maintenance_lease {
    atomic_uint claimed,state; /* live1, reload committed2, revoked4, final drain8, binding16. */
    struct a733_maintenance_transfer transfer;
    struct a733_counter_clock *clock;
    struct a733_platform_locks *locks;
    struct a733_storage_native_transport *transport;
    struct a733_storage_update *service;
    void *storage_task,*usb_task;
    uint8_t grant[A733_STORAGE_GRANT_BYTES];
    atomic_uint_fast64_t live_until,version,admitted,command,operation;
    atomic_uint_fast64_t chunk[2];
    uint64_t read_blocks,written_blocks,worker_sequence,worker_now,worker_command,worker_deadline;
    uint64_t consumed,usb_sequence,usb_hash[4],usb_now,usb_response,usb_complete;
};
static struct a733_maintenance_lease native;
struct a733_maintenance_lease *a733_maintenance_native_lease(void){return &native;}
static int present(const uint8_t *p,unsigned n)
{unsigned a=0;for(unsigned i=0;i<n;++i)a|=p[i];return a!=0;}
static int bad(struct a733_maintenance_lease *l)
{if(l==&native)atomic_fetch_or_explicit(&l->state,4,memory_order_seq_cst);return 0;}
void a733_maintenance_revoke(struct a733_maintenance_lease *l){(void)bad(l);}
static int sample(struct a733_maintenance_lease *l,uint64_t *now)
{
    if(l!=&native || !(atomic_load_explicit(&l->state,memory_order_seq_cst)&1) ||
       (atomic_load_explicit(&l->state,memory_order_seq_cst)&4) ||
       a733_counter_clock_now(l->clock,now)!=1)return bad(l);
    if(*now>=atomic_load_explicit(&l->live_until,memory_order_seq_cst) ||
       *now>=l->transfer.storage->deadline_us)return bad(l);
    return 1;
}
int a733_maintenance_alive(struct a733_maintenance_lease *l)
{uint64_t now;return sample(l,&now);}
static int bound(const struct a733_maintenance_transfer *t,uint64_t before,uint64_t *until)
{
    uint64_t reserve=t->guard_us+A733_MAINTENANCE_OPERATION_US;
    if(reserve<t->guard_us || t->minimum_reload_us<=reserve ||
       t->storage->deadline_us<=t->guard_us ||
       !a733_deadline_after(before,t->minimum_reload_us-reserve,until))return 0;
    if(*until>t->storage->deadline_us-t->guard_us)*until=t->storage->deadline_us-t->guard_us;
    return 1;
}
int a733_maintenance_bind(struct a733_maintenance_lease *l,
    const struct a733_maintenance_transfer *t,struct a733_counter_clock *clock,
    struct a733_platform_locks *locks,struct a733_storage_native_transport *transport,
    struct a733_storage_update *service,void *storage_task,void *usb_task)
{
    if(l!=&native)return 0;
    unsigned expected=0;
    if(!atomic_compare_exchange_strong(&l->claimed,&expected,1))return bad(l);
    expected=0;
    if(!atomic_compare_exchange_strong(&l->state,&expected,16))return bad(l);
    if(!t || !t->storage || !clock || !clock->initialized || clock->frequency!=24000000 ||
       !locks || !locks->initialized || locks->now!=a733_counter_clock_now || locks->context!=clock ||
       !transport || !service || !storage_task || !usb_task || storage_task==usb_task ||
       transport->service!=service || !transport->channel || transport->owner_task!=usb_task ||
       atomic_load(&transport->generation)!=t->generation || atomic_load(&transport->lost) ||
       t->magic!=A733_MAINTENANCE_TRANSFER_MAGIC || !t->epoch || !t->generation || !t->lease ||
       !present(t->image,32) || !present(t->qualification,32) ||
       memcmp(t->qualification,t->storage->recovery,32) ||
       !t->guard_us || t->reload_after_us<t->reload_before_us ||
       t->reload_after_us-t->reload_before_us>1000 ||
       t->maximum_recovery_us<t->minimum_reload_us ||
       t->reset_by_us<=t->maximum_recovery_us ||
       t->storage->deadline_us>t->reset_by_us-t->maximum_recovery_us)return bad(l);
    uint64_t until,now;
    struct a733_storage_grant checked;
    a733_storage_grant_encode(t->storage,l->grant);
    if(!a733_storage_grant_decode(&checked,l->grant,sizeof(l->grant)) || !bound(t,t->reload_before_us,&until) ||
       a733_maintenance_hw_admit(t)!=1 || a733_counter_clock_now(clock,&now)!=1 ||
       now<t->reload_after_us || now>=until)return bad(l);
    l->transfer=*t;l->clock=clock;l->locks=locks;l->transport=transport;l->service=service;
    l->storage_task=storage_task;l->usb_task=usb_task;l->worker_now=l->usb_now=now;
    atomic_init(&l->live_until,until);atomic_init(&l->version,0);
    atomic_init(&l->admitted,0);atomic_init(&l->command,0);atomic_init(&l->operation,0);
    atomic_init(&l->chunk[0],0);atomic_init(&l->chunk[1],0);
    expected=16;
    return atomic_compare_exchange_strong_explicit(&l->state,&expected,1,
        memory_order_seq_cst,memory_order_seq_cst);
}
static uint32_t word(const uint8_t *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
int a733_maintenance_storage_observe(struct a733_maintenance_lease *l,const struct a733_storage_update *s)
{
    uint64_t now;
    if(l!=&native || s!=l->service || !a733_maintenance_hw_task(l->storage_task) || !sample(l,&now))return bad(l);
    uint8_t grant[A733_STORAGE_GRANT_BYTES];a733_storage_grant_encode(&s->grant,grant);
    if(now<l->worker_now || s->last_now>now || s->self!=s ||
       !a733_storage_update_alive(s) || !a733_storage_update_owned(s,s->platform) ||
       memcmp(grant,l->grant,sizeof(grant)) || s->state==A733_STORAGE_RETAINED ||
       s->sequence<l->worker_sequence || s->sequence-l->worker_sequence>1 ||
       s->read_blocks<l->read_blocks || s->written_blocks<l->written_blocks ||
       s->read_blocks>s->grant.blocks || s->written_blocks>s->grant.blocks ||
       (s->sequence==l->worker_sequence &&
        (s->active_command!=l->worker_command || s->operation_deadline!=l->worker_deadline)))return bad(l);
    if(s->sequence && (!s->operation_deadline || s->operation_deadline>s->grant.deadline_us ||
       (s->state!=A733_STORAGE_IDLE && now>=s->operation_deadline) ||
       (s->sequence!=l->worker_sequence && s->operation_deadline>now &&
        s->operation_deadline-now>A733_MAINTENANCE_OPERATION_US)))return bad(l);
    uint64_t read=s->read_blocks-l->read_blocks,write=s->written_blocks-l->written_blocks;
    if(read || write) {
        if((read && write) || read>2 || write>2 || s->state!=A733_STORAGE_IDLE ||
           now>=s->operation_deadline || s->result.error!=SUNXI_MMC_OK ||
           s->result.received!=s->chunk_bytes || s->chunk_bytes!=(read+write)*512 ||
           (read && s->active_command!=A733_STORAGE_BACKUP) ||
           (write && s->active_command!=A733_STORAGE_WRITE) || s->response_size<56 ||
           word(s->response)!=s->sequence || word(s->response+4))return bad(l);
    }
    uint64_t version=atomic_load_explicit(&l->version,memory_order_seq_cst);
    if(version>UINT64_MAX-2)return bad(l);
    atomic_store_explicit(&l->version,version+1,memory_order_seq_cst);
    atomic_store_explicit(&l->admitted,s->sequence,memory_order_seq_cst);
    atomic_store_explicit(&l->command,s->active_command,memory_order_seq_cst);
    atomic_store_explicit(&l->operation,s->state==A733_STORAGE_IDLE?0:s->operation_deadline,memory_order_seq_cst);
    if(read || write) {
        atomic_store_explicit(&l->chunk[1],atomic_load_explicit(&l->chunk[0],memory_order_seq_cst),memory_order_seq_cst);
        atomic_store_explicit(&l->chunk[0],s->sequence,memory_order_seq_cst);
    }
    atomic_store_explicit(&l->version,version+2,memory_order_seq_cst);
    l->read_blocks=s->read_blocks;l->written_blocks=s->written_blocks;
    l->worker_sequence=s->sequence;l->worker_now=now;
    l->worker_command=s->active_command;l->worker_deadline=s->operation_deadline;
    if(s->finished)atomic_fetch_or_explicit(&l->state,8,memory_order_seq_cst);
    return 1;
}
int a733_maintenance_poll(struct a733_maintenance_lease *l)
{
    uint64_t now;
    if(l!=&native || !a733_maintenance_hw_task(l->usb_task) || !sample(l,&now))return bad(l);
    struct a733_storage_native_transport *t=l->transport;
    if(now<l->usb_now || atomic_load_explicit(&t->lost,memory_order_seq_cst) ||
       atomic_load_explicit(&t->channel->lost,memory_order_seq_cst))return bad(l);
    l->usb_now=now;
    uint64_t tv=atomic_load_explicit(&t->version,memory_order_seq_cst);
    uint64_t generation=atomic_load_explicit(&t->generation,memory_order_seq_cst);
    uint64_t seq=atomic_load_explicit(&t->accepted_sequence,memory_order_seq_cst);
    uint64_t command=atomic_load_explicit(&t->accepted_command,memory_order_seq_cst);
    uint64_t response=atomic_load_explicit(&t->response_sequence,memory_order_seq_cst);
    uint64_t complete=atomic_load_explicit(&t->completed_sequence,memory_order_seq_cst);
    uint64_t connected=atomic_load_explicit(&t->connected,memory_order_seq_cst),hash[4];
    for(unsigned i=0;i<4;++i)hash[i]=atomic_load_explicit(&t->accepted_hash[i],memory_order_seq_cst);
    if((tv&1) || tv!=atomic_load_explicit(&t->version,memory_order_seq_cst))return 1;
    if(generation!=l->transfer.generation || seq<l->usb_sequence || seq>UINT32_MAX ||
       complete<l->usb_complete || response<l->usb_response ||
       complete>response || response>seq || (seq && !connected))return bad(l);
    if(seq==l->usb_sequence && memcmp(hash,l->usb_hash,sizeof(hash)))return bad(l);
    if(seq!=l->usb_sequence){l->usb_sequence=seq;memcpy(l->usb_hash,hash,sizeof(hash));}
    l->usb_response=response;l->usb_complete=complete;
    if(command==A733_STORAGE_FINISH)atomic_fetch_or_explicit(&l->state,8,memory_order_seq_cst);
    uint64_t v=atomic_load_explicit(&l->version,memory_order_seq_cst);
    uint64_t admitted=atomic_load_explicit(&l->admitted,memory_order_seq_cst);
    uint64_t op=atomic_load_explicit(&l->command,memory_order_seq_cst);
    uint64_t deadline=atomic_load_explicit(&l->operation,memory_order_seq_cst);
    uint64_t a=atomic_load_explicit(&l->chunk[0],memory_order_seq_cst);
    uint64_t b=atomic_load_explicit(&l->chunk[1],memory_order_seq_cst);
    if((v&1) || v!=atomic_load_explicit(&l->version,memory_order_seq_cst))return 1;
    if(deadline && now>=deadline)return bad(l);
    if((atomic_load_explicit(&l->state,memory_order_seq_cst)&8) ||
       !complete || complete<=l->consumed || (complete!=a && complete!=b) ||
       seq!=complete+1 || admitted!=seq || op!=command)return 1;
    uint64_t before,after,until;
    uint64_t limit=atomic_load_explicit(&l->live_until,memory_order_seq_cst);
    if(!a733_maintenance_hw_reload(&l->transfer,l->clock,l->locks,l->usb_task,&l->state,limit,&before,&after) ||
       before<now || after<before || after-before>l->transfer.guard_us ||
       !bound(&l->transfer,before,&until) || after>=until)return bad(l);
    l->consumed=complete;l->usb_now=after;
    atomic_store_explicit(&l->live_until,until,memory_order_seq_cst);return 1;
}
