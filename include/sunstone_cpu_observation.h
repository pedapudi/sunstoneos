/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SUNSTONE_CPU_OBSERVATION_H
#define SUNSTONE_CPU_OBSERVATION_H
#include <stdint.h>
#include "sunstone_cpu_topology.h"
#define SS_CPU_OBSERVATION_CACHES 14u
/* Flags describe captured data, independently of launch admission. */
enum { SS_CPU_OBS_IDENTITY=1, SS_CPU_OBS_ATOMIC_KNOWN=2,
       SS_CPU_OBS_LSE=4, SS_CPU_OBS_CACHE_FORMAT=8 };
struct ss_cpu_cache_observation {
    uint64_t ccsidr, bytes;
    uint32_t level, kind, line_bytes, ways, sets, reserved;
};
struct ss_cpu_observation {
    uint64_t mpidr, midr, isar0, mmfr2, clidr;
    uint32_t core, flags, atomic, ccidx, cache_count, reserved;
    struct ss_cpu_cache_observation cache[SS_CPU_OBSERVATION_CACHES];
};
enum ss_cpu_observation_register {
    SS_CPU_REG_MPIDR, SS_CPU_REG_MIDR, SS_CPU_REG_ISAR0, SS_CPU_REG_MMFR2,
    SS_CPU_REG_CLIDR, SS_CPU_REG_CSSELR, SS_CPU_REG_CCSIDR
};
struct ss_cpu_observation_ops {
    uint64_t (*read)(void *, enum ss_cpu_observation_register);
    void (*select)(void *, uint64_t);
    void (*isb)(void *);
};
/* Boot owner only, before tasks. No MMIO, maintenance, clock or power writes.
 * CSSELR is restored, with ISB, even when the geometry format is unknown. */
int ss_cpu_observation_capture(unsigned, const struct ss_cpu_observation_ops *,
                               void *, struct ss_cpu_observation *);
void ss_cpu_observe_boot(unsigned core);
/* EL0-safe immutable RAM snapshot, published once by this CPU at boot. */
int ss_cpu_observation_copy(unsigned core, struct ss_cpu_observation *out);
#endif
