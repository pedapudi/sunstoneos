/* Secure Device storage survives the final no-Normal-writer barrier. This
 * profile has THS INTID71 and, with GPU recognition, INTID95. */
#include "cpu-return.h"
#include "cpu-return-runtime.h"
#include "a733_thermal.h"
#include "irq_table.h"
static struct {
    uint64_t route;
    uint32_t group,modifier,config,priority,valid;
} saved[
#ifdef SUNSTONE_A733_OS_GPU_LINEAR
2
#else
1
#endif
] __attribute__((section(".bss.a733_smp_monitor"),aligned(128)));
#define GICD UINT32_C(0x03400000)
static const unsigned ids[]={A733_THERMAL_INTID,
#ifdef SUNSTONE_A733_OS_GPU_LINEAR
95,
#endif
};
static void barrier(void){__asm__ volatile("dsb sy":::"memory");}
static uint32_t rd(uint32_t o){uint32_t v=*(volatile uint32_t *)(uintptr_t)(GICD+o);barrier();return v;}
static void wr(uint32_t o,uint32_t v){*(volatile uint32_t *)(uintptr_t)(GICD+o)=v;barrier();}
void os_cpu_return_spi_save(void)
{
    if(a733_irq_route_count!=sizeof(ids)/sizeof(*ids))return;
    for(unsigned i=0;i<a733_irq_route_count;++i){
        unsigned id=ids[i],core=i ? OS_RECOGNITION_CORE : 0;
        if(a733_irq_routes[i].intid!=id || a733_irq_routes[i].core!=core ||
           ((rd(0x100+id/32*4)|rd(0x200+id/32*4)|rd(0x300+id/32*4))&(1u<<(id%32))))return;
        saved[i].group=rd(0x80+id/32*4);saved[i].modifier=rd(0xd00+id/32*4);
        saved[i].config=rd(0xc00+id/16*4);saved[i].priority=rd(0x400+id/4*4);
        saved[i].route=*(volatile uint64_t *)(uintptr_t)(GICD+0x6000+id*8);barrier();
    }
    for(unsigned i=0;i<a733_irq_route_count;++i)saved[i].valid=1;
    barrier();
}
int os_cpu_return_spi_restore(void)
{
    if((rd(0)&7) || rd(0)&UINT32_C(0x80000000))return 0;
    for(unsigned i=0;i<sizeof(ids)/sizeof(*ids);++i)
        if(!saved[i].valid || (rd(0x100+ids[i]/32*4)&(1u<<(ids[i]%32))))return 0;
    for(unsigned i=0;i<sizeof(ids)/sizeof(*ids);++i){
        unsigned id=ids[i];const uint32_t bit=1u<<(id%32),word=id/32*4;
        while(rd(0x300+word)&bit)if(a733_smp_counter()>=a733_qualification_report.deadline)return 0;
        wr(0x280+word,bit);
        if(rd(0x200+word)&bit)return 0;
        wr(0x80+word,saved[i].group);wr(0xd00+word,saved[i].modifier);
        wr(0xc00+id/16*4,saved[i].config);wr(0x400+id/4*4,saved[i].priority);
        *(volatile uint64_t *)(uintptr_t)(GICD+0x6000+id*8)=saved[i].route;barrier();
        if(rd(0x80+word)!=saved[i].group || rd(0xd00+word)!=saved[i].modifier ||
           rd(0xc00+id/16*4)!=saved[i].config || rd(0x400+id/4*4)!=saved[i].priority ||
           *(volatile uint64_t *)(uintptr_t)(GICD+0x6000+id*8)!=saved[i].route)return 0;
    }
    return 1;
}
