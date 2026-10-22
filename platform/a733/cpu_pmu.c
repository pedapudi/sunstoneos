/* SPDX-License-Identifier: Apache-2.0 */
/* Per-core cycle and event counts around a measurement region, at EL1.
 *
 * Register sequence: Cortex-A55 TRM 100442_0200_00 sections C2.5, D5.2, D5.3
 * and D5.4; docs/designs/cpu-pmu.md lists what no pinned source proves.
 * Each core writes only its own state line, with IRQs masked, so the module
 * needs no lock, DMB or DSB. Begin's one kernel critical section, in
 * vTaskCoreAffinityGet, runs before the counted window.
 *
 * The counted window runs from begin's PMEVCNTR0_EL0 write to end's
 * PMEVCNTR0_EL0 read. Counter 0 counts INST_RETIRED, so begin zeroes it last
 * and end reads it first. Begin returns through the leaf function start, so
 * no stack frame is restored inside the window, and end checks only its
 * arguments before it reads counter 0. */
#include "FreeRTOS.h"
#include "task.h"
#include "a733_cpu_pmu.h"

#define PMCR_E (UINT64_C(1)<<0)
#define PMCR_P (UINT64_C(1)<<1)  /* Event counter reset; never written as 1. */
#define PMCR_C (UINT64_C(1)<<2)  /* Cycle counter reset; never written as 1. */
#define PMCR_D (UINT64_C(1)<<3)  /* Count every 64th cycle. */
#define PMCR_LC (UINT64_C(1)<<6) /* 64-bit cycle counter overflow. */
#define PMCR_N(v) (((v)>>11)&0x1f)
#define CYCLE_BIT (UINT64_C(1)<<31)
#define ALL_EVENTS ((UINT64_C(1)<<A733_CPU_PMU_EVENTS)-1)

_Static_assert(configNUMBER_OF_CORES <= A733_CPU_PMU_CORES, "one state line per core");

/* Written only by its own core with IRQs masked. */
struct core_state {
    uint32_t generation;
    uint8_t configured;
    uint8_t counters;
    uint8_t supported;
    uint8_t reserved;
    uint32_t midr;
    uint32_t cntfrq;
} __attribute__((aligned(64)));
_Static_assert(sizeof(struct core_state) == 64, "one line per core");
static struct core_state a733_cpu_pmu_state[A733_CPU_PMU_CORES];

#define SYSREG_READ(name) ({ uint64_t v_; __asm__ volatile("mrs %0, " #name : "=r"(v_) :: "memory"); v_; })
#define SYSREG_WRITE(name, v) __asm__ volatile("msr " #name ", %0" :: "r"((uint64_t)(v)) : "memory")
#define ZERO(name) __asm__ volatile("msr " #name ", xzr" ::: "memory")
/* Bit e of PMCEID0_EL0 for event e below 0x20, otherwise bit e - 0x20 of
 * PMCEID1_EL0, moved to bit i. */
#define IMPLEMENTED(e, i) ((unsigned)((((e) < 0x20 ? ceid0 >> (e) : ceid1 >> ((e) - 0x20)) & 1) << (i)))

static inline void irq_restore(uint64_t daif)
{
    __asm__ volatile("msr daif, %0" :: "r"(daif) : "memory");
}

/* Runs with IRQs masked when the core's configured flag is clear or
 * PMCR_EL0.E reads 0. Each written register is read back once. */
static __attribute__((noinline)) int configure(struct core_state *s, uint64_t pmcr)
{
    unsigned n = PMCR_N(pmcr);
    if (!n) return A733_CPU_PMU_NO_COUNTERS;
    if (n > A733_CPU_PMU_EVENTS) n = A733_CPU_PMU_EVENTS;
    const uint64_t ceid0 = SYSREG_READ(pmceid0_el0), ceid1 = SYSREG_READ(pmceid1_el0);
    const uint64_t used = (UINT64_C(1) << n) - 1, enable = used | CYCLE_BIT;
    const unsigned supported = (IMPLEMENTED(0x08, 0) | IMPLEMENTED(0x03, 1) | IMPLEMENTED(0x17, 2) |
                                IMPLEMENTED(0x19, 3) | IMPLEMENTED(0x34, 4) | IMPLEMENTED(0x35, 5)) & (unsigned)used;
    const uint64_t intr = ALL_EVENTS | CYCLE_BIT;
    const uint64_t control = (pmcr | PMCR_E | PMCR_LC) & ~(PMCR_P | PMCR_C | PMCR_D);
    /* All filter bits zero: count at EL0 and EL1, not at EL2. */
    switch (n) {
    case 6: SYSREG_WRITE(pmevtyper5_el0, 0x35); /* fall through */
    case 5: SYSREG_WRITE(pmevtyper4_el0, 0x34); /* fall through */
    case 4: SYSREG_WRITE(pmevtyper3_el0, 0x19); /* fall through */
    case 3: SYSREG_WRITE(pmevtyper2_el0, 0x17); /* fall through */
    case 2: SYSREG_WRITE(pmevtyper1_el0, 0x03); /* fall through */
    default: SYSREG_WRITE(pmevtyper0_el0, 0x08);
    }
    ZERO(pmccfiltr_el0);
    SYSREG_WRITE(pmintenclr_el1, intr);
    ZERO(pmuserenr_el0);
    SYSREG_WRITE(pmcntenset_el0, enable);
    SYSREG_WRITE(pmcr_el0, control);
    __asm__ volatile("isb" ::: "memory");
    int ok = 1;
    switch (n) {
    case 6: ok &= SYSREG_READ(pmevtyper5_el0) == 0x35; /* fall through */
    case 5: ok &= SYSREG_READ(pmevtyper4_el0) == 0x34; /* fall through */
    case 4: ok &= SYSREG_READ(pmevtyper3_el0) == 0x19; /* fall through */
    case 3: ok &= SYSREG_READ(pmevtyper2_el0) == 0x17; /* fall through */
    case 2: ok &= SYSREG_READ(pmevtyper1_el0) == 0x03; /* fall through */
    default: ok &= SYSREG_READ(pmevtyper0_el0) == 0x08;
    }
    ok &= SYSREG_READ(pmccfiltr_el0) == 0;
    ok &= (SYSREG_READ(pmintenset_el1) & intr) == 0;
    ok &= (SYSREG_READ(pmuserenr_el0) & 0xf) == 0;
    ok &= (SYSREG_READ(pmcntenset_el0) & enable) == enable;
    ok &= (SYSREG_READ(pmcr_el0) & (PMCR_E | PMCR_D | PMCR_LC)) == (PMCR_E | PMCR_LC);
    if (!ok) return A733_CPU_PMU_CONFIG_MISMATCH;
    s->counters = (uint8_t)n;
    s->supported = (uint8_t)supported;
    s->midr = (uint32_t)SYSREG_READ(midr_el1);
    s->cntfrq = (uint32_t)SYSREG_READ(cntfrq_el0);
    s->configured = 1;
    return A733_CPU_PMU_OK;
}

/* Zero the event counters, counter 0 last, and store the starting counts.
 * Begin tail-calls this leaf, so begin's frame is gone before the counted
 * window; a tail call needs begin's signature, so the DAIF value to restore
 * arrives in region->ticks. */
static __attribute__((noinline)) int start(struct a733_cpu_pmu_region *region)
{
    const unsigned n = a733_cpu_pmu_state[region->core].counters;
    const uint64_t used = (UINT64_C(1) << n) - 1, daif = region->ticks;
    uint64_t cycles, ticks;
    switch (n) {
    case 6: ZERO(pmevcntr5_el0); /* fall through */
    case 5: ZERO(pmevcntr4_el0); /* fall through */
    case 4: ZERO(pmevcntr3_el0); /* fall through */
    case 3: ZERO(pmevcntr2_el0); /* fall through */
    case 2: ZERO(pmevcntr1_el0); /* fall through */
    default: break;
    }
    __asm__ volatile("msr pmevcntr0_el0, xzr\n msr pmovsclr_el0, %2\n isb\n"
                     "mrs %0, pmccntr_el0\n mrs %1, cntpct_el0\n msr daif, %3"
                     : "=&r"(cycles), "=&r"(ticks) : "r"(used), "r"(daif) : "memory");
    region->cycles = cycles;
    region->ticks = ticks;
    return A733_CPU_PMU_OK;
}

int a733_cpu_pmu_begin(struct a733_cpu_pmu_region *region)
{
    if (!region) return A733_CPU_PMU_BAD_ARGUMENT;
    region->core = A733_CPU_PMU_NO_CORE;
    /* The scheduler runs a task whose mask holds one core on that core only. */
    const UBaseType_t affinity = vTaskCoreAffinityGet(NULL);
    uint64_t daif, core, pmcr;
    __asm__ volatile("mrs %0, daif\n msr daifset, #2\n mrs %1, tpidr_el1\n mrs %2, pmcr_el0"
                     : "=&r"(daif), "=&r"(core), "=&r"(pmcr) :: "memory");
    if (core >= configNUMBER_OF_CORES || affinity != ((UBaseType_t)1 << core)) {
        irq_restore(daif);
        return A733_CPU_PMU_NOT_PINNED;
    }
    struct core_state *s = &a733_cpu_pmu_state[core];
    if (!s->configured || !(pmcr & PMCR_E)) {
        const int status = configure(s, pmcr);
        if (status != A733_CPU_PMU_OK) {
            irq_restore(daif);
            return status;
        }
    }
    region->generation = ++s->generation;
    region->core = (uint8_t)core;
    region->ticks = daif;
    __attribute__((musttail)) return start(region);
}

/* Every field is stored here, so a refusal leaves no stale bytes and the
 * compiler emits no memset call. */
static void write_record(struct a733_cpu_pmu_record *record, unsigned core, unsigned counters,
                         unsigned supported, unsigned overflow, int status, const struct core_state *s,
                         uint64_t cycles, uint64_t ticks, const uint64_t *v)
{
    const unsigned valid = supported & ~overflow;
    record->format = A733_CPU_PMU_FORMAT;
    record->core = (uint8_t)core;
    record->counters = (uint8_t)counters;
    record->supported = (uint8_t)supported;
    record->overflow = (uint8_t)overflow;
    record->status = (uint8_t)status;
    record->reserved0 = 0;
    record->midr = s ? s->midr : 0;
    record->cntfrq = s ? s->cntfrq : 0;
    record->cycles = cycles;
    record->ticks = ticks;
    for (unsigned i = 0; i < A733_CPU_PMU_EVENTS; ++i)
        record->events[i] = (valid >> i) & 1 ? (uint32_t)v[i] : 0;
    record->reserved1 = 0;
}

static int refuse(struct a733_cpu_pmu_record *record, unsigned core, int status)
{
    write_record(record, core, 0, 0, 0, status, NULL, 0, 0, NULL);
    return status;
}

int a733_cpu_pmu_end(const struct a733_cpu_pmu_region *region, struct a733_cpu_pmu_record *record)
{
    if (!record) return A733_CPU_PMU_BAD_ARGUMENT;
    if (!region) return refuse(record, A733_CPU_PMU_NO_CORE, A733_CPU_PMU_BAD_ARGUMENT);
    /* Begin refuses before any counter access, also where PMCR_EL0.N reads 0
     * and PMEVCNTR0_EL0 does not exist. */
    if (region->core == A733_CPU_PMU_NO_CORE) return refuse(record, A733_CPU_PMU_NO_CORE, A733_CPU_PMU_NOT_BEGUN);
    uint64_t daif, v[A733_CPU_PMU_EVENTS] = {0}, cycles, ticks, core;
    /* Counter 0 exists on every core where begin succeeds (N >= 1), and the
     * continuation programs every core alike, so it is read before the core
     * check. The other counters are read only on the beginning core. */
    __asm__ volatile("mrs %0, daif\n msr daifset, #2\n isb\n mrs %1, pmevcntr0_el0\n"
                     "mrs %2, pmccntr_el0\n mrs %3, cntpct_el0\n mrs %4, tpidr_el1"
                     : "=&r"(daif), "=&r"(v[0]), "=&r"(cycles), "=&r"(ticks), "=&r"(core) :: "memory");
    if (core != region->core) {
        irq_restore(daif);
        return refuse(record, (unsigned)core, A733_CPU_PMU_CORE_CHANGED);
    }
    const struct core_state *s = &a733_cpu_pmu_state[core];
    const unsigned n = s->counters;
    switch (n) {
    case 6: v[5] = SYSREG_READ(pmevcntr5_el0); /* fall through */
    case 5: v[4] = SYSREG_READ(pmevcntr4_el0); /* fall through */
    case 4: v[3] = SYSREG_READ(pmevcntr3_el0); /* fall through */
    case 3: v[2] = SYSREG_READ(pmevcntr2_el0); /* fall through */
    case 2: v[1] = SYSREG_READ(pmevcntr1_el0); /* fall through */
    default: break;
    }
    const uint64_t overflow = SYSREG_READ(pmovsset_el0) & ((UINT64_C(1) << n) - 1);
    const uint64_t pmcr = SYSREG_READ(pmcr_el0);
    irq_restore(daif);
    if (s->generation != region->generation) return refuse(record, (unsigned)core, A733_CPU_PMU_GENERATION_CHANGED);
    if (!(pmcr & PMCR_E)) return refuse(record, (unsigned)core, A733_CPU_PMU_DISABLED);
    write_record(record, (unsigned)core, n, s->supported, (unsigned)overflow, A733_CPU_PMU_OK, s,
                 cycles - region->cycles, ticks - region->ticks, v);
    return A733_CPU_PMU_OK;
}
