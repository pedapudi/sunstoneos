/* Secure Device storage survives the final no-Normal-writer barrier. This
 * profile has exactly one table entry: THS INTID71 on CPU0. */
#include "cpu-return.h"
#include "cpu-return-runtime.h"
#include "a733_thermal.h"
#include "irq_table.h"
static struct {
    uint64_t route;
    uint32_t group,modifier,config,priority,valid;
} saved __attribute__((section(".bss.a733_smp_monitor"),aligned(128)));
#define GICD UINT32_C(0x03400000)
#define ID A733_THERMAL_INTID
static void barrier(void){__asm__ volatile("dsb sy":::"memory");}
static uint32_t rd(uint32_t o){uint32_t v=*(volatile uint32_t *)(uintptr_t)(GICD+o);barrier();return v;}
static void wr(uint32_t o,uint32_t v){*(volatile uint32_t *)(uintptr_t)(GICD+o)=v;barrier();}
void os_cpu_return_spi_save(void)
{
    if(a733_irq_route_count!=1||a733_irq_routes[0].intid!=ID||a733_irq_routes[0].core!=0||
       ((rd(0x100+ID/32*4)|rd(0x200+ID/32*4)|rd(0x300+ID/32*4))&(1u<<(ID%32))))return;
    saved.group=rd(0x80+ID/32*4);saved.modifier=rd(0xd00+ID/32*4);
    saved.config=rd(0xc00+ID/16*4);saved.priority=rd(0x400+ID/4*4);
    saved.route=*(volatile uint64_t *)(uintptr_t)(GICD+0x6000+ID*8);barrier();saved.valid=1;barrier();
}
int os_cpu_return_spi_restore(void)
{
    const uint32_t bit=1u<<(ID%32),word=ID/32*4;
    if(!saved.valid||(rd(0)&7)||rd(0)&UINT32_C(0x80000000)||(rd(0x100+word)&bit))return 0;
    while(rd(0x300+word)&bit)if(a733_smp_counter()>=a733_qualification_report.deadline)return 0;
    wr(0x280+word,bit);
    if(rd(0x200+word)&bit)return 0;
    wr(0x80+word,saved.group);wr(0xd00+word,saved.modifier);
    wr(0xc00+ID/16*4,saved.config);wr(0x400+ID/4*4,saved.priority);
    *(volatile uint64_t *)(uintptr_t)(GICD+0x6000+ID*8)=saved.route;barrier();
    return rd(0x80+word)==saved.group&&rd(0xd00+word)==saved.modifier&&
        rd(0xc00+ID/16*4)==saved.config&&rd(0x400+ID/4*4)==saved.priority&&
        *(volatile uint64_t *)(uintptr_t)(GICD+0x6000+ID*8)==saved.route;
}
