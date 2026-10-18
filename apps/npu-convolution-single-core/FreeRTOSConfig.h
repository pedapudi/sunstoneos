#include "../gpu-render-single-core/FreeRTOSConfig.h"
/* Every trusted task executes at EL1 on the admitted boot CPU. */
#define xTaskCreateStaticAffinitySet(fn,name,depth,arg,priority,stack,tcb,affinity) xTaskCreateStatic(fn,name,depth,arg,priority,stack,tcb)
