#include "irq_table.h"
#ifndef A733_IRQ_TABLE_MMIO_DEFINED
#define GICD UINT64_C(0x03400000)
static inline uint32_t gicd_read(uint64_t offset)
{return *(volatile uint32_t *)(uintptr_t)(GICD+offset);}
static inline void gicd_write(uint64_t offset,uint32_t value)
{*(volatile uint32_t *)(uintptr_t)(GICD+offset)=value;}
static inline uint64_t gicd_read64(uint64_t offset)
{return *(volatile uint64_t *)(uintptr_t)(GICD+offset);}
static inline void gicd_write64(uint64_t offset,uint64_t value)
{*(volatile uint64_t *)(uintptr_t)(GICD+offset)=value;}
static inline void gicd_barrier(void){__asm__ volatile("dsb sy":::"memory");}
#endif

int a733_irq_table_valid(void)
{
    if(!a733_irq_route_count || a733_irq_route_count>64)return 0;
    for(unsigned i=0;i<a733_irq_route_count;++i) {
        const struct a733_irq_route *r=&a733_irq_routes[i];
        if(r->intid<32 || r->intid>=1020 || r->core>=8 || !r->handler || r->priority<A733_IRQ_TABLE_MIN_PRIORITY)
            return 0;
        for(unsigned j=0;j<i;++j)if(a733_irq_routes[j].intid==r->intid)return 0;
    }
    return 1;
}
int a733_irq_table_dispatch(unsigned intid,unsigned core)
{
    for(unsigned i=0;i<a733_irq_route_count;++i) {
        const struct a733_irq_route *r=&a733_irq_routes[i];
        if(r->intid!=intid)continue;
        if(r->core!=core)return 0;
        r->handler();
        return 1;
    }
    return 0;
}
/* A733 numbers cores in MPIDR affinity level 1, so core c routes to Aff1=c. */
static uint64_t affinity(unsigned core){return (uint64_t)core<<8;}
int a733_irq_table_route(uint64_t deadline,uint64_t (*counter)(void))
{
    if(!counter || !a733_irq_table_valid() || counter()>=deadline)return 0;
    /* Affinity routing for both states, groups disabled, no pending write. */
    uint32_t ctlr=gicd_read(0);
    if((ctlr&0x70u)!=0x30u || (ctlr&7u) || (ctlr&(1u<<31)))return 0;
    unsigned lines=((gicd_read(4)&31u)+1u)*32u;
    for(unsigned i=0;i<a733_irq_route_count;++i) {
        unsigned id=a733_irq_routes[i].intid;
        uint32_t bit=1u<<(id%32),word=4u*(id/32);
        /* Common boot left every SPI disabled, inactive and not pending. */
        if(id>=lines || ((gicd_read(0x100+word)|gicd_read(0x200+word)|gicd_read(0x300+word))&bit))
            return 0;
    }
    for(unsigned i=0;i<a733_irq_route_count;++i) {
        const struct a733_irq_route *r=&a733_irq_routes[i];
        unsigned id=r->intid,shift=8u*(id%4u);
        uint32_t bit=1u<<(id%32),word=4u*(id/32);
        gicd_write(0x80+word,gicd_read(0x80+word)|bit);
        gicd_write(0xd00+word,gicd_read(0xd00+word)&~bit);
        gicd_write(0xc00+4u*(id/16),gicd_read(0xc00+4u*(id/16))&~(3u<<(2u*(id%16))));
        gicd_write(0x400+4u*(id/4),(gicd_read(0x400+4u*(id/4))&~(0xffu<<shift))|
            ((uint32_t)r->priority<<shift));
        /* One 64-bit access writes the complete affinity route. */
        gicd_write64(0x6000+8u*id,affinity(r->core));
    }
    gicd_barrier();
    for(unsigned i=0;i<a733_irq_route_count;++i) {
        const struct a733_irq_route *r=&a733_irq_routes[i];
        unsigned id=r->intid,shift=8u*(id%4u);
        uint32_t bit=1u<<(id%32),word=4u*(id/32);
        if(!(gicd_read(0x80+word)&bit) || (gicd_read(0xd00+word)&bit) ||
           (gicd_read(0xc00+4u*(id/16))&(3u<<(2u*(id%16)))) ||
           ((gicd_read(0x400+4u*(id/4))>>shift)&0xffu)!=r->priority ||
           gicd_read64(0x6000+8u*id)!=affinity(r->core) ||
           ((gicd_read(0x100+word)|gicd_read(0x200+word)|gicd_read(0x300+word))&bit))
            return 0;
    }
    return !(gicd_read(0)&(1u<<31)) && counter()<deadline;
}
