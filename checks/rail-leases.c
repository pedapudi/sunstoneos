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
    unsigned times, io;
};
static int now(void *p,uint64_t *out)
{
    struct model *m=p;++m->times;
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
static const enum a733_rail accepted[]={A733_RAIL_DCDC1,A733_RAIL_DCDC2,A733_RAIL_DCDC3,A733_RAIL_DCDC4,
    A733_RAIL_DCDC5,A733_RAIL_DCDC8,A733_RAIL_DCDC9,A733_RAIL_BLDO1,A733_RAIL_BLDO2,A733_RAIL_BLDO5,
    A733_RAIL_CLDO1,A733_RAIL_CLDO2,A733_RAIL_CLDO3,A733_RAIL_CLDO5,A733_RAIL_ELDO6};
static unsigned registry_holders(const struct model *m)
{unsigned n=0;for(unsigned i=0;i<A733_CLOCK_CLIENTS;++i)n+=m->clocks.slots[i].id!=0;return n;}
/* Acquire, poll and release in production order, for every accepted output:
 * the first poll is READY, and no registry or GPU lease and no bus access is
 * taken. Every other output is refused. */
static void fifteen_rails(void)
{
    struct model m;init(&m);
    assert(sizeof(m.rails)<=1632 && m.clocks.authority==&m.rails.authority);
    for(unsigned i=0;i<sizeof(accepted)/sizeof(*accepted);++i)for(unsigned mode=0;mode<2;++mode) {
        struct a733_rail_token t;const a733_rail_mask_t bit=A733_RAIL_BIT(accepted[i]);
        assert(a733_rail_acquire(&m.rails,1,mode ? 0 : bit,mode ? bit : 0,100,&t)==A733_RAIL_PENDING);
        assert(a733_rail_poll(&m.rails,&t)==A733_RAIL_READY && a733_rail_status(&m.rails,&t)==A733_RAIL_READY);
        assert(!registry_holders(&m) && !m.gpu.rail_guards && !m.io);
        assert(a733_rail_release(&m.rails,&t,100)==A733_RAIL_READY);
        assert(a733_rail_status(&m.rails,&t)==A733_RAIL_INVALID);
    }
    struct a733_rail_token out={.id=999},saved=out;
    for(unsigned rail=0;rail<A733_RAIL_COUNT;++rail)if(!(A733_RAIL_ACCEPTED&A733_RAIL_BIT(rail))) {
        assert(a733_rail_acquire(&m.rails,1,A733_RAIL_BIT(rail),0,100,&out)==A733_RAIL_INVALID);
        assert(a733_rail_acquire(&m.rails,1,0,A733_RAIL_BIT(rail),100,&out)==A733_RAIL_INVALID);
    }
    assert(!memcmp(&out,&saved,sizeof(out)) && m.rails.free_slots==UINT32_MAX && !m.rails.shared_held);
}
/* Exclusive excludes every other holder of an output and shared excludes an
 * exclusive holder. The ledger no longer bridges to the registry's DCDC2 scope
 * or the GPU manager's DCDC4 guard: neither is a rail authority. */
static void exclusion(void)
{
    struct model m;init(&m);struct a733_rail_token cpu,other,shared,out;
    const a733_rail_mask_t d3=A733_RAIL_BIT(A733_RAIL_DCDC3);
    assert(a733_rail_acquire(&m.rails,1,0,d3,100,&cpu)==A733_RAIL_PENDING);
    assert(a733_rail_acquire(&m.rails,2,0,d3,100,&out)==A733_RAIL_BUSY);
    assert(a733_rail_acquire(&m.rails,2,d3,0,100,&out)==A733_RAIL_BUSY);
    assert(a733_rail_release(&m.rails,&cpu,100)==A733_RAIL_READY);
    assert(a733_rail_acquire(&m.rails,2,d3|D2,0,100,&shared)==A733_RAIL_PENDING);
    assert(a733_rail_acquire(&m.rails,3,d3,0,100,&other)==A733_RAIL_PENDING);
    assert(a733_rail_acquire(&m.rails,1,0,d3,100,&out)==A733_RAIL_BUSY);
    assert(a733_rail_release(&m.rails,&shared,100)==A733_RAIL_READY);
    assert(a733_rail_acquire(&m.rails,1,0,d3,100,&out)==A733_RAIL_BUSY);
    assert(a733_rail_release(&m.rails,&other,100)==A733_RAIL_READY);
    assert(a733_rail_acquire(&m.rails,1,0,d3,100,&cpu)==A733_RAIL_PENDING);
    assert(m.rails.exclusive_held==d3 && !m.rails.shared_held);
    struct a733_clock_token legacy;uint64_t gpu;struct a733_gpu_power_resources ready;
    assert(a733_rail_acquire(&m.rails,4,0,D2|D4,100,&out)==A733_RAIL_PENDING);
    assert(a733_clock_acquire(&m.clocks,5,A733_POWER_DCDC2,0,100,&legacy)==A733_CLOCK_OK);
    assert(a733_gpu_resources_acquire(&m.gpu,0,A733_GPU_RESOURCE_RAIL,100,&gpu,&ready)==A733_GPU_RESOURCE_OK);
    assert(a733_rail_status(&m.rails,&out)==A733_RAIL_READY);
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
    /* A full table returns BUSY and leaves the output unchanged. */
    struct a733_rail_token tokens[A733_RAIL_CLIENTS];
    for(unsigned i=0;i<A733_RAIL_CLIENTS;++i) {
        assert(a733_rail_acquire(&m.rails,i+1,A733_RAIL_BIT(A733_RAIL_BLDO5),0,100,&tokens[i])==A733_RAIL_PENDING);
        assert((tokens[i].id&(A733_RAIL_CLIENTS-1))==i);
    }
    assert(a733_rail_acquire(&m.rails,200,A733_RAIL_BIT(A733_RAIL_BLDO5),0,100,&out)==A733_RAIL_BUSY);
    assert(!memcmp(&out,&saved,sizeof(out)));
    assert(a733_rail_poll(&m.rails,&tokens[0])==A733_RAIL_READY);
    struct a733_rail_borrow b[A733_RAIL_BORROWS],extra,copy;
    for(unsigned i=0;i<A733_RAIL_BORROWS;++i)
        assert(a733_rail_borrow(&m.rails,&tokens[0],A733_RAIL_BIT(A733_RAIL_BLDO5),&b[i])==A733_RAIL_READY);
    assert(a733_rail_borrow(&m.rails,&tokens[0],A733_RAIL_BIT(A733_RAIL_BLDO5),&extra)==A733_RAIL_BUSY);
    assert(a733_rail_borrow(&m.rails,&tokens[1],A733_RAIL_BIT(A733_RAIL_BLDO5),&b[3])==A733_RAIL_INVALID);
    assert(a733_rail_borrow(&m.rails,&tokens[1],D2,&extra)==A733_RAIL_INVALID);
    assert(a733_rail_release(&m.rails,&tokens[0],100)==A733_RAIL_BUSY);
    copy=b[0];assert(a733_rail_borrow_status(&m.rails,&copy)==A733_RAIL_INVALID);
    assert(a733_rail_end_borrow(&m.rails,&copy)==A733_RAIL_INVALID);
    for(unsigned i=0;i<A733_RAIL_BORROWS;++i) {
        assert(a733_rail_borrow_status(&m.rails,&b[i])==A733_RAIL_READY);
        assert(a733_rail_end_borrow(&m.rails,&b[i])==A733_RAIL_READY);
        assert(a733_rail_end_borrow(&m.rails,&b[i])==A733_RAIL_INVALID);
    }
    /* Identifiers are never reused: the freed slot gets a new generation. */
    const struct a733_rail_token stale=tokens[0];
    assert(a733_rail_release(&m.rails,&tokens[0],100)==A733_RAIL_READY);
    assert(a733_rail_acquire(&m.rails,1,A733_RAIL_BIT(A733_RAIL_BLDO5),0,100,&tokens[0])==A733_RAIL_PENDING);
    assert((tokens[0].id&(A733_RAIL_CLIENTS-1))==0 && tokens[0].id!=stale.id);
    assert(a733_rail_status(&m.rails,&stale)==A733_RAIL_INVALID);
    for(unsigned i=0;i<A733_RAIL_CLIENTS;++i)assert(a733_rail_release(&m.rails,&tokens[i],100)==A733_RAIL_READY);
    assert(m.rails.free_slots==UINT32_MAX && !m.rails.shared_held && !m.rails.sharers[A733_RAIL_BLDO5]);
}
/* A counted domain row pins its token like a borrow. */
static void pins(void)
{
    struct model m;init(&m);struct a733_rail_token t;
    assert(a733_rail_acquire(&m.rails,1,D2|A733_RAIL_BIT(A733_RAIL_CLDO2),0,100,&t)==A733_RAIL_PENDING);
    assert(a733_rail_pin(&m.rails,&t,D4)==A733_RAIL_INVALID);
    assert(a733_rail_pin(&m.rails,&t,D2)==A733_RAIL_READY);
    assert(a733_rail_pin(&m.rails,&t,D2|A733_RAIL_BIT(A733_RAIL_CLDO2))==A733_RAIL_READY);
    assert(a733_rail_release(&m.rails,&t,100)==A733_RAIL_BUSY);
    assert(a733_rail_unpin(&m.rails,&t)==A733_RAIL_READY && a733_rail_unpin(&m.rails,&t)==A733_RAIL_READY);
    assert(a733_rail_unpin(&m.rails,&t)==A733_RAIL_INVALID);
    assert(a733_rail_release(&m.rails,&t,100)==A733_RAIL_READY);
}
static void terminal_faults(void)
{
    struct model m;init(&m);struct a733_rail_token t,u;
    /* A copied ledger cannot authenticate anything. */
    assert(a733_rail_acquire(&m.rails,1,D2|D4,0,100,&t)==A733_RAIL_PENDING);
    struct a733_rail_borrow b;
    assert(a733_rail_borrow(&m.rails,&t,D2,&b)==A733_RAIL_READY);
    static struct a733_rail_leases copied;copied=m.rails;
    assert(a733_rail_status(&copied,&t)==A733_RAIL_INVALID);
    /* A registry fault is reported in the state word; rail tokens stay valid
     * because the ledger takes no registry lease. */
    struct a733_clock_token c;
    assert(a733_clock_acquire(&m.clocks,1,A733_CLOCK_OSC,0,100,&c)==A733_CLOCK_OK);
    assert(a733_clock_retain(&m.clocks,&c)==A733_CLOCK_FAULT);
    assert(atomic_load(&m.rails.authority.word)==A733_POWER_CLOCK_FAULT);
    assert(a733_rail_status(&m.rails,&t)==A733_RAIL_READY);
    /* A ledger fault keeps every reservation and borrow. */
    assert(a733_rail_retain(&m.rails,&t)==A733_RAIL_RETAINED);
    assert(atomic_load(&m.rails.authority.word)&A733_POWER_LEDGER_FAULT);
    assert(a733_rail_status(&m.rails,&t)==A733_RAIL_RETAINED);
    assert(a733_rail_borrow_status(&m.rails,&b)==A733_RAIL_RETAINED);
    assert(a733_rail_end_borrow(&m.rails,&b)==A733_RAIL_RETAINED && m.rails.pins[t.id&31]==1);
    assert(a733_rail_release(&m.rails,&t,100)==A733_RAIL_RETAINED);
    assert(a733_rail_acquire(&m.rails,2,A733_RAIL_BIT(A733_RAIL_BLDO1),0,100,&u)==A733_RAIL_RETAINED);
    /* A clock that runs backwards faults the ledger. */
    init(&m);
    assert(a733_rail_acquire(&m.rails,1,D4,0,100,&t)==A733_RAIL_PENDING);
    m.time=0;
    assert(a733_rail_release(&m.rails,&t,100)==A733_RAIL_RETAINED);
    assert(a733_rail_status(&m.rails,&t)==A733_RAIL_RETAINED && !m.io);
    /* A replaced time source faults the ledger before any change. */
    init(&m);
    assert(a733_rail_acquire(&m.rails,1,D4,0,100,&t)==A733_RAIL_PENDING);
    m.rails.binding.context=&m.time;
    assert(a733_rail_release(&m.rails,&t,100)==A733_RAIL_RETAINED);
    /* One registry reports to one ledger. */
    init(&m);
    static struct a733_rail_leases second;second=(struct a733_rail_leases){0};
    const struct a733_rail_binding again={&m.clocks,&m.gpu,99,now,&m};
    assert(!a733_rail_leases_init(&second,&again));
    /* BUSY refuses without a change. */
    atomic_store(&m.rails.busy,1);
    assert(a733_rail_acquire(&m.rails,1,D4,0,100,&t)==A733_RAIL_BUSY);
    atomic_store(&m.rails.busy,0);
}
int main(void)
{
    scopes();fifteen_rails();exclusion();aliases_and_capacity();pins();terminal_faults();
    puts("PASS: fifteen rails READY on the first poll without bus access, exclusion, aliases, "
        "capacity, never-reused identifiers, pins and fault retention");
}
