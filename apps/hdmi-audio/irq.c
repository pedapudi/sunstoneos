#include "application.h"
#include "irq_table.h"
static void dma_irq(void){dma_service_i2s_irq_handler(&audio.irq);}
const struct a733_irq_route a733_irq_routes[]={{A733_DMA0_INTID,0,0xd8,dma_irq}};
const unsigned a733_irq_route_count=sizeof(a733_irq_routes)/sizeof(a733_irq_routes[0]);
