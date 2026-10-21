/* The image's fixed shared-peripheral interrupt table: only DMA0, routed to
 * the DMA worker's core. The EL3 boot programs the route (boot.patch) and
 * the shared handler dispatches through it; every other INTID keeps the
 * status-4 stop. The DMA service enables the source through its rearm. */
#include "application.h"
#include "irq_table.h"
static void dma_irq(void){dma_service_i2s_irq_handler(&app.dma_irq);}
const struct a733_irq_route a733_irq_routes[]={
    {A733_DMA0_INTID,0,0xd8,dma_irq},
};
const unsigned a733_irq_route_count=sizeof(a733_irq_routes)/sizeof(a733_irq_routes[0]);
