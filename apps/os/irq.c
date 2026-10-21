/* The image's fixed shared-peripheral interrupt table. The EL3 boot programs
 * these routes (boot.patch) and the shared handler dispatches through them;
 * every other INTID keeps the status-4 stop. Device owners enable sources. */
#include "irq_table.h"
#include "a733_thermal.h"
void os_thermal_irq(void);
const struct a733_irq_route a733_irq_routes[]={
    {A733_THERMAL_INTID,0,0xd8,os_thermal_irq},
};
const unsigned a733_irq_route_count=sizeof(a733_irq_routes)/sizeof(a733_irq_routes[0]);
