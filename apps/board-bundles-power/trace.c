/* SPDX-License-Identifier: Apache-2.0 */
/* Records of bundles 6 and 7; see trace.h. */
#include "trace.h"

void bb_twi_observe(struct bb_twi *t, unsigned write, uintptr_t address, uint32_t value)
{
    if (address != BB_TWI_CNTR)
        return;
    if (write) {
        /* START and repeated START 0x68, byte 0x48, STOP 0x58. */
        t->mode = value == 0x58 ? 2 : value == 0x68 || value == 0x48 ? 1 : 0;
        t->condition = value == 0x68;
        t->polls = 0;
        return;
    }
    ++t->reads;
    t->enable_seen |= value & 0x80;
    if (t->mode == 1) {
        if (!t->polls++) {
            if (value & 8) ++t->first_set;
            else ++t->first_clear;
        }
        if (!(value & 8))
            return;
        if (t->polls > t->polls_max) t->polls_max = t->polls;
        if (t->condition) ++t->conditions;
        else ++t->bytes;
        t->mode = 0;
    } else if (t->mode == 2 && !(value & 0x10)) {
        ++t->stops;
        t->mode = 0;
    }
}

void bb_status_spin(int (*read)(void *, uintptr_t, uint32_t *), void *context, uint64_t (*now)(void),
    uintptr_t status, uint64_t start, uint64_t bound, struct bb_status_trace *t)
{
    *t = (struct bb_status_trace){0};
    uint32_t previous = 0;
    for (;;) {
        uint32_t v;
        if (read(context, status, &v) != 1)
            return;
        const uint64_t at = now() - start;
        /* The first value and every change; the count goes on past the
         * stored entries. */
        if (!t->reads++ || v != previous) {
            if (t->changes < BB_STATUS_CHANGES) {
                t->change[t->changes].value = v;
                t->change[t->changes].at = at;
            }
            ++t->changes;
        }
        if (t->reads == 1)
            t->first = v;
        previous = v;
        t->last_at = at;
        if ((v & 15) == 8) {
            t->on = 1;
            t->on_at = at;
            return;
        }
        if (at >= bound) {
            t->bounded = 1;
            return;
        }
    }
}
