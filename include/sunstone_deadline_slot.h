#ifndef SUNSTONE_DEADLINE_SLOT_H
#define SUNSTONE_DEADLINE_SLOT_H
/* One deadline owner's slot (runtime/deadline.c). Only the deadline runtime reads or writes the fields. The type is in a
 * header so that the OS image's tables can allocate the slots (tools/sunstone-manifest.py, SUNSTONE_DEADLINE_IMAGE_SLOTS). */
#include <stdatomic.h>
#include <stdint.h>
#include "sunstone_cache_line.h"
/* Each slot fills one cache line, so a slot's owner on one core never shares a line with another slot or with the
 * variables that the linker places next to the table. */
struct sunstone_deadline_slot {
    _Alignas(SS_CACHE_LINE) _Atomic(void *) owner; /* the registered task's handle */
    atomic_uint_fast64_t deadline;
    uint64_t sequence,last_deadline,woken_us;
    unsigned failed;
    unsigned index;                  /* the notification index of the wait in progress */
    unsigned own_index;              /* 1 + the index every wait of this owner uses (sunstone_deadline_register_index),
                                      * or 0 for the calling task's default (sunstone_deadline_notify_index) */
};
_Static_assert(sizeof(struct sunstone_deadline_slot)==SS_CACHE_LINE,"a deadline slot is one cache line");
#endif
