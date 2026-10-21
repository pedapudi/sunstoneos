/* Host model of the fixed SPI route table: validation, dispatch and EL3 route
 * programming against a modeled GIC-600 distributor. No hardware access. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint8_t gicd[0x10000] __attribute__((aligned(8)));
static uint64_t now, deadline_step;
static unsigned writes_after_refusal;
#define A733_IRQ_TABLE_MMIO_DEFINED 1
#define A733_IRQ_TABLE_CONST
static uint32_t gicd_read(uint64_t o){uint32_t v;memcpy(&v,gicd+o,4);return v;}
static void gicd_write(uint64_t o,uint32_t v){memcpy(gicd+o,&v,4);++writes_after_refusal;}
static uint64_t gicd_read64(uint64_t o){uint64_t v;memcpy(&v,gicd+o,8);return v;}
static void gicd_write64(uint64_t o,uint64_t v){memcpy(gicd+o,&v,8);++writes_after_refusal;}
static void gicd_barrier(void){}
#include "../platform/a733-smp-cached/irq_table.c"

static unsigned calls[3];
static void usb_host(void){++calls[0];}
static void dma0(void){++calls[1];}
static void thermal(void){++calls[2];}
struct a733_irq_route a733_irq_routes[4];
unsigned a733_irq_route_count;
static uint64_t counter(void){now+=deadline_step;return now;}

static void reset_gic(void)
{
    memset(gicd,0,sizeof(gicd));
    gicd_write(0,0x30);          /* affinity routing for both states, groups off */
    gicd_write(4,10);            /* ITLinesNumber 10: 352 interrupt lines */
    for(unsigned i=0;i<11;++i)gicd_write(0x80+4*i,0);
    now=0;deadline_step=1;writes_after_refusal=0;
}
static void table(unsigned n)
{
    const struct a733_irq_route base[3]={{187,0,0xd8,usb_host},{82,1,0xc0,dma0},{105,6,0xb0,thermal}};
    memcpy(a733_irq_routes,base,sizeof(base));a733_irq_route_count=n;
}
int main(void)
{
    table(3);
    assert(a733_irq_table_valid());
    /* Dispatch runs only the routed handler on its core. */
    assert(a733_irq_table_dispatch(187,0) && calls[0]==1);
    assert(!a733_irq_table_dispatch(187,1) && calls[0]==1);
    assert(a733_irq_table_dispatch(105,6) && calls[2]==1);
    assert(!a733_irq_table_dispatch(90,0));
    /* Routing writes group, level mode, priority and Aff1 core. */
    reset_gic();
    assert(a733_irq_table_route(1000,counter));
    assert(gicd_read(0x80+4*5)&(1u<<27));
    assert(((gicd_read(0x400+4*(187/4))>>24)&0xff)==0xd8);
    assert(gicd_read64(0x6000+8*187)==0);
    assert(gicd_read64(0x6000+8*82)==UINT64_C(0x100));
    assert(gicd_read64(0x6000+8*105)==UINT64_C(0x600));
    assert(!(gicd_read(0xc00+4*(82/16))&(3u<<(2*(82%16)))));
    /* Refusals: each changes one condition and writes no route register. */
    struct { const char *name; void (*prepare)(void); } cases[]={
        {"enabled groups",0},{"pending write",0},{"enabled source",0},{"line beyond ITLines",0},
        {"expired deadline",0},{"duplicate intid",0},{"api priority",0},{"bad core",0},{"sgi",0},
    };
    for(unsigned k=0;k<sizeof(cases)/sizeof(cases[0]);++k) {
        reset_gic();table(3);
        uint64_t deadline=1000;
        switch(k) {
        case 0: gicd_write(0,0x33);break;
        case 1: gicd_write(0,0x30|(1u<<31));break;
        case 2: gicd_write(0x100+4*(187/32),1u<<(187%32));break;
        case 3: gicd_write(4,4);break;                       /* 160 lines: 187 is absent */
        case 4: deadline=1;break;
        case 5: a733_irq_routes[1].intid=187;break;
        case 6: a733_irq_routes[0].priority=0xa0;break;
        case 7: a733_irq_routes[2].core=8;break;
        case 8: a733_irq_routes[1].intid=1;break;
        }
        writes_after_refusal=0;
        assert(!a733_irq_table_route(deadline,counter));
        assert(writes_after_refusal==0);
        printf("refused: %s\n",cases[k].name);
    }
    table(0);assert(!a733_irq_table_valid());
    puts("PASS irq table: validation, per-core dispatch, EL3 route programming and nine refusals");
    return 0;
}
