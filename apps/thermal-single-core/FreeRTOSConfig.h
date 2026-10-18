#include "../storage-read-single-core/FreeRTOSConfig.h"
/* Native THERMAL owner and clients remain on the boot CPU. */
#define xTaskCreateStaticAffinitySet(fn,name,depth,arg,priority,stack,tcb,affinity) xTaskCreateStatic(fn,name,depth,arg,priority,stack,tcb)
