#ifndef SUNSTONE_BG_H
#define SUNSTONE_BG_H
#include "sunstone_bg_count.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Optional scheduler measurement, selected by SUNSTONE_BG_LOAD. Start once;
 * phases are 0..2. One stop caller and all workers retain their task identities.
 * Stop publishes a hint to its caller after every worker stops writing.
 * The caller rechecks ulSunstoneBgStopped after a runtime/deadline wait and
 * keeps its original absolute limit. It may read statistics once the count
 * equals SUNSTONE_BG_TASKS. No reset or second workload lifetime exists. */
void vSunstoneBgStart(void);
void vSunstoneBgPhase(unsigned phase);
void vSunstoneBgStop(void);
unsigned ulSunstoneBgStopped(void);
/* kind0 bursts, kind1 busy microseconds, kind2 lateness histogram bin arg. */
unsigned long ulSunstoneBgValue(unsigned phase,unsigned kind,unsigned arg);
#ifdef __cplusplus
}
#endif
#endif
