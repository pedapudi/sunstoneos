/* G2D 2D engine service (SS_DEVICE_G2D): the start entry, quiesce owner and interrupt handler of the device class. The service is not
 * implemented yet: the entry creates no task, enables no interrupt and touches no register. When the session grants
 * the device, the entry registers the device's quiesce owner, whose step has nothing to stop and completes at once.
 * INTID 83 is routed for an application that declares the device and stays disabled, so the handler never runs; it
 * touches no register. */
#include "os.h"
#include "application.h"
#include "quiesce.h"

int os_g2d_task_privileged(void (*entry)(void *),void *context){(void)entry;(void)context;return 0;}
int os_g2d_pins(struct a733_pin_handoff *pins){return pins!=NULL;}
static enum os_quiesce_result quiesce_step(struct os_quiesce *q){(void)q;return OS_QUIESCE_DONE;}
static struct os_quiesce g2d_quiesce={.owner=OS_QUIESCE_G2D,.order=OS_QUIESCE_DEVICES,.step=quiesce_step};
int os_g2d_start(void)
{
    const struct ss_grant *grant=ss_grant();
    if(!grant||!(grant->devices&SS_DEVICE_G2D))return 1;
    return os_quiesce_register(&g2d_quiesce);
}
void os_g2d_irq(void){}
