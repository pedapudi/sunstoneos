/* The image's fixed shared-peripheral interrupt table. The EL3 boot programs
 * these routes (boot.patch) and the shared handler dispatches through them;
 * every other INTID keeps the status-4 stop. Device owners enable sources. */
#include "irq_table.h"
#include "a733_thermal.h"
void os_thermal_irq(void);
#ifdef SUNSTONE_A733_OS_AUDIO
void os_audio_irq(void);
#endif
const struct a733_irq_route a733_irq_routes[]={
    {A733_THERMAL_INTID,0,0xd8,os_thermal_irq},
#ifdef SUNSTONE_A733_OS_AUDIO
    /* DMA0 (A733_DMA0_INTID), on the core of the audio DMA worker. */
    {143,0,0xd8,os_audio_irq},
#endif
};
const unsigned a733_irq_route_count=sizeof(a733_irq_routes)/sizeof(a733_irq_routes[0]);
