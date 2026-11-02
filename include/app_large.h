#ifndef SUNSTONE_APP_LARGE_H
#define SUNSTONE_APP_LARGE_H
#include "app_region.h"
#include "sunstone_large.h"
/* One admitted large region of an application. */
struct large_slot {
    uint64_t base, bytes;
    uint32_t index, flags, state, error;
};
/* Take every region of the application's records from the ledger, all or nothing. out has out_capacity
 * (at least count) entries and is indexed by the record's index. Returns an app_region_status; on failure
 * nothing is held. */
int large_admit(struct app_region_ledger *, uint8_t owner, const struct ss_large_region *records, unsigned count,
                struct large_slot *out, unsigned out_capacity);
#endif
