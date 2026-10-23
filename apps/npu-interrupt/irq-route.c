#include "irq-route.h"
#include "a733_mmio.h"

#define GICD UINT32_C(0x03400000)
#define GICR UINT32_C(0x03460000)
#define BIT (UINT32_C(1) << 1)
#define RWP (UINT32_C(1) << 31)
#define CONFIG_MASK (UINT32_C(3) << 2)
#define PRIORITY_MASK (UINT32_C(255) << 8)
#define PRIORITY (UINT32_C(0xd8) << 8)

static uint64_t read_route(void)
{ return a733_mmio_read64(GICD+0x6308); }
static void write_route(uint64_t route)
{ a733_mmio_write64_dsb_sy_after(GICD+0x6308,route); }
static void replace(uint32_t offset, uint32_t mask, uint32_t bits)
{ a733_mmio_write32_dsb_sy_after(GICD+offset,(a733_mmio_read32(GICD+offset)&~mask)|bits); }
static int timely(uint64_t deadline)
{
    uint64_t now; __asm__ volatile("mrs %0,cntpct_el0" : "=r"(now));
    return now<deadline;
}
static int el3_boot_cpu(void)
{
    uint64_t el,mpidr;
    __asm__ volatile("mrs %0,CurrentEL\n mrs %1,mpidr_el1" : "=r"(el),"=r"(mpidr));
    return el==12 && !(mpidr&UINT64_C(0xff00ffffff));
}
static int propagated(uint64_t deadline)
{
    while(a733_mmio_read32(GICD)&RWP)if(!timely(deadline))return 0;
    return timely(deadline);
}
static int quiet(void)
{
    return !((a733_mmio_read32(GICD+0x10c)|a733_mmio_read32(GICD+0x20c)|a733_mmio_read32(GICD+0x30c))&BIT);
}
static int monitor_ready(uint64_t deadline, int groups_off)
{
    if(!el3_boot_cpu() || !timely(deadline))return 0;
    uint32_t ctlr=a733_mmio_read32(GICD);
    return !(ctlr&(1u<<6)) && (ctlr&0x30u)==0x30u &&
        (!groups_off || !(ctlr&7u)) && (a733_mmio_read32(GICD+4)&31u)>=3 &&
        !(a733_mmio_read32(GICR)&9u) /* No LPIs or pending redistributor control write. */ &&
        propagated(deadline) && quiet();
}
int npu_irq_route_capture(struct npu_irq_route_snapshot *s,uint64_t deadline)
{
    if(!s)return 0;
    s->phase=0;
    if(!monitor_ready(deadline,0))return 0;
    s->route=read_route();
    s->group=a733_mmio_read32(GICD+0x8c)&BIT;
    s->modifier=a733_mmio_read32(GICD+0xd0c)&BIT;
    s->configuration=a733_mmio_read32(GICD+0xc18)&CONFIG_MASK;
    s->priority=a733_mmio_read32(GICD+0x460)&PRIORITY_MASK;
    if(!quiet() || !timely(deadline))return 0;
    s->phase=1;return 1;
}
int npu_irq_route_install(struct npu_irq_route_snapshot *s,uint64_t deadline)
{
    if(!s || s->phase!=1 || !monitor_ready(deadline,1))return 0;
    /* A partial installation is retained; it must never be mistaken for an
     * untouched captured route by a caller attempting ordinary return. */
    s->phase=2;
    replace(0x8c,BIT,BIT);replace(0xd0c,BIT,0);
    replace(0xc18,CONFIG_MASK,0);replace(0x460,PRIORITY_MASK,PRIORITY);
    write_route(0);
    return propagated(deadline) && quiet() &&
        (a733_mmio_read32(GICD+0x8c)&BIT)==BIT && !(a733_mmio_read32(GICD+0xd0c)&BIT) &&
        !(a733_mmio_read32(GICD+0xc18)&CONFIG_MASK) &&
        (a733_mmio_read32(GICD+0x460)&PRIORITY_MASK)==PRIORITY && read_route()==0;
}
int npu_irq_route_restore(struct npu_irq_route_snapshot *s,uint64_t deadline)
{
    if(!s || s->phase!=2 || !monitor_ready(deadline,1))return 0;
    write_route(s->route);
    replace(0x460,PRIORITY_MASK,s->priority);
    replace(0xc18,CONFIG_MASK,s->configuration);
    replace(0xd0c,BIT,s->modifier);replace(0x8c,BIT,s->group);
    if(!propagated(deadline) || !quiet() || read_route()!=s->route ||
       (a733_mmio_read32(GICD+0x460)&PRIORITY_MASK)!=s->priority ||
       (a733_mmio_read32(GICD+0xc18)&CONFIG_MASK)!=s->configuration ||
       (a733_mmio_read32(GICD+0xd0c)&BIT)!=s->modifier ||
       (a733_mmio_read32(GICD+0x8c)&BIT)!=s->group)return 0;
    s->phase=3;return 1;
}
