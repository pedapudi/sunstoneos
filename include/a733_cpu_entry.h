#ifndef SUNSTONE_A733_CPU_ENTRY_H
#define SUNSTONE_A733_CPU_ENTRY_H
#define A733_ENTRY_MAGIC 0x53435045
#define A733_ENTRY_ARMED 1
#define A733_ENTRY_RELEASE 2
#define A733_ENTRY_ARRIVED 1
#define A733_ENTRY_RELEASED 2
#define A733_ENTRY_FAULT 3
#define A733_ENTRY_SLOT_BYTES 256
#define A733_ENTRY_STACK_BYTES 4096
#define A733_ENTRY_SCTLR_REJECT 0x02001005
#ifndef __ASSEMBLER__
#include "a733_cpu_start.h"
#include <stddef.h>
struct a733_cpu_command {
    uint64_t magic, epoch, core, state, inverse_epoch, reserved[11];
};
struct a733_cpu_report {
    uint64_t epoch, core, midr, mpidr, sctlr, state;
    uint64_t esr, far, elr, spsr, reason, reserved[5];
};
struct a733_cpu_slot {
    _Alignas(128) struct a733_cpu_command command;
    _Alignas(128) struct a733_cpu_report report;
};
_Static_assert(sizeof(struct a733_cpu_slot)==256,"entry assembly slot ABI");
_Static_assert(offsetof(struct a733_cpu_slot,report)==128,"entry report ABI");
_Static_assert(offsetof(struct a733_cpu_report,state)==40,"entry state ABI");
_Static_assert(offsetof(struct a733_cpu_report,reason)==80,"entry reason ABI");
enum a733_cpu_entry_state { A733_CPU_ENTRY_NEW, A733_CPU_ENTRY_PENDING,
    A733_CPU_ENTRY_DISPATCHED, A733_CPU_ENTRY_RETAINED };
enum a733_cpu_entry_error { A733_CPU_ENTRY_OK, A733_CPU_ENTRY_IO,
    A733_CPU_ENTRY_TIME, A733_CPU_ENTRY_DIRTY_SLOT, A733_CPU_ENTRY_POWER,
    A733_CPU_ENTRY_REPORT };
struct a733_cpu_entry_ops {
    int (*read_slot)(void *, unsigned, struct a733_cpu_slot *);
    int (*arm)(void *, unsigned, const struct a733_cpu_command *);
    int (*release)(void *, unsigned, uint64_t);
    int (*now_us)(void *, uint64_t *);
};
struct a733_cpu_entry_config {
    struct a733_cpu_start *power;
    struct a733_cpu_entry_ops ops;
    void *context;
    uint64_t epoch, entry;
};
struct a733_cpu_entry {
    struct a733_cpu_entry_config config;
    struct a733_cpu_report report;
    uint64_t last_now, deadline, lease;
    unsigned core, phase, initialized, entered, writes_attempted;
    enum a733_cpu_entry_state state;
    enum a733_cpu_entry_error error;
};
/* One privileged boot owner, one-shot zero storage. power is an initialized,
 * unpolled CPU power controller exclusively borrowed for this entire lifetime.
 * No caller may poll or mutate it separately. entry is the actual linked entry
 * address. Its code, vectors, mailbox and per-core stacks must be reserved,
 * published to physical memory and inaccessible to all other writers. Epoch
 * must be nonzero and never reused within that reservation's lifetime.
 * Callbacks are bounded, non-reentrant, exactly-one success, same physical
 * microsecond timebase as power, and preserve their original context. Owner,
 * config and power allocations must be disjoint. Callback-private writable
 * fields must not overlap them; context and all resources remain live. read_slot
 * returns a stable report or state zero if publication overlaps its snapshot.
 * arm publishes fields before ARMED; release validates the exact existing
 * command then publishes RELEASE. Both complete DSB SY before success and
 * signal SEV after publication. Uncertain writes may have taken effect.
 * No callback creates or authenticates physical ownership or cache coherence. */
int a733_cpu_entry_init(struct a733_cpu_entry *, const struct a733_cpu_entry_config *);
/* At most one mailbox operation or one existing power-controller poll. Uses
 * power's absolute deadline. DISPATCHED proves the secondary published its
 * release acknowledgment immediately before branching to the fixed continuation;
 * it does not prove continuation completion, coherent caches, GIC or FreeRTOS.
 * Every terminal result retains all resources; no rollback/reset/retry. */
enum a733_cpu_entry_state a733_cpu_entry_poll(struct a733_cpu_entry *);
/* Boot-only native adapter: core0 at EL3, MMU/data cache off, little-endian,
 * identity physical DRAM, no cacheable aliases or dirty cache lines covering
 * any reserved image/mailbox/stack memory. Requires prior code publication.
 * Validates local register state and linked ranges; cannot establish remote
 * firmware exclusion or validate physical DRAM ownership. Not an EL1 API. */
int a733_cpu_entry_native_init(struct a733_cpu_entry *, struct a733_cpu_start *, uint64_t epoch);
extern struct a733_cpu_slot a733_cpu_mailboxes[8];
extern unsigned char a733_cpu_entry_stacks[8 * A733_ENTRY_STACK_BYTES];
void a733_cpu_secondary_entry(void);
/* Must be supplied by platform integration; no default implementation.
 * x0 core, x1 epoch, x2 report PA, x3 command PA; SP_EL3 is this core's reserved
 * 4KiB stack top. DAIF masked; MMU/data/instruction caches off; little-endian.
 * Replace temporary vectors before changing translation/cache state or TPIDR_EL3.
 * Establish CPU-specific coherence, GIC, EL1 and scheduler state. Never return.
 * Return is caught and reported. Caller-saved registers otherwise unspecified. */
void a733_cpu_secondary_continue(unsigned long core, uint64_t epoch,
                                 struct a733_cpu_report *, struct a733_cpu_command *);
#endif
#endif
