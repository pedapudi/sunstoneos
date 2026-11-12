/* The generated image table supplies Secure Device storage for every route. */
#include "cpu-return.h"
#include "cpu-return-runtime.h"
#include "a733_thermal.h"
#include "irq_table.h"
#define saved os_cpu_return_spi_saved
static unsigned saved_count __attribute__((section(".bss.a733_smp_monitor")));
#define GICD UINT32_C(0x03400000)
static void barrier(void){__asm__ volatile("dsb sy":::"memory");}
static uint32_t rd(uint32_t o){uint32_t v=*(volatile uint32_t *)(uintptr_t)(GICD+o);barrier();return v;}
static void wr(uint32_t o,uint32_t v){*(volatile uint32_t *)(uintptr_t)(GICD+o)=v;barrier();}
void os_cpu_return_spi_save(void)
{
    saved_count=0;
    if(a733_irq_route_count>os_cpu_return_spi_capacity||!a733_irq_table_valid())return;
    for(unsigned i=0;i<a733_irq_route_count;++i)saved[i].valid=0;
    for(unsigned i=0;i<a733_irq_route_count;++i){
        unsigned id=a733_irq_routes[i].intid;
        if(((rd(0x100+id/32*4)|rd(0x200+id/32*4)|rd(0x300+id/32*4))&(1u<<(id%32))))return;
        saved[i].intid=id;
        saved[i].group=rd(0x80+id/32*4);saved[i].modifier=rd(0xd00+id/32*4);
        saved[i].config=rd(0xc00+id/16*4);saved[i].priority=rd(0x400+id/4*4);
        saved[i].route=*(volatile uint64_t *)(uintptr_t)(GICD+0x6000+id*8);barrier();
    }
    for(unsigned i=0;i<a733_irq_route_count;++i)saved[i].valid=1;
    saved_count=a733_irq_route_count;barrier();
}
int os_cpu_return_spi_restore(void)
{
    if(saved_count!=a733_irq_route_count||saved_count>os_cpu_return_spi_capacity)return 0;
    if((rd(0)&7) || rd(0)&UINT32_C(0x80000000))return 0;
    for(unsigned i=0;i<saved_count;++i)
        if(!saved[i].valid || (rd(0x100+saved[i].intid/32*4)&(1u<<(saved[i].intid%32))))return 0;
    for(unsigned i=0;i<saved_count;++i){
        unsigned id=saved[i].intid;const uint32_t bit=1u<<(id%32),word=id/32*4;
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
