#ifndef SUNSTONE_BOARD_FAIL_RECORD_H
#define SUNSTONE_BOARD_FAIL_RECORD_H
/* Whether a failure note goes to the vector's first-failure call (SVC #0x5c0e, apps/os/vectors.patch). The first note always does. One
 * later note does when the first was an application's (SUNSTONE_CRUMB_FAIL_APPLICATION) and this one is not: a device that failed during
 * a failure return. The vector replaces the retained record only in that case (FAULT's level 0xc), so this is the same rule on the C side:
 * without it the later note never reaches the vector. */
#include <stdatomic.h>
#include <stdint.h>
static inline int board_fail_record_wanted(atomic_uint *recorded, atomic_uint *first_site, atomic_uint *second, uint32_t site)
{
    if(!atomic_exchange_explicit(recorded,1,memory_order_acq_rel)){atomic_store_explicit(first_site,site,memory_order_release);return 1;}
    if(((site>>12)&15u)==4u)return 0;                                              /* SUNSTONE_CRUMB_FAIL_APPLICATION */
    if(((atomic_load_explicit(first_site,memory_order_acquire)>>12)&15u)!=4u)return 0;
    return !atomic_exchange_explicit(second,1,memory_order_acq_rel);
}
#endif
