#ifndef SUNSTONE_TLBI_H
#define SUNSTONE_TLBI_H
#include <stdint.h>
/* Optional periodic translation-invalidation measurement. Counts are atomic
 * observations, not a simultaneous hardware snapshot. Costs use 24 MHz counter
 * ticks. Control form 0 records wakes and issues no invalidation instruction. */
struct sunstone_tlbi_sample {
    uint64_t period_us,max_ticks,total_ticks;
    uint32_t wakes,issued,core,form;
};
#ifdef __cplusplus
extern "C" {
#endif
/* Available when SUNSTONE_TLBI_ISSUER is defined, including control value0.
 * Copies observations only; changes no task, timer or device state. */
int sunstone_tlbi_sample(struct sunstone_tlbi_sample *);
#ifdef __cplusplus
}
#endif
#endif
