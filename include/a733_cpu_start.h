#ifndef SUNSTONE_A733_CPU_START_H
#define SUNSTONE_A733_CPU_START_H
#include <stdint.h>

#define A733_CPU_COUNT 8u
enum a733_cpu_start_state { A733_CPU_START_NEW, A733_CPU_START_PENDING,
    A733_CPU_START_POWERED, A733_CPU_START_RETAINED };
enum a733_cpu_start_error { A733_CPU_START_NO_ERROR, A733_CPU_START_IO,
    A733_CPU_START_TIME, A733_CPU_START_INITIAL_STATE, A733_CPU_START_VERIFY };
struct a733_cpu_start_ops {
    int (*read32)(void *, uintptr_t, uint32_t *);
    int (*write32)(void *, uintptr_t, uint32_t);
    int (*barrier)(void *);
    int (*now_us)(void *, uint64_t *);
};
struct a733_cpu_start_config {
    struct a733_cpu_start_ops ops;
    void *context;
    uint64_t boot_mpidr, entry, lease, deadline;
    unsigned core;
};
struct a733_cpu_start {
    struct a733_cpu_start_config config;
    uint64_t last_now;
    uint32_t control, mode, architecture, power_status;
    enum a733_cpu_start_state state;
    enum a733_cpu_start_error error;
    unsigned step, initialized, entered, writes_attempted;
};
/* Pure topology conversion. Accept architectural MPIDR flags, reject reserved
 * bits and unknown affinities. Output unchanged on refusal. A733 uses Aff1 for
 * core number, Aff0/Aff2/Aff3 zero. Expected types: cores 0..5 A55, 6..7 A76. */
int a733_cpu_index(uint64_t mpidr, unsigned *out);
/* Privileged boot owner only, zero unused storage. One-shot, no reinit/disposal.
 * lease is an existing nonzero platform reservation, NOT created/verified here.
 * It must own this OFF secondary CPU, its boot/power words, stable CPU clocks,
 * rails, interconnect, and published executable entry/stack/mailbox allocations.
 * All remote firmware writers must have relinquished those resources. The
 * boot CPU must be core 0; this component admits cores 1..7 and a 4-byte-aligned
 * DRAM entry below 4 GiB. The entry runs at reset privilege and must park without
 * touching hotplug words until the boot owner hands off after POWERED.
 * Callbacks return exactly 1, are bounded/non-reentrant, and preserve their
 * original context. They perform ordered, privileged physical register access.
 * One serialized owner; callback state/storage must remain disjoint and live.
 * Requires independently qualified watchdog recovery before physical use.
 */
int a733_cpu_start_init(struct a733_cpu_start *, const struct a733_cpu_start_config *);
/* One register or barrier operation per poll, bounded by pre/post monotonic
 * microsecond checks. POWERED proves PPU state ON and request cleanup only:
 * it does not prove CPU entry, GIC setup, cache coherence or scheduler startup.
 * Every terminal result retains lease, entry, stack and mailbox. Failure has
 * no rollback/reset/retry and terminal polls invoke no callbacks. */
enum a733_cpu_start_state a733_cpu_start_poll(struct a733_cpu_start *);
#endif
