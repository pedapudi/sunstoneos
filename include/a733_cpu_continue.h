#ifndef SUNSTONE_A733_CPU_CONTINUE_H
#define SUNSTONE_A733_CPU_CONTINUE_H
#define A733_CONTINUE_MAGIC 0x53435043
#define A733_CONTINUE_PENDING 1
#define A733_CONTINUE_GIC_READY 2
#define A733_CONTINUE_EL1_READY 3
#define A733_CONTINUE_SCHEDULER_REQUEST 4
#define A733_CONTINUE_RETAINED 5
#define A733_CONTINUE_STACK_BYTES 32768
#ifndef __ASSEMBLER__
#include "a733_cpu_entry.h"
struct a733_cpu_continue_plan {
    uint64_t magic, epoch, deadline_us, ttbr0, reserved[12];
};
struct a733_cpu_continue_record {
    uint64_t epoch, core, state, error, redistributor, esr, far, elr, reserved[8];
};
struct a733_cpu_continue_ops {
    int (*read32)(void *,uintptr_t,uint32_t *);
    int (*read64)(void *,uintptr_t,uint64_t *);
    int (*write32)(void *,uintptr_t,uint32_t);
    int (*now_us)(void *,uint64_t *);
};
struct a733_cpu_continue_config {
    struct a733_cpu_continue_ops ops;
    void *context;
    uint64_t epoch, deadline_us, mpidr;
};
struct a733_cpu_continue {
    struct a733_cpu_continue_config config;
    uint64_t last_now, redistributor;
    uint32_t control, waker, configuration, priority, seen;
    unsigned core, frame, phase, state, error, initialized, entered, writes_attempted;
    /* Appended so that the fields above keep the frozen native layout: the
     * last GICD_CTLR value read in phase 0. */
    uint32_t distributor;
};
/* Called by a released secondary or primary core0, under its CPU/register lease.
 * Primary GIC distributor security/routing setup has completed; all SGI/PPI
 * sources for this core are quiescent and exclusively owned by this boot image.
 * Callbacks are ordered/bounded/non-reentrant, return exactly one on success,
 * and share the owner's monotonic microsecond timebase. No callback establishes
 * physical ownership. Fresh zero state, copied config, disjoint live storage.
 * GIC-600 redistributor power is requested for this core only; rejected requests
 * recheck group stability within the original deadline. Access failure retains.
 * GIC_READY proves only local register sequencing; CPU system registers,
 * translation, exceptions and scheduler startup are separate subsequent steps. */
int a733_cpu_continue_init(struct a733_cpu_continue *,const struct a733_cpu_continue_config *);
unsigned a733_cpu_continue_poll(struct a733_cpu_continue *);
/* A733 core n has Aff1=n and Aff0=0: SGI0 to one core, no broadcast. */
int a733_cpu_sgi1r(unsigned core,uint64_t *out);
/* Canonical diagnostic mappings: Device peripherals; 128MiB Normal NC DRAM;
 * 0x41000000 code block read-only executable; 0x42000000 monitor block EL1-only
 * non-executable. All other DRAM is EL0/EL1 RW, non-executable. No task isolation.
 * Arrays are separate 4KiB-aligned owned tables, CPU identity addresses below4G.
 * This constructs descriptors; it does not publish them or qualify atomicity. */
int a733_cpu_continue_tables(uint64_t level1[512],uint64_t level2[512],uint64_t level2_pa);
/* Primary-only native preparation: one fresh epoch, absolute deadline in us.
 * Requires core0 at EL3, MMU/data caches off, published code and all participants
 * using this same NC profile without cacheable aliases. The dedicated linker
 * must place entry mailboxes/stacks, plan, records and tables in 0x42000000..
 * 0x42200000, preserving other reservations there. Does not start CPUs.
 * Root startup must separately install matching primary mappings/GIC/vectors
 * and an eight-core FreeRTOS port with A733 SGI affinity encoding. */
int a733_cpu_continue_prepare(uint64_t epoch,uint64_t deadline_us);
/* Primary EL3 entry after distributor setup and secondary releases. Uses the
 * current reserved boot stack, configures local GIC without hotplug accesses,
 * then enters the same diagnostic EL1 profile and calls this fixed image hook.
 * Neither function returns. The image hook initializes PPI30/tick and starts
 * the kernel; it must preserve the shared NC profile and eight-core port ABI. */
void a733_cpu_primary_continue(uint64_t epoch);
void a733_smp_primary_el1(void);
int a733_smp_pre_scheduler(unsigned long core,uint64_t epoch,uint64_t deadline_us);
void a733_smp_exit_privileged(unsigned status,uint64_t detail);
extern struct a733_cpu_continue_plan a733_cpu_continue_plan;
extern struct a733_cpu_continue_record a733_cpu_continue_records[8];
extern uint64_t a733_cpu_continue_level1[512],a733_cpu_continue_level2[512];
extern unsigned char a733_cpu_el1_stacks[8*A733_CONTINUE_STACK_BYTES];
#endif
#endif
