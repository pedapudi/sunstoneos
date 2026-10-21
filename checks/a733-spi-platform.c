#include "a733_spi_platform.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct cell {uintptr_t a;uint32_t v;};
struct fixture {
    struct a733_spi_platform platform;
    struct a733_clock_leases clocks;
    struct a733_pins pins;
    struct cell cells[160];unsigned count;
    uint64_t time,hold_start;
    unsigned locked,poisoned,authorized,initial_pin_writes,transactions,received,rn;
    unsigned deny_read,late_reap,late_seen,osc_reads;
    uint64_t trace;
    uintptr_t fail_write;
};
static uint32_t *cell(struct fixture *f,uintptr_t a)
{
    for(unsigned i=0;i<f->count;i++)if(f->cells[i].a==a)return &f->cells[i].v;
    assert(f->count<160);f->cells[f->count]=(struct cell){a,0};return &f->cells[f->count++].v;
}
static int now(void *p,uint64_t *t)
{
    struct fixture *f=p;
    if(f->late_reap&&f->platform.spi.state==A733_SPI_READY&&!f->platform.spi.transfer.rx){f->time=100000;f->late_seen++;}
    *t=++f->time;return 1;
}
static int current(void *p){(void)p;return 1;}
static int enter(void *p,uint64_t deadline)
{
    struct fixture *f=p;assert(!f->locked);if(f->poisoned||f->time>=deadline)return 0;
    f->locked=1;return 1;
}
static void leave(void *p){struct fixture *f=p;assert(f->locked&&!f->poisoned);f->locked=0;}
static void retain(void *p){struct fixture *f=p;assert(f->locked);f->poisoned=1;}
static int authorize(void *p,const struct a733_spi_platform_config *c,
    const struct a733_clock_token *token,enum a733_spi_platform_admission phase)
{
    struct fixture *f=p;assert(c->owner==8&&c->handoff==71);
    assert(a733_clock_token_status(&f->clocks,token)==A733_CLOCK_OK);
    assert(token->exclusive==(A733_CLOCK_SPI0<<c->format.port));
    assert(phase==A733_SPI_PLATFORM_START||phase==A733_SPI_PLATFORM_STOP);
    return f->authorized?1:0;
}
static int rd(void *p,uintptr_t a,uint32_t *v)
{
    struct fixture *f=p;assert(f->locked);
    if(f->deny_read)return 0;
    if(a==0x07090160)f->osc_reads++;
    if(a==f->platform.spi.plan.base+4&&(*cell(f,a)&0x80000000)) *cell(f,a)=0x80;
    if(a==f->platform.spi.plan.base+0x18)*cell(f,a)&=~0x80008000u;
    if(a==f->platform.spi.plan.base+0x1c)*v=f->rn;else *v=*cell(f,a);
    return 1;
}
static int wr(void *p,uintptr_t a,uint32_t v)
{
    struct fixture *f=p;assert(f->locked);struct a733_spi_platform *s=&f->platform;
    f->trace=(f->trace^a^((uint64_t)v<<32))*UINT64_C(0x100000001b3);
    uintptr_t base=s->spi.plan.base,bank=0x03400000u+4*(s->spi.plan.intid/32);
    if(a==bank+0x180){*cell(f,bank+0x100)&=~v;return 1;}
    if(a==bank+0x280){*cell(f,bank+0x200)&=~v;return 1;}
    if(a==base+0x14){*cell(f,a)&=~v;return 1;}
    if(a==s->spi.plan.reset) {
        if((v&0x10001)==1)f->hold_start=f->time;
        if((v&0x10001)==0x10001) {
            assert(f->time-f->hold_start>=10);
            *cell(f,base+4)=0x80;*cell(f,base+8)=0x87;
        }
    }
    for(unsigned i=0;i<4;i++) {
        struct a733_pin_fields fields;assert(a733_pin_fields_banked(s->route.bank,s->requests[i].pin,&fields));
        if(a==fields.mux_address&&((v>>fields.mux_shift)&15)==s->route.function) {
            assert(*cell(f,base+4)==0x83&&!(s->spi.tcr&0x80000000));
            assert(!!(*cell(f,base+8)&0x80)!=s->config.format.active_high);
            assert(s->spi.state==A733_SPI_READY);f->initial_pin_writes++;
        }
    }
    *cell(f,a)=v;
    if(a==base+8&&(v&0x80000000)) {
        assert(s->pin_lease.id&&!s->pins_released&&*cell(f,base+0x34)==0);
        f->transactions++;f->rn=*cell(f,base+0x30);f->received=0;
        *cell(f,base+8)&=~0x80000000u;*cell(f,base+0x14)|=0x1000;
    }
    return a!=f->fail_write;
}
static int read8(void *p,uintptr_t a,uint8_t *v)
{
    struct fixture *f=p;assert(f->locked&&a==f->platform.spi.plan.base+0x300&&f->rn);
    *v=(uint8_t)++f->received;f->rn--;return 1;
}
static int write8(void *p,uintptr_t a,uint8_t v){(void)p;(void)a;(void)v;assert(0);return 0;}
static void init(struct fixture *f,enum a733_spi_route route)
{
    memset(f,0,sizeof(*f));f->authorized=1;
    struct a733_spi_route_plan r;assert(a733_spi_route_plan(route,0,&r));
    struct a733_pin_handoff h={0};h.authorized[r.bank]=a733_pin_bank_mask(r.bank);
    struct a733_pins_operations pins={enter,leave,now,rd,wr};
    assert(a733_pins_init_banks(&f->pins,&pins,f,&h));
    assert(a733_clock_leases_init(&f->clocks,now,f));
    struct a733_spi_platform_config c={.owner=8,.handoff=71,.deadline=100000,.route=route,
        .format={r.port,0,0,0,route&1,500000,2,2}};
    for(unsigned i=0;i<4;i++){c.expected[i]=(struct a733_pin_config){15,0,0};c.desired[i]=(struct a733_pin_config){r.function,0,0};}
    struct a733_spi_platform_ops ops={now,current,authorize,enter,leave,retain,rd,wr,read8,write8};
    assert(a733_spi_platform_init(&f->platform,&c,&ops,f,&f->clocks,&f->pins));
    for(unsigned i=0;i<4;i++) {
        struct a733_pin_fields fields;assert(a733_pin_fields_banked(r.bank,f->platform.requests[i].pin,&fields));
        *cell(f,fields.mux_address)|=15u<<fields.mux_shift;
    }
    *cell(f,0x07090160)=0x10000002;*cell(f,0x02002000)=0xf8636300;
    *cell(f,f->platform.spi.plan.clock)=0x07000302;
    *cell(f,f->platform.spi.plan.reset)=0x20000000;
}
static enum a733_spi_platform_result advance(struct fixture *f,enum a733_spi_platform_result goal)
{
    enum a733_spi_platform_result r=A733_SPI_PLATFORM_PENDING;
    for(unsigned i=0;i<100&&r!=goal&&r!=A733_SPI_PLATFORM_RETAINED;i++)r=a733_spi_platform_poll(&f->platform,8);
    return r;
}
static void working(void)
{
    for(unsigned route=0;route<6;route++) {
        struct fixture f;init(&f,route);
        assert(advance(&f,A733_SPI_PLATFORM_READY)==A733_SPI_PLATFORM_READY);
        assert(f.platform.pin_lease.id&&f.platform.clock.id&&f.initial_pin_writes&&!f.transactions);
        assert(f.platform.clock.exclusive>>32); /* Real high-bit controller scope. */
        if(route==A733_SPI3_PE) {
            uint8_t rx[4]={0};struct a733_spi_transfer t={A733_SPI_READ,0,rx,0,4};
            assert(a733_spi_platform_submit(&f.platform,&t,100000)==A733_SPI_PLATFORM_PENDING);
            /* Each transfer poll revalidates the parent once, inside admission. */
            enum a733_spi_platform_result r=A733_SPI_PLATFORM_PENDING;
            for(unsigned i=0;i<100&&r==A733_SPI_PLATFORM_PENDING;i++) {
                f.osc_reads=0;r=a733_spi_platform_poll(&f.platform,8);assert(f.osc_reads==1);
            }
            assert(r==A733_SPI_PLATFORM_COMPLETE);
            assert(f.transactions==1&&rx[0]==1&&rx[3]==4);
            f.late_reap=1;
            assert(a733_spi_platform_reap(&f.platform,100000)==A733_SPI_PLATFORM_READY);
            assert(!f.late_seen&&!f.platform.spi.transfer.rx&&!f.platform.fault);
            f.late_reap=0;
        }
        /* Completed-operation deadlines do not expire idle cached status. */
        uint64_t saved=f.time;f.time=100001;
        assert(a733_spi_platform_poll(&f.platform,8)==A733_SPI_PLATFORM_READY);
        f.time=saved;
        assert(a733_spi_platform_close(&f.platform,100000)==A733_SPI_PLATFORM_PENDING);
        assert(advance(&f,A733_SPI_PLATFORM_CLOSED)==A733_SPI_PLATFORM_CLOSED);
        if(getenv("SPI_TRACE"))printf("route %u write trace %016llx\n",route,(unsigned long long)f.trace);
        assert(!f.platform.clock.id&&!f.poisoned&&!f.locked);
        assert(*cell(&f,f.platform.spi.plan.clock)==0x07000302);
        assert(*cell(&f,f.platform.spi.plan.reset)==0x20000000);
        for(unsigned i=0;i<4;i++) {
            struct a733_pin_fields fields;assert(a733_pin_fields_banked(f.platform.route.bank,f.platform.requests[i].pin,&fields));
            assert((*cell(&f,fields.mux_address)>>(fields.mux_shift)&15)==15);
        }
    }
}
static void ownership(void)
{
    struct fixture f;init(&f,A733_SPI3_PE);
    struct a733_clock_token other={0};
    assert(a733_clock_acquire(&f.clocks,19,A733_CLOCK_OSC,A733_CLOCK_SPI3,100000,&other)==A733_CLOCK_OK);
    assert(a733_spi_platform_poll(&f.platform,8)==A733_SPI_PLATFORM_BUSY&&!f.platform.clock.id);
    assert(a733_clock_release(&f.clocks,&other,100000)==A733_CLOCK_OK);
    f.authorized=0;
    assert(a733_spi_platform_poll(&f.platform,8)==A733_SPI_PLATFORM_PENDING);
    assert(a733_spi_platform_poll(&f.platform,8)==A733_SPI_PLATFORM_PENDING);
    assert(*cell(&f,f.platform.spi.plan.clock)==0x07000302&&!f.platform.pin_lease.id);
    f.authorized=1;assert(advance(&f,A733_SPI_PLATFORM_READY)==A733_SPI_PLATFORM_READY);
    uint64_t pin_id=f.platform.pin_lease.id,clock_id=f.platform.clock.id;
    f.pins.failed=1; /* Independent pin operation has retained uncertain state. */
    assert(a733_spi_platform_close(&f.platform,100000)==A733_SPI_PLATFORM_RETAINED);
    assert(f.platform.pin_lease.id==pin_id&&f.platform.clock.id==clock_id&&f.poisoned);
}
static void pin_conflict(void)
{
    struct fixture f;init(&f,A733_SPI3_PE);
    struct a733_pin_request_banked r=f.platform.requests[0];r.desired.function=0;
    struct a733_pin_lease_banked other={0};
    assert(a733_pins_acquire_banked(&f.pins,19,&r,1,&other,100000)==A733_PINS_OK);
    enum a733_spi_platform_result result=A733_SPI_PLATFORM_PENDING;
    for(unsigned i=0;i<40&&result!=A733_SPI_PLATFORM_BUSY;i++)result=a733_spi_platform_poll(&f.platform,8);
    assert(result==A733_SPI_PLATFORM_BUSY&&f.platform.spi.state==A733_SPI_READY&&!f.platform.pin_lease.id);
    assert(a733_pins_release_banked(&f.pins,&other,1,100000)==A733_PINS_OK);
    assert(advance(&f,A733_SPI_PLATFORM_READY)==A733_SPI_PLATFORM_READY);
}
static void bad_cs_readback(void)
{
    struct fixture f;init(&f,A733_SPI3_PE);
    for(unsigned i=0;i<30&&f.platform.spi.state!=A733_SPI_READY;i++)
        assert(a733_spi_platform_poll(&f.platform,8)==A733_SPI_PLATFORM_PENDING);
    assert(f.platform.spi.state==A733_SPI_READY&&!f.platform.pin_lease.id);
    *cell(&f,f.platform.spi.plan.base+8)^=0x40; /* Hardware CS owner differs. */
    assert(a733_spi_platform_poll(&f.platform,8)==A733_SPI_PLATFORM_RETAINED);
    assert(!f.platform.pin_lease.id&&!f.initial_pin_writes&&f.poisoned);
}
static void uncertain(void)
{
    struct fixture f;init(&f,A733_SPI3_PE);f.fail_write=f.platform.spi.plan.clock;
    assert(advance(&f,A733_SPI_PLATFORM_RETAINED)==A733_SPI_PLATFORM_RETAINED);
    assert(f.platform.clock.id&&!f.platform.pin_lease.id&&f.poisoned&&f.locked&&f.clocks.failed);
    uint64_t before=f.time;assert(a733_spi_platform_poll(&f.platform,8)==A733_SPI_PLATFORM_RETAINED);
    assert(f.time==before);
    struct a733_spi_route_plan route;
    assert(!a733_spi_route_plan((enum a733_spi_route)6,0,&route));
    assert(!a733_spi_route_plan(A733_SPI3_PE,2,&route));
}
int main(void){working();ownership();pin_conflict();bad_cs_readback();uncertain();puts("SPI real clock/pin/core composition, retained faults and restoration passed");return 0;}
