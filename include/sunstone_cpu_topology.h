/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SUNSTONE_CPU_TOPOLOGY_H
#define SUNSTONE_CPU_TOPOLOGY_H
#include <stddef.h>
#include <stdint.h>
#define SS_TOPOLOGY_VERSION 1u
#define SS_CPU_MAX 64u
#define SS_CACHE_MAX (SS_CPU_MAX * 3u + 1u)
#define SS_DOMAIN_MAX SS_CPU_MAX
#define SS_ENERGY_POINT_MAX 256u
#define SS_ID_NONE UINT16_MAX
#define SS_MPIDR_AFFINITY_MASK UINT64_C(0xff00ffffff)
enum ss_topology_status { SS_TOPOLOGY_OK, SS_TOPOLOGY_INVALID,
    SS_TOPOLOGY_SPACE, SS_TOPOLOGY_UNCALIBRATED, SS_TOPOLOGY_OVERFLOW };
enum ss_cpu_validity { SS_CPU_CAPACITY_MAX_VALID=1u,
    SS_CPU_CAPACITY_EFFECTIVE_VALID=2u, SS_CPU_ENERGY_VALID=4u };
enum ss_cache_type { SS_CACHE_INSTRUCTION=1, SS_CACHE_DATA, SS_CACHE_UNIFIED };
struct ss_cpu_info {
    uint64_t affinity, siblings;
    uint32_t midr, capacity_max, capacity_effective, flags;
    uint16_t logical_id, physical_core, cluster, performance_domain;
    uint16_t l1i, l1d, l2, l3;
};
struct ss_cpu_cache {
    uint64_t cpus, bytes;
    uint32_t line_bytes;
    uint16_t level, type;
    uint16_t clock_id, rail_id, opp_table_id; /* SS_ID_NONE when absent. */
    uint16_t reserved;                        /* zero; names the trailing padding */
};
/* OPP IDs refer to the platform clock owner's tables. Energy remains unknown
 * until a record supplies measured power and its provenance. */
struct ss_cpu_energy_point {
    uint64_t frequency_hz;
    uint32_t power_uw, opp_id;
};
struct ss_cpu_domain {
    uint64_t cpus;
    uint16_t clock_id, rail_id, opp_table_id;
    uint16_t reserved;                        /* zero; names the padding before the pointer */
    const struct ss_cpu_energy_point *energy;
    size_t energy_count;
};
struct ss_cpu_policy {
    uint64_t admitted, reserved, service, application, irq;
};
struct ss_cpu_topology {
    uint32_t version, reserved;               /* reserved is zero; names the padding */
    uint64_t calibration_epoch, operating_epoch;
    const char *calibration_source, *capacity_workload, *energy_source;
    /* Pure lookup in the referenced clock owner table. Returns zero for an
     * unsupported table/row; energy points must match its frequency exactly. */
    uint64_t (*opp_frequency_hz)(unsigned table_id, unsigned opp_id);
    const struct ss_cpu_info *cpus;
    const struct ss_cpu_cache *caches;
    const struct ss_cpu_domain *domains;
    size_t cpu_count, cache_count, domain_count;
    const struct ss_cpu_policy *policy;
};
struct ss_work_range { uint64_t begin, end; uint32_t cpu, reserved; };
/* Pointer-free application snapshot metadata. Observed is separate from the
 * board's admitted policy mask; each application grant controls task placement.
 * Zero epochs mean capacity is not calibrated. */
struct ss_cpu_topology_info {
    uint32_t version, cpu_count, cache_count, domain_count;
    uint64_t calibration_epoch, operating_epoch;
    uint64_t admitted, reserved, irq, observed;
};
/* Validate once before publishing an immutable snapshot. Query and split take
 * only validated snapshots whose storage remains alive throughout the call. */
int ss_cpu_topology_validate(const struct ss_cpu_topology *);
int ss_cpu_from_mpidr(const struct ss_cpu_topology *, uint64_t mpidr, unsigned *cpu);
int ss_cpu_topology_query(const struct ss_cpu_topology *, struct ss_cpu_info *,
    size_t capacity, size_t *count);
int ss_cpu_work_split(const struct ss_cpu_topology *, uint64_t units,
    uint64_t mask, uint32_t quantum, struct ss_work_range *, size_t capacity, size_t *count);
const struct ss_cpu_topology *a733_cpu_topology(void);
struct a733_cpu_opp;
const struct a733_cpu_opp *a733_cpu_topology_opps(unsigned domain, unsigned *count);
#endif
