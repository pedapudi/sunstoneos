/* Video decoder service (SS_DEVICE_VIDEO_DECODE): the start entry, quiesce owner and interrupt handler of the device class. The service is not
 * implemented yet: the entry creates no task, enables no interrupt and touches no register. When the session grants
 * the device, the entry registers the device's quiesce owner, whose step has nothing to stop and completes at once.
 * INTID 81 is routed for an application that declares the device and stays disabled, so the handler never runs; it
 * touches no register. */
#include "os.h"
#include "application.h"
#include "quiesce.h"

int os_video_decode_task_privileged(void (*entry)(void *),void *context){(void)entry;(void)context;return 0;}
int os_video_decode_pins(struct a733_pin_handoff *pins){return pins!=NULL;}
static enum os_quiesce_result quiesce_step(struct os_quiesce *q){(void)q;return OS_QUIESCE_DONE;}
static struct os_quiesce video_decode_quiesce={.owner=OS_QUIESCE_VIDEO_DECODE,.order=OS_QUIESCE_DEVICES,.step=quiesce_step};
int os_video_decode_start(void)
{
    const struct ss_grant *grant=ss_grant();
    if(!grant||!(grant->devices&SS_DEVICE_VIDEO_DECODE))return 1;
    return os_quiesce_register(&video_decode_quiesce);
}
void os_video_decode_irq(void){}
