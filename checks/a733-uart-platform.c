#include "a733_uart_platform_native.h"
#include "a733_pins_native.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define CCU UINT32_C(0x02002000)
#define RCCU UINT32_C(0x07010000)
#define GIC UINT32_C(0x03400000)
#define BIT(n) (UINT32_C(1)<<(n))
static struct a733_clock_leases clocks;
static struct a733_pins pins;
static uint32_t ccu[0x2000/4],rccu[0x340/4],gic[0x10000/4],pio[0x600/4],rpio[0x410/4];
static uint64_t now_us;
static uint32_t oscillator;
static unsigned deferred_irq;
static unsigned held,lock_retained,reads,writes;
struct fixture {
    struct a733_uart_platform binding;
    uint32_t uart[0xa8/4],ier,dll,dlh;
    unsigned port,admitted,correct_task,admission_unknown,admission_reentry;
    unsigned gic_wait,halt_stuck,shift_busy,rx_count;
    uint8_t rx,tx;
    unsigned sent,fail_write,expire_write,fail_read,cause,line_error;
};
static struct fixture first,second;
static uint32_t *reg(uintptr_t a)
{
    if (a>=CCU && a<CCU+sizeof(ccu)) return &ccu[(a-CCU)/4];
    if (a>=RCCU && a<RCCU+sizeof(rccu)) return &rccu[(a-RCCU)/4];
    if (a>=GIC && a<GIC+sizeof(gic)) return &gic[(a-GIC)/4];
    if (a>=A733_PINS_BASE && a<A733_PINS_BASE+sizeof(pio)) return &pio[(a-A733_PINS_BASE)/4];
    assert(a>=A733_PINS_R_BASE && a<A733_PINS_R_BASE+sizeof(rpio));
    return &rpio[(a-A733_PINS_R_BASE)/4];
}
static int time_read(void *cookie,uint64_t *out)
{ (void)cookie; *out=now_us; return 1; }
static int lock_enter(void *cookie,uint64_t deadline)
{ (void)cookie; if (held || lock_retained || now_us>=deadline) return 0; held=1; return 1; }
static void lock_leave(void *cookie)
{ (void)cookie; assert(held && !lock_retained); held=0; }
static void lock_keep(void *cookie)
{ (void)cookie; assert(held);lock_retained=1; }
static int pin_read(void *cookie,uintptr_t a,uint32_t *v)
{ assert(cookie==&pins && held && a733_pins_native_address(a,0)); ++reads;*v=*reg(a);return 1; }
static int pin_write(void *cookie,uintptr_t a,uint32_t v)
{ assert(cookie==&pins && held && a733_pins_native_address(a,1));++writes;*reg(a)=v;return 1; }
static uintptr_t uart_base(unsigned port)
{ return port<7 ? UINT32_C(0x02500000)+port*0x1000 : UINT32_C(0x07080000)+(port-7)*0x1000; }
static uintptr_t bus(unsigned port)
{ return port<7 ? CCU+0xe00+port*4 : RCCU+0x18c; }
static uint32_t local_mask(unsigned port)
{ unsigned g=port<7 ? 1 : BIT(port-7);return g|(g<<16); }
static unsigned intid(unsigned port)
{ return port<7 ? 34+port : 233+port-7; }
static uintptr_t irq_reg(unsigned port,unsigned offset)
{ return GIC+offset+(intid(port)/32)*4; }
static int current(void *cookie) { return ((struct fixture *)cookie)->correct_task; }
static int authorize(void *cookie,const struct a733_uart_platform_config *c,
    const struct a733_clock_token *t,enum a733_uart_admission phase)
{
    struct fixture *f=cookie;
    assert(c==&f->binding.config && t==&f->binding.clock && t->id && !held);
    assert(phase==A733_UART_ADMIT_START || phase==A733_UART_ADMIT_STOP);
    assert(a733_clock_token_status(&clocks,t)==A733_CLOCK_OK);
    if (f->admission_reentry) assert(a733_uart_platform_poll(&f->binding)==A733_UART_PLATFORM_BUSY);
    if (f->admission_unknown) return -1;
    return f->admitted;
}
static int read32(void *cookie,uintptr_t a,uint32_t *v)
{
    struct fixture *f=cookie; uintptr_t base=uart_base(f->port);
    assert(f->correct_task && a733_uart_platform_native_address(f->port,a,0));
    if (++reads==f->fail_read) return 0;
    if (a>=base && a<=base+0xa4) {
        assert(f->binding.clock.id && f->binding.pin_lease.id && (*reg(bus(f->port))&local_mask(f->port))==local_mask(f->port));
        unsigned off=a-base;
        if (off==0 && !(f->uart[3]&0x80)) {assert(f->rx_count);*v=f->rx;--f->rx_count;}
        else if (off==0x14) *v=(f->shift_busy ? 0x20 : 0x60)|(f->rx_count ? 1 : 0)|f->line_error;
        else if (off==0x7c) *v=6|(f->shift_busy ? 1 : 0)|(f->rx_count ? 8 : 0);
        else if (off==0xa4 && !f->halt_stuck) {*v=f->uart[off/4]&~4u;f->uart[off/4]=*v;}
        else if (off==8) *v=f->cause;
        else *v=f->uart[off/4];
        return 1;
    }
    assert(held && !lock_retained);
    if (a==UINT32_C(0x07090160)) {*v=oscillator;return 1;}
    if (a==GIC && f->gic_wait) {--f->gic_wait;*v=BIT(31);return 1;}
    *v=*reg(a);return 1;
}
static int write32(void *cookie,uintptr_t a,uint32_t v)
{
    struct fixture *f=cookie;uintptr_t base=uart_base(f->port);
    assert(f->correct_task && a733_uart_platform_native_address(f->port,a,1));
    ++writes;
    if (a>=base && a<=base+0xa4) {
        assert(f->binding.clock.id && f->binding.pin_lease.id && (*reg(bus(f->port))&local_mask(f->port))==local_mask(f->port));
        assert(!(*reg(irq_reg(f->port,0x100))&BIT(intid(f->port)%32)));
        unsigned off=a-base;
        if (off==0) {
            if (f->uart[3]&0x80) f->dll=v;
            else {f->tx=v;++f->sent;}
        } else if (off==4) {
            if (f->uart[3]&0x80) f->dlh=v;else f->ier=v;
        } else if (off!=8) f->uart[off/4]=v;
    } else {
        assert(held && !lock_retained && f->binding.clock.id);
        if (a==bus(f->port)) {
            uint32_t mask=local_mask(f->port),old=*reg(a);
            assert(!((old^v)&~mask));
            assert(f->binding.pin_lease.id);
            *reg(a)=v;
            if (!(v&(mask&0xffff0000u))) memset(f->uart,0,sizeof(f->uart));
        } else {
            uint32_t bit=BIT(intid(f->port)%32);
            assert(v==bit);
            if (a==irq_reg(f->port,0x180)) *reg(irq_reg(f->port,0x100))&=~bit;
            else {assert(a==irq_reg(f->port,0x280));*reg(irq_reg(f->port,0x200))&=~bit;}
        }
    }
    if (writes==f->expire_write) now_us=f->binding.deadline;
    return writes!=f->fail_write;
}
static void initialize_fixture(struct fixture *f,enum a733_uart_route route,unsigned flow,uint64_t owner)
{
    memset(f,0,sizeof(*f));f->admitted=f->correct_task=1;f->cause=1;
    struct a733_uart_route_plan p; assert(a733_uart_route_plan(route,&p));f->port=p.port;
    struct a733_uart_platform_config c={.owner=owner,.handoff=owner+100,.startup_deadline_us=1000,
        .route=route,.deferred_irq=deferred_irq,.format={p.port,8,2,0,flow,24000000,1500000,0}};
    for (unsigned i=0;i<4;++i) {c.expected[i]=(struct a733_pin_config){15,0,0};c.desired[i]=(struct a733_pin_config){p.function,0,1};}
    struct a733_uart_platform_ops ops={time_read,current,authorize,lock_enter,lock_leave,lock_keep,read32,write32};
    assert(a733_uart_platform_init(&f->binding,&c,&ops,f,&clocks,&pins));
}
static void setup(void)
{
    held=lock_retained=reads=writes=0;now_us=0;deferred_irq=0;oscillator=BIT(28)|BIT(1);
    memset(&clocks,0,sizeof(clocks));memset(&pins,0,sizeof(pins));
    memset(ccu,0,sizeof(ccu));memset(rccu,0,sizeof(rccu));memset(gic,0,sizeof(gic));
    memset(pio,0,sizeof(pio));memset(rpio,0,sizeof(rpio));
    ccu[0]=UINT32_C(0xf8000000)|(99u<<8)|(99u<<16);
    assert(a733_clock_leases_init(&clocks,time_read,0));
    struct a733_pin_handoff handoff={0};
    for (unsigned b=1;b<A733_PIN_BANKS;++b) {
        handoff.authorized[b]=a733_pin_bank_mask(b);
        for (unsigned p=0;p<32;++p) if (handoff.authorized[b]&BIT(p)) {
            struct a733_pin_fields f;assert(a733_pin_fields_banked(b,p,&f));
            *reg(f.mux_address)|=15u<<f.mux_shift;
        }
    }
    struct a733_pins_operations ops={lock_enter,lock_leave,time_read,pin_read,pin_write};
    assert(a733_pins_init_banks(&pins,&ops,&pins,&handoff));
}
static enum a733_uart_platform_result step(struct fixture *f)
{ ++now_us;return a733_uart_platform_poll(&f->binding); }
static void ready(struct fixture *f)
{
    for (unsigned i=0;i<100;++i) {
        enum a733_uart_platform_result r=step(f);
        if (r==A733_UART_PLATFORM_READY) return;
        assert(r==A733_UART_PLATFORM_PENDING || r==A733_UART_PLATFORM_BUSY);
    }
    assert(0);
}
static void closed(struct fixture *f)
{
    assert(a733_uart_platform_close(&f->binding,1,now_us+1000)==A733_UART_PLATFORM_PENDING);
    for (unsigned i=0;i<100;++i) {
        enum a733_uart_platform_result r=step(f);
        if (r==A733_UART_PLATFORM_CLOSED) {assert(!f->binding.clock.id && !f->binding.pin_lease.id);return;}
        assert(r==A733_UART_PLATFORM_PENDING || r==A733_UART_PLATFORM_BUSY);
    }
    assert(0);
}
static void retained(struct fixture *f,unsigned pin_owned)
{
    assert(f->binding.clock.id && (!!f->binding.pin_lease.id)==pin_owned);
    assert(f->binding.fault && clocks.failed);
    unsigned r=reads,w=writes;
    assert(a733_uart_platform_poll(&f->binding)==A733_UART_PLATFORM_RETAINED);
    assert(reads==r && writes==w);
}
int main(void)
{
    /* Actual core configuration and FIFO service, then real resource release. */
    setup();initialize_fixture(&first,A733_UART1_BLUETOOTH,1,1);
    first.admission_reentry=1;first.gic_wait=2;
    *reg(irq_reg(1,0x100))=BIT(3)|BIT(4);*reg(irq_reg(1,0x200))=BIT(3)|BIT(4);
    ready(&first);
    assert(first.dll==1 && first.dlh==0 && first.ier==13 && first.uart[4]==0x22);
    assert(*reg(irq_reg(1,0x100))==BIT(4));
    initialize_fixture(&second,A733_UART1_HEADER,0,2);
    unsigned before=writes;
    assert(step(&second)==A733_UART_PLATFORM_BUSY && !second.binding.clock.id && writes==before);
    assert(a733_uart_platform_live(&first.binding,now_us+100)==A733_UART_PLATFORM_READY);
    struct a733_uart *u=a733_uart_platform_controller(&first.binding);assert(u);
    struct a733_uart_byte rx;uint8_t tx=0x42;struct a733_uart_result result;
    first.rx=0x5a;first.rx_count=1;
    assert(a733_uart_exchange(u,&rx,1,&tx,1,1,now_us+100,&result)==A733_UART_READY);
    assert(result.received==1 && rx.value==0x5a && result.transmitted==1 && first.tx==tx);
    first.shift_busy=1;
    assert(a733_uart_platform_close(&first.binding,1,now_us+1000)==A733_UART_PLATFORM_PENDING);
    for (unsigned i=0;i<20;++i) assert(step(&first)==A733_UART_PLATFORM_PENDING);
    assert(first.binding.uart.state==A733_UART_DRAINING && first.binding.pin_lease.id && first.binding.clock.id);
    first.shift_busy=0;
    for (unsigned i=0;i<100;++i) if (step(&first)==A733_UART_PLATFORM_CLOSED) break;
    assert(!first.binding.pin_lease.id && !first.binding.clock.id && !*reg(bus(1)));
    assert(*reg(irq_reg(1,0x200))==BIT(4));
    ready(&second);closed(&second); /* Alternate route can follow completed release. */

    /* Separate UART local scopes coexist; parent changes conflict. */
    setup();initialize_fixture(&first,A733_UART1_BLUETOOTH,1,1);ready(&first);
    initialize_fixture(&second,A733_UART2_PB,0,2);ready(&second);
    struct a733_clock_token change={0};
    assert(a733_clock_acquire(&clocks,9,0,A733_CLOCK_APB_UART,now_us+100,&change)==A733_CLOCK_BUSY);
    closed(&first);assert(second.binding.clock.id && *reg(bus(2))==0x10001);closed(&second);

    /* Always-on UART reset/gate shares one word with the other controller. */
    setup();initialize_fixture(&first,A733_S_UART0_PL,0,1);
    *reg(bus(7))=0x80020002; /* Neighbor gate/reset and an unrelated bit. */
    ready(&first);assert(*reg(bus(7))==0x80030003);
    closed(&first);assert(*reg(bus(7))==0x80020002);
    initialize_fixture(&second,A733_S_UART1_PL,0,2);
    *reg(bus(8))=0x80010001;
    ready(&second);closed(&second);assert(*reg(bus(8))==0x80010001);

    /* Normalized SYS_CLK24M accepts unequal PLL_REF factors for raw 26MHz
     * and 19.2MHz oscillators. Direct S_UART DCXO must remain 24MHz. */
    setup();initialize_fixture(&first,A733_UART2_PB,0,1);
    oscillator|=2u<<14;ccu[0]=0xf8000000u|(95u<<8)|(103u<<16);
    ready(&first);closed(&first);
    setup();initialize_fixture(&first,A733_UART2_PB,0,1);
    oscillator|=1u<<14;ccu[0]=0xf8000000u|(99u<<8)|(79u<<16);
    ready(&first);closed(&first);
    setup();initialize_fixture(&first,A733_S_UART0_PL,0,1);
    oscillator|=2u<<14;ccu[0]=0xf8000000u|(95u<<8)|(103u<<16);
    for(unsigned i=0;i<4&&!first.binding.fault;++i)(void)step(&first);
    retained(&first,0);
    setup();initialize_fixture(&first,A733_UART2_PB,0,1);
    oscillator|=2u<<14; /* Equal factors cannot normalize 26MHz. */
    for(unsigned i=0;i<4&&!first.binding.fault;++i)(void)step(&first);
    retained(&first,0);

    /* Admission and source refusals perform no controller access. */
    setup();initialize_fixture(&first,A733_UART1_BLUETOOTH,1,1);first.admitted=0;
    for (unsigned i=0;i<3;++i) assert(step(&first)==A733_UART_PLATFORM_PENDING);
    assert(!reads && !writes && first.binding.clock.id && !first.binding.pin_lease.id);
    first.admitted=1;ccu[0x538/4]=1;
    for (unsigned i=0;i<4 && !first.binding.fault;++i) (void)step(&first);
    retained(&first,0);
    setup();initialize_fixture(&first,A733_UART1_BLUETOOTH,1,1);first.correct_task=0;
    assert(step(&first)==A733_UART_PLATFORM_INVALID && !reads && !writes && !first.binding.clock.id);
    first.correct_task=1;first.admission_unknown=1;
    assert(step(&first)==A733_UART_PLATFORM_PENDING);
    assert(step(&first)==A733_UART_PLATFORM_RETAINED);retained(&first,0);

    /* Failed local writes retain the real pin/clock reservations and lock. */
    setup();initialize_fixture(&first,A733_UART1_BLUETOOTH,1,1);
    while (!first.binding.pin_lease.id) assert(step(&first)==A733_UART_PLATFORM_PENDING);
    first.fail_write=writes+1;
    assert(step(&first)==A733_UART_PLATFORM_RETAINED && lock_retained);retained(&first,1);
    setup();initialize_fixture(&first,A733_UART1_BLUETOOTH,1,1);first.halt_stuck=1;
    for (unsigned i=0;i<30;++i) assert(step(&first)==A733_UART_PLATFORM_PENDING);
    now_us=1000;assert(a733_uart_platform_poll(&first.binding)==A733_UART_PLATFORM_RETAINED);retained(&first,1);

    /* A late live data access faults both layers without releasing authority. */
    setup();initialize_fixture(&first,A733_UART1_BLUETOOTH,1,1);ready(&first);
    u=a733_uart_platform_controller(&first.binding);first.expire_write=writes+1;
    assert(a733_uart_platform_live(&first.binding,now_us+100)==A733_UART_PLATFORM_READY);
    assert(a733_uart_exchange(u,0,0,&tx,1,1,first.binding.deadline,&result)==A733_UART_FAULT);
    retained(&first,1);assert(!lock_retained); /* Data I/O owns no global mutex. */

    /* Ownership remains queryable after recovery, but live service refuses. */
    setup();initialize_fixture(&first,A733_UART1_BLUETOOTH,1,1);ready(&first);
    assert(lock_enter(0,now_us+100));pins.failed=1;lock_leave(0);
    int owned=0;
    assert(a733_pins_held_banked(&pins,&first.binding.pin_lease,&owned,now_us+100)==A733_PINS_OK && owned);
    assert(a733_uart_platform_live(&first.binding,now_us+100)==A733_UART_PLATFORM_RETAINED);
    retained(&first,1);assert(lock_retained);

    /* All documented routes reach their intended native controller, and no
     * native address admission grants another controller or parent write. */
    for (unsigned r=A733_UART0_PB0;r<=A733_S_UART1_PL;++r) {
        struct a733_uart_route_plan p;assert(a733_uart_route_plan(r,&p));
        assert(a733_pin_descriptor(p.bank,p.tx)->uart[p.function]==p.port+1);
        assert(a733_pin_descriptor(p.bank,p.rx)->uart[p.function]==p.port+1);
        assert(a733_uart_platform_native_address(p.port,uart_base(p.port)+0x14,0));
        assert(!a733_uart_platform_native_address(p.port,uart_base(p.port)+0x14,1));
        assert(!a733_uart_platform_native_address(p.port,uart_base((p.port+1)%9),1));
        assert(!a733_uart_platform_native_address(p.port,CCU+0x538,1));
        assert(!a733_uart_platform_native_address(p.port,RCCU+0x10,1));
    }
    puts("UART platform: actual registry/pins/controller composition, route exclusion, parent stability, masked GIC, complete drain/release and retained failures passed");
    return 0;
}
