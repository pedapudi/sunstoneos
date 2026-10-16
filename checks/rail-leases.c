#include "a733_rail_leases.h"
#include "a733_gpu_clock_parent.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct model {
    struct a733_clock_leases clocks;
    struct a733_gpu_resource_manager gpu;
    struct a733_rail_leases rails;
    uint64_t time;
    unsigned times, io, lock_gpu_at, late_at, mutate_at;
    struct a733_rail_token *mutate;
};
static int now(void *p,uint64_t *out)
{
    struct model *m=p;++m->times;
    if(m->times==m->lock_gpu_at)atomic_store(&m->gpu.busy,1);
    if(m->times==m->late_at)m->time=1000;
    if(m->times==m->mutate_at)*m->mutate=(struct a733_rail_token){.id=999};
    *out=m->time;return 1;
}
static int read32(void *p,uintptr_t a,uint32_t *v)
{(void)a;(void)v;++((struct model *)p)->io;assert(0);return 0;}
static int update32(void *p,uintptr_t a,uint32_t r,uint32_t s,uint64_t d)
{(void)a;(void)r;(void)s;(void)d;++((struct model *)p)->io;assert(0);return 0;}
static int rail(void *p,struct a733_gpu_resource_rail *r,uint64_t d)
{
    (void)d;++((struct model *)p)->io;
    *r=(struct a733_gpu_resource_rail){.chip=3,.enable=8,.voltage=30};return 1;
}
static void init(struct model *m)
{
    *m=(struct model){.time=1};
    struct a733_gpu_resource_ops ops={now,read32,update32,rail};
    assert(a733_clock_leases_init(&m->clocks,now,m));
    assert(a733_gpu_resources_init(&m->gpu,&ops,m));
    struct a733_rail_binding b={&m->clocks,&m->gpu,99,now,m};
    assert(a733_rail_leases_init(&m->rails,&b));
}
#define D2 A733_RAIL_BIT(A733_RAIL_DCDC2)
#define D4 A733_RAIL_BIT(A733_RAIL_DCDC4)
static void scopes(void)
{
    struct model m;init(&m);struct a733_clock_token twi,radio,sdc,out;
    assert(A733_CLOCK_SDC1==(UINT64_C(1)<<53));
    assert(A733_CLOCK_RADIO_MODULE==(UINT64_C(1)<<54));
    assert(A733_CLOCK_S_TWI0==(UINT64_C(1)<<55));
    assert(a733_clock_acquire(&m.clocks,1,0,A733_CLOCK_S_TWI0,100,&twi)==A733_CLOCK_OK);
    assert(twi.shared==(A733_CLOCK_APBS1|A733_CLOCK_REF|A733_CLOCK_OSC));
    assert(a733_clock_acquire(&m.clocks,2,0,A733_CLOCK_APB_UART,100,&out)==A733_CLOCK_OK);
    assert(a733_clock_acquire(&m.clocks,2,0,A733_CLOCK_APBS1,100,&out)==A733_CLOCK_BUSY);
    assert(a733_clock_acquire(&m.clocks,3,0,A733_CLOCK_RADIO_MODULE,100,&radio)==A733_CLOCK_OK);
    assert(!radio.shared);
    assert(a733_clock_acquire(&m.clocks,4,A733_CLOCK_FABRIC|A733_CLOCK_REF,A733_CLOCK_SDC1,100,&sdc)==A733_CLOCK_OK);
    assert(a733_clock_acquire(&m.clocks,5,0,A733_CLOCK_FABRIC,100,&out)==A733_CLOCK_BUSY);
    const a733_clock_mask_t locals[]={A733_CLOCK_SDC1,A733_CLOCK_RADIO_MODULE,A733_CLOCK_S_TWI0};
    for(unsigned i=0;i<3;++i) {
        assert(a733_clock_acquire(&m.clocks,5,locals[i],0,100,&out)==A733_CLOCK_INVALID);
        assert(a733_clock_acquire(&m.clocks,5,0,locals[i],100,&out)==A733_CLOCK_BUSY);
    }
}
static void bridges_and_borrows(void)
{
    struct model m;init(&m);struct a733_clock_token old2,conflict;
    uint64_t old4;struct a733_gpu_power_resources ready;
    assert(a733_clock_acquire(&m.clocks,1,A733_POWER_DCDC2,0,100,&old2)==A733_CLOCK_OK);
    /* Actual legacy GPU reader obtains a stable DCDC4 lease and supplies its
     * own physical-readback model. New guard operations perform no such read. */
    assert(a733_gpu_resources_acquire(&m.gpu,0,A733_GPU_RESOURCE_RAIL,100,&old4,&ready)==A733_GPU_RESOURCE_OK);
    unsigned io=m.io;struct a733_rail_token t;
    assert(a733_rail_acquire(&m.rails,7,0,D2|D4,100,&t)==A733_RAIL_PENDING);
    assert(a733_rail_status(&m.rails,&t)==A733_RAIL_PENDING);
    assert(a733_rail_poll(&m.rails,&t)==A733_RAIL_PENDING);
    assert(a733_clock_release(&m.clocks,&old2,100)==A733_CLOCK_OK);
    assert(a733_rail_poll(&m.rails,&t)==A733_RAIL_PENDING);
    assert(m.rails.slots[0].dcdc2.id && !m.rails.slots[0].dcdc4.lease.token);
    assert(a733_clock_acquire(&m.clocks,1,A733_POWER_DCDC2,0,100,&conflict)==A733_CLOCK_BUSY);
    assert(a733_gpu_resources_release(&m.gpu,0,old4,100)==A733_GPU_RESOURCE_OK);
    assert(a733_rail_poll(&m.rails,&t)==A733_RAIL_READY && m.io==io);
    assert(a733_gpu_resources_acquire(&m.gpu,0,A733_GPU_RESOURCE_RAIL,100,&old4,&ready)==A733_GPU_RESOURCE_BUSY);
    assert(a733_gpu_resources_change(&m.gpu,0,A733_GPU_RESOURCE_RAIL,&old4)==A733_GPU_RESOURCE_BUSY);
    struct a733_gpu_rail_guard guard=m.rails.slots[0].dcdc4;
    assert(a733_gpu_resources_change_end(&m.gpu,guard.client,guard.lease.token,1)==A733_GPU_RESOURCE_INVALID);
    struct a733_rail_borrow b,copy;
    assert(a733_rail_borrow(&m.rails,&t,D2|D4,&b)==A733_RAIL_READY);
    copy=b;assert(a733_rail_borrow_status(&m.rails,&copy)==A733_RAIL_INVALID);
    assert(a733_rail_end_borrow(&m.rails,&copy)==A733_RAIL_INVALID);
    assert(a733_rail_release(&m.rails,&t,100)==A733_RAIL_BUSY);
    assert(a733_rail_borrow_status(&m.rails,&b)==A733_RAIL_READY);
    assert(a733_rail_borrow(&m.rails,&t,D2,&b)==A733_RAIL_INVALID);
    assert(a733_rail_end_borrow(&m.rails,&b)==A733_RAIL_READY);
    assert(a733_rail_end_borrow(&m.rails,&b)==A733_RAIL_INVALID);
    assert(a733_rail_release(&m.rails,&t,100)==A733_RAIL_READY);
    assert(a733_rail_status(&m.rails,&t)==A733_RAIL_INVALID);
    assert(a733_clock_acquire(&m.clocks,1,0,A733_POWER_DCDC2,100,&conflict)==A733_CLOCK_OK);
    assert(a733_gpu_resources_change(&m.gpu,0,A733_GPU_RESOURCE_RAIL,&old4)==A733_GPU_RESOURCE_OK);
    assert(m.io==io);
}
static void aliases_and_capacity(void)
{
    struct model m;init(&m);struct a733_rail_token t,out={.id=999},saved=out;
    const a733_rail_mask_t aliases=A733_RAIL_BIT(A733_RAIL_DCDC1)|A733_RAIL_BIT(A733_RAIL_DC1SW1)|A733_RAIL_BIT(A733_RAIL_DC1SW2);
    assert(a733_rail_acquire(&m.rails,1,A733_RAIL_BIT(A733_RAIL_DC1SW1),0,100,&t)==A733_RAIL_PENDING);
    assert(t.shared==aliases);
    assert(a733_rail_poll(&m.rails,&t)==A733_RAIL_READY);
    assert(a733_rail_acquire(&m.rails,2,0,A733_RAIL_BIT(A733_RAIL_DCDC1),100,&out)==A733_RAIL_BUSY);
    assert(!memcmp(&out,&saved,sizeof(out)));
    assert(a733_rail_acquire(&m.rails,2,UINT64_C(1)<<39,0,100,&out)==A733_RAIL_INVALID);
    struct a733_rail_token forged=t;forged.owner++;
    assert(a733_rail_release(&m.rails,&forged,100)==A733_RAIL_INVALID);
    assert(a733_rail_acquire(&m.rails,2,aliases,0,1,&out)==A733_RAIL_EXPIRED);
    assert(!memcmp(&out,&saved,sizeof(out)));
    assert(a733_rail_release(&m.rails,&t,100)==A733_RAIL_READY);
    assert(a733_rail_acquire(&m.rails,1,A733_RAIL_BIT(A733_RAIL_DCDC1),A733_RAIL_BIT(A733_RAIL_DC1SW2),100,&t)==A733_RAIL_PENDING);
    assert(!t.shared && t.exclusive==aliases);
    assert(a733_rail_release(&m.rails,&t,100)==A733_RAIL_READY);
    struct a733_rail_token tokens[A733_RAIL_CLIENTS];
    for(unsigned i=0;i<A733_RAIL_CLIENTS;++i)
        assert(a733_rail_acquire(&m.rails,i+1,A733_RAIL_BIT(A733_RAIL_BLDO5),0,100,&tokens[i])==A733_RAIL_PENDING);
    assert(a733_rail_acquire(&m.rails,200,A733_RAIL_BIT(A733_RAIL_BLDO5),0,100,&out)==A733_RAIL_BUSY);
    assert(a733_rail_poll(&m.rails,&tokens[0])==A733_RAIL_READY);
    struct a733_rail_borrow b[A733_RAIL_BORROWS],extra;
    for(unsigned i=0;i<A733_RAIL_BORROWS;++i)
        assert(a733_rail_borrow(&m.rails,&tokens[0],A733_RAIL_BIT(A733_RAIL_BLDO5),&b[i])==A733_RAIL_READY);
    assert(a733_rail_borrow(&m.rails,&tokens[0],A733_RAIL_BIT(A733_RAIL_BLDO5),&extra)==A733_RAIL_BUSY);
    assert(a733_rail_release(&m.rails,&tokens[0],100)==A733_RAIL_BUSY);
}
static void partial_cleanup(void)
{
    struct model m;init(&m);struct a733_rail_token t;struct a733_clock_token later;
    assert(a733_rail_acquire(&m.rails,1,D2|D4,0,100,&t)==A733_RAIL_PENDING);
    assert(a733_rail_poll(&m.rails,&t)==A733_RAIL_READY);
    const struct a733_gpu_rail_guard g=m.rails.slots[0].dcdc4;
    assert(g.client>=A733_GPU_RESOURCE_CLIENTS);
    struct a733_gpu_clock_parent parent={0};
    assert(!a733_gpu_clock_parent_init(&parent,&m.clocks,&m.gpu,0,7,100));
    uint64_t legacy;struct a733_gpu_power_resources physical;
    assert(a733_gpu_resources_acquire(&m.gpu,0,A733_GPU_RESOURCE_RAIL,100,&legacy,&physical)==A733_GPU_RESOURCE_OK);
    assert(a733_gpu_resources_release(&m.gpu,0,legacy,100)==A733_GPU_RESOURCE_OK);
    unsigned io=m.io;
    assert(a733_gpu_resources_release(&m.gpu,g.client,g.lease.token,100)==A733_GPU_RESOURCE_INVALID);
    m.lock_gpu_at=m.times+2; /* Common release commits before GPU lock becomes available. */
    assert(a733_rail_release(&m.rails,&t,100)==A733_RAIL_PENDING);
    assert(m.rails.slots[0].state==A733_RAIL_CLOSING && !m.rails.slots[0].dcdc2.id);
    assert(m.rails.slots[0].dcdc4.lease.token);
    atomic_store(&m.gpu.busy,0);
    assert(a733_clock_acquire(&m.clocks,2,0,A733_POWER_DCDC2,100,&later)==A733_CLOCK_OK);
    assert(a733_rail_status(&m.rails,&t)==A733_RAIL_PENDING);
    struct a733_rail_borrow b;
    assert(a733_rail_borrow(&m.rails,&t,D4,&b)==A733_RAIL_PENDING);
    assert(a733_rail_release(&m.rails,&t,100)==A733_RAIL_READY && m.io==io);
}
static void caller_mutation(void)
{
    struct model m;init(&m);struct a733_rail_token t;
    assert(a733_rail_acquire(&m.rails,1,0,D2|D4,100,&t)==A733_RAIL_PENDING);
    struct a733_rail_token admitted=t;
    m.mutate=&t;m.mutate_at=m.times+1;
    assert(a733_rail_poll(&m.rails,&t)==A733_RAIL_READY);
    assert(t.id==999 && m.rails.slots[0].dcdc2.exclusive==A733_POWER_DCDC2);
    assert(m.rails.slots[0].dcdc4.lease.mode==A733_GPU_RESOURCE_CHANGE);
    assert(a733_rail_status(&m.rails,&t)==A733_RAIL_INVALID);
    assert(a733_rail_status(&m.rails,&admitted)==A733_RAIL_READY);
}
static void terminal_faults(void)
{
    struct model m;init(&m);struct a733_rail_token t;
    assert(a733_rail_acquire(&m.rails,1,D2|D4,0,100,&t)==A733_RAIL_PENDING);
    m.late_at=m.times+3; /* Opening sample, common acquire, final publication sample. */
    assert(a733_rail_poll(&m.rails,&t)==A733_RAIL_RETAINED);
    assert(m.rails.slots[0].dcdc2.id && m.rails.slots[0].dcdc4.lease.token);
    unsigned calls=m.times;
    assert(a733_rail_poll(&m.rails,&t)==A733_RAIL_RETAINED);
    assert(a733_rail_release(&m.rails,&t,2000)==A733_RAIL_RETAINED && m.times==calls && !m.io);
    init(&m);
    assert(a733_rail_acquire(&m.rails,1,D2|D4,0,100,&t)==A733_RAIL_PENDING);
    assert(a733_rail_poll(&m.rails,&t)==A733_RAIL_READY);
    struct a733_rail_borrow b;
    assert(a733_rail_borrow(&m.rails,&t,D2,&b)==A733_RAIL_READY);
    struct a733_rail_leases copied=m.rails;
    assert(a733_rail_status(&copied,&t)==A733_RAIL_INVALID);
    /* Actual common fault propagates; all lease/borrow ownership survives. */
    assert(a733_clock_retain(&m.clocks,&m.rails.slots[0].dcdc2)==A733_CLOCK_FAULT);
    assert(a733_rail_borrow_status(&m.rails,&b)==A733_RAIL_RETAINED);
    assert(a733_rail_end_borrow(&m.rails,&b)==A733_RAIL_RETAINED);
    assert(m.rails.slots[0].borrows==1);
    init(&m);
    assert(a733_rail_acquire(&m.rails,1,D2|D4,0,100,&t)==A733_RAIL_PENDING);
    assert(a733_rail_poll(&m.rails,&t)==A733_RAIL_READY);
    m.late_at=m.times+3;
    assert(a733_rail_release(&m.rails,&t,100)==A733_RAIL_RETAINED);
    assert(!m.rails.slots[0].dcdc2.id && m.rails.slots[0].dcdc4.lease.token);
    calls=m.times;
    assert(a733_rail_release(&m.rails,&t,2000)==A733_RAIL_RETAINED && calls==m.times);
    init(&m);
    assert(a733_rail_acquire(&m.rails,1,D4,0,100,&t)==A733_RAIL_PENDING);
    assert(a733_rail_poll(&m.rails,&t)==A733_RAIL_READY);
    m.rails.binding.bridge_owner++;
    assert(a733_rail_poll(&m.rails,&t)==A733_RAIL_RETAINED);
    assert(m.rails.slots[0].dcdc4.lease.token && !m.io);
}
int main(void)
{
    scopes();bridges_and_borrows();aliases_and_capacity();partial_cleanup();caller_mutation();terminal_faults();
    puts("PASS: real DCDC2/DCDC4 guard conflicts, rail aliases, pinned snapshots, partial cleanup and fault retention");
}
