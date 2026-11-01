/* Fixed returning-probe CPU owner. See CPU-OWNERSHIP.md and the source pins
 * in boot/a733/stage2/cpu-sources.json. No clock, rail or reset writes. */
#include "probe.h"
#include "boot.h"
#include "sunstone_probe.h"
#include "a733_mmio.h"

struct i2s_probe_cpu_state {
    uint32_t policy[8],control[8],mode[8];
    uint32_t active,attempted,polls,pad;
    uint64_t deadline,last,acquire_ticks,release_ticks;
} __attribute__((aligned(64)));
static struct i2s_probe_cpu_state cpu __attribute__((section(".monitor_state")));
_Static_assert(sizeof(cpu)%64==0,"CPU owner has dedicated monitor cache lines");
static uintptr_t ppu(unsigned c){return UINT32_C(0x07052000)+(uintptr_t)c*0x1000;}
static uintptr_t control(unsigned c){return UINT32_C(0x07050200)+(uintptr_t)c*4;}
static uint32_t rd(uintptr_t a){return a733_mmio_read32_dsb_sy_both(a);}
static _Noreturn void fault(unsigned site,uintptr_t a,uint32_t value)
{sunstone_probe_fault(site,value,0,a);}
static void bounded(uintptr_t a)
{
    uint64_t now=counter();
    /* Retain the counter or poll-count input that latched the timing fault. */
    if(now<cpu.last)fault(0x8e,a,(uint32_t)now);
    if(now>=cpu.deadline)fault(0x8f,a,(uint32_t)now);
    if(++cpu.polls>65536)fault(0x90,a,cpu.polls);
    cpu.last=now;
}
static void same(uintptr_t a,uint32_t mask,uint32_t wanted,unsigned site)
{uint32_t v=rd(a);bounded(a);if((v&mask)!=wanted)fault(site,a,v);}
static void requests(unsigned c)
{
    /* Vendor CPU_OFF profile: GIC wake stays disabled; neither software
     * start request may change while this owner holds or releases a core. */
    same(control(c),UINT32_MAX,cpu.control[c],0x87);
    same(control(c)+0x20,UINT32_MAX,cpu.mode[c],0x87);
}
static void window(void)
{
    cpu.last=counter();cpu.deadline=cpu.last+COUNTER_HZ/10;
    if(cpu.deadline>sunstone_probe_deadline)cpu.deadline=sunstone_probe_deadline;
    cpu.polls=0;
}
/* The production loop and the CPU1 isolation check execute this same helper. */
__attribute__((noinline)) static void transition(unsigned c,uint32_t wanted)
{
    uintptr_t a=ppu(c);requests(c);cpu.attempted|=1u<<c;
    a733_mmio_write32_dsb_sy_both(a,wanted);
    /* DEN0051E 3.4.3/4.1.1: PWSR acknowledges dynamic-enable changes only
     * after an in-flight transition completes. Await actual OFF as well;
     * PWPR may retain its old readback until this completion. */
    for(;;){uint32_t v=rd(a+8);bounded(a+8);requests(c);
        if(v==wanted)break;
        if(v&~UINT32_C(0x108))fault(0x88,a+8,v);
    }
    same(a,UINT32_MAX,wanted,0x89);
}
uint64_t sunstone_probe_acquire_secondaries(void)
{
    cpu.active=cpu.attempted=0;window();uint64_t start=cpu.last;
    /* Preflight all seven before the first mutation. These are the shared
     * probe's existing identity/OFF/request predicates, with acknowledged
     * dynamic OFF admitted solely as input to the static transition. */
    for(unsigned c=1;c<8;++c){uintptr_t a=ppu(c);
        same(a+0xfc8,0xfff00fff,0x0b60043b,0x80);
        same(a+0xfcc,UINT32_MAX,0x11,0x80);
        same(a+0xfb0,0x101101ff,0x10110100,0x80);
        same(a+4,UINT32_MAX,0,0x81);
        uint32_t p=rd(a);bounded(a);
        if(p&~UINT32_C(0x100))fault(0x82,a,p);
        cpu.policy[c]=p;same(a+8,UINT32_MAX,p,0x83);
        uint32_t v=rd(control(c));bounded(control(c));
        if((v&3)!=2)fault(0x84,control(c),v);cpu.control[c]=v;
        v=rd(control(c)+0x20);bounded(control(c)+0x20);
        if(v&1)fault(0x85,control(c)+0x20,v);cpu.mode[c]=v;
    }
    for(unsigned c=1;c<8;++c){
        requests(c);same(ppu(c),UINT32_MAX,cpu.policy[c],0x86);
        same(ppu(c)+8,UINT32_MAX,cpu.policy[c],0x86);
        if(cpu.policy[c])transition(c,0);
    }
    for(unsigned c=1;c<8;++c){requests(c);
        same(ppu(c),UINT32_MAX,0,0x8a);same(ppu(c)+8,UINT32_MAX,0,0x8a);}
    cpu.active=1;cpu.acquire_ticks=counter()-start;return SUNSTONE_PROBE_READY;
}
uint64_t sunstone_probe_release_secondaries(void)
{
    if(!cpu.active)fault(0x8b,0,0);
    window();uint64_t start=cpu.last;
    for(unsigned c=1;c<8;++c){requests(c);
        same(ppu(c),UINT32_MAX,0,0x8c);same(ppu(c)+8,UINT32_MAX,0,0x8c);}
    for(unsigned c=1;c<8;++c)if(cpu.policy[c])transition(c,cpu.policy[c]);
    for(unsigned c=1;c<8;++c){requests(c);
        same(ppu(c),UINT32_MAX,cpu.policy[c],0x8d);
        same(ppu(c)+8,UINT32_MAX,cpu.policy[c],0x8d);}
    cpu.active=0;cpu.release_ticks=counter()-start;return SUNSTONE_PROBE_READY;
}
