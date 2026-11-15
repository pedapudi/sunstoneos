#ifndef SUNSTONE_CACHE_LINE_H
#define SUNSTONE_CACHE_LINE_H
/* The data cache line of the A733's Cortex-A76 and Cortex-A55 cores, in bytes: the unit that the scheduler's per-core
 * state (kernel/scheduler/owner.h) and the deadline slots (include/sunstone_deadline_slot.h) are aligned to, so that data
 * written by one core does not share a line with data that another core writes. */
#define SS_CACHE_LINE 64u
#endif
