/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef SUNSTONE_A733_CPU_COHERENCE_H
#define SUNSTONE_A733_CPU_COHERENCE_H
#include <stddef.h>
#include <stdint.h>

/* Cold-entry primitives used by the dedicated cached eight-core diagnostic. */
#define A733_CPU_COHERENCE_MAIR UINT64_C(0x44ff00)
#define A733_CPU_COHERENCE_TCR UINT64_C(0x200803019)
#define A733_CPU_COHERENCE_TCR_EL3 UINT64_C(0x80823019)
#define A733_CPU_COHERENCE_MONITOR UINT64_C(0x42000000)
#define A733_CPU_COHERENCE_MONITOR_END UINT64_C(0x42200000)
#define A733_CPU_COHERENCE_AT 1u
#define A733_CPU_COHERENCE_BHB 2u
#define A733_CPU_COHERENCE_POWER_DSB 4u
#define A733_CPU_COHERENCE_TLBI 8u

struct a733_cpu_coherence_identity {
    uint64_t midr, mpidr, clidr, mmfr0, clusteridr, clustercfr;
};
struct a733_cpu_coherence_plan {
    uint64_t actlr, actlr2, actlr3, ectlr, clusteractlr;
    uint32_t requirements; /* Required integration hooks; never a proof bitset. */
    uint8_t core, revision, patch; /* patch: 0 none, 1 A55, 2 A76. */
};
/* Pure planner, atomic refusal. Supported A55 r0p0/r0p1/r1p0/r2p0;
 * A76 r0p0/r1p0/r2p0/r3p0/r3p1/r4p0/r4p1. All other revisions refuse.
 * CLUSTERIDR low byte is the DSU revision; no board identity is inferred.
 */
int a733_cpu_coherence_plan(struct a733_cpu_coherence_plan *,
                            const struct a733_cpu_coherence_identity *, unsigned core);
/* Immutable identity profile: MMIO [0,1GiB) Device; RAM [1GiB,0x48000000)
 * WBWA Inner Shareable except the privileged Normal NC monitor/table arena.
 * Code [0x41000000,0x41200000) RO executable; other RAM RW XN.
 * Table walks and monitor accesses are NC in both translated regimes.
 * el is 1 or 3; EL3 RAM descriptors select nonsecure physical addresses,
 * matching nonsecure EL1, with EL3-specific AP/execute-never fields.
 * CPU arrays must be disjoint/aligned 4KiB; L2 physical belongs to monitor.
 */
int a733_cpu_coherence_tables(uint64_t level1[512], uint64_t level2[512], uint64_t l2pa, unsigned el);

enum a733_cpu_coherence_register {
    A733_COH_CURRENT_EL, A733_COH_SCTLR3, A733_COH_SCTLR1, A733_COH_DAIF,
    A733_COH_MIDR, A733_COH_MPIDR, A733_COH_CLIDR, A733_COH_MMFR0,
    A733_COH_CLUSTERIDR, A733_COH_CLUSTERCFR,
    A733_COH_ACTLR, A733_COH_ACTLR2, A733_COH_ACTLR3, A733_COH_ECTLR,
    A733_COH_CLUSTERACTLR, A733_COH_PATCH_SELECT, A733_COH_PATCH_OP,
    A733_COH_PATCH_MASK, A733_COH_PATCH_CONTROL, A733_COH_REGISTERS
};
struct a733_cpu_coherence_ops {
    int (*now_us)(void *, uint64_t *);
    int (*read)(void *, unsigned reg, uint64_t *);
    /* Return 1 only after the write and ISB complete; -1 is uncertain. */
    int (*write)(void *, unsigned reg, uint64_t);
};
enum a733_cpu_coherence_state { A733_COH_NEW, A733_COH_IDENTIFY,
    A733_COH_CONTROL, A733_COH_PATCH, A733_COH_PREPARED, A733_COH_RETAINED };
struct a733_cpu_coherence {
    struct a733_cpu_coherence_ops ops;
    struct a733_cpu_coherence_identity identity;
    struct a733_cpu_coherence_plan plan;
    void *context;
    uint64_t token, deadline, now, value;
    unsigned core, index, phase, error;
    enum a733_cpu_coherence_state state;
};
/* Caller borrows an exclusive cold-boot ownership token, all CPUs parked with
 * caches/translation off and exceptions masked. Core0 alone owns DSU writes;
 * complete core0 preparation before other cores. All CPUs must report matching
 * DSU identity to the boot coordinator. No task, DMA or foreign firmware may
 * touch reservations. Dirty aliases must already have been cleaned/removed;
 * reset-state cache invalidity must be established independently.
 * Fresh zeroed storage, one serialized privileged caller, no callback reentry.
 * Every poll has at most one register operation and pre/post deadline checks.
 * PREPARED means reset controls were applied, not cache coherence qualified.
 * Retain token, cores, memory and recovery ownership in every terminal state.
 */
int a733_cpu_coherence_init(struct a733_cpu_coherence *,
    const struct a733_cpu_coherence_ops *, void *, unsigned core,
    uint64_t token, uint64_t deadline_us);
int a733_cpu_coherence_poll(struct a733_cpu_coherence *);

#ifdef __aarch64__
extern const struct a733_cpu_coherence_ops a733_cpu_coherence_native_ops;
/* Stackless cache/TLB primitives, DAIF masked. Return 1 success, -1 refusal or
 * deadline. Absolute deadlines use CNTPCT ticks; caller must establish a stable
 * counter, checked monotonic externally. No instruction can recover a bus hang.
 * EL3 publication accepts owned physical DRAM [0x40000000,0x48000000), after
 * dirty aliases have been resolved. Performs CVAC + DSB SY, never invalidates
 * possibly dirty data. Clean code/data/table ranges before exposing to peers.
 */
int a733_cpu_coherence_publish(uint64_t base, uint64_t bytes, uint64_t deadline_ticks);
/* EL1, M/C/I all zero; immutable canonical tables physically published; every
 * core already prepared and same profile selected. TTBR is a 4KiB monitor page.
 * Stack/code/vectors/return address must reside in mapped admitted intervals.
 * Invalidate local I-cache and TLB, install profile, enable M/C/I, read back.
 * After any attempted enable, failure retains ownership; never turn caches off
 * or reuse memory automatically. Exceptions require separately integrated
 * speculative-AT/BHB hooks. This helper alone is not production readiness.
 */
int a733_cpu_coherence_enable_el1(uint64_t ttbr, uint64_t deadline_ticks);
/* Equivalent cold EL3 transition using the separately built EL3 tables.
 * All participants must establish this translated profile before concurrent
 * cached operation. EL3 accesses MMIO securely and shared RAM nonsecurely. */
int a733_cpu_coherence_enable_el3(uint64_t ttbr, uint64_t deadline_ticks);
#endif
#endif
