#ifndef A733_QUALIFICATION_FREERTOS_CONFIG_H
#define A733_QUALIFICATION_FREERTOS_CONFIG_H
#include "../../platform/a733-smp-cached/FreeRTOSConfig.h"
#ifndef __ASSEMBLER__
#undef vApplicationPassiveIdleHook
void a733_qualification_idle(void);
#define vApplicationPassiveIdleHook a733_qualification_idle
#endif
#endif
