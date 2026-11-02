/* SPDX-License-Identifier: GPL-2.0-only */
#include "sunstone_cpu_topology.h"
#include "sunstone_board.h"
#include "a733_cpu_clock.h"
#include "a733_rail_leases.h"
/* Identity and sharing: docs/cpu-start.md, docs/cpu-coherence.md and
 * docs/designs/scheduler-partitioned.md. Geometry and maximum integer capacity:
 * port 3-1, entry 1793576407894634321-03a919d6, seven repetitions per core at
 * CPU-L 1794 MHz/1000 mV, CPU-B 2002 MHz/1050 mV, DSU 1274 MHz.
 * docs/status/cpu-topology.md records hashes, spread and workload limits.
 * Effective capacity and energy stay unknown without runtime/power evidence. */
#define PRIVATE_CACHE(i,l1,l2) \
    {UINT64_C(1)<<(i),(l1),64,1,SS_CACHE_INSTRUCTION,SS_ID_NONE,SS_ID_NONE,SS_ID_NONE,0}, \
    {UINT64_C(1)<<(i),(l1),64,1,SS_CACHE_DATA,SS_ID_NONE,SS_ID_NONE,SS_ID_NONE,0}, \
    {UINT64_C(1)<<(i),(l2),64,2,SS_CACHE_UNIFIED,SS_ID_NONE,SS_ID_NONE,SS_ID_NONE,0}
static const struct ss_cpu_cache caches[]={
    PRIVATE_CACHE(0,32768,131072),PRIVATE_CACHE(1,32768,131072),
    PRIVATE_CACHE(2,32768,131072),PRIVATE_CACHE(3,32768,131072),
    PRIVATE_CACHE(4,32768,131072),PRIVATE_CACHE(5,32768,131072),
    PRIVATE_CACHE(6,65536,262144),PRIVATE_CACHE(7,65536,262144),
    {0xff,1048576,64,3,SS_CACHE_UNIFIED,A733_CPU_DSU,A733_RAIL_DCDC5,A733_CPU_DSU,0}
};
#define CPU(i,part,domain) { .affinity=(uint64_t)(i)<<8,.siblings=UINT64_C(1)<<(i), \
    .midr=(part),.logical_id=(i),.physical_core=(i),.cluster=0, \
    .performance_domain=(domain),.l1i=3*(i),.l1d=3*(i)+1,.l2=3*(i)+2,.l3=24, \
    .capacity_max=(domain)?1024:285,.flags=SS_CPU_CAPACITY_MAX_VALID }
static const struct ss_cpu_info cpus[]={
    CPU(0,0x412fd050,0),CPU(1,0x412fd050,0),CPU(2,0x412fd050,0),
    CPU(3,0x412fd050,0),CPU(4,0x412fd050,0),CPU(5,0x412fd050,0),
    CPU(6,0x414fd0b1,1),CPU(7,0x414fd0b1,1)
};
/* CPU-L and DSU share DCDC5; DSU has an OPP-table link on the shared L3,
 * rather than a CPU performance domain with duplicate members. */
static const struct ss_cpu_domain domains[]={
    {.cpus=0x3f,.clock_id=A733_CPU_LITTLE,.rail_id=A733_RAIL_DCDC5,.opp_table_id=A733_CPU_LITTLE},
    {.cpus=0xc0,.clock_id=A733_CPU_BIG,.rail_id=A733_RAIL_DCDC3,.opp_table_id=A733_CPU_BIG}
};
static uint64_t opp_frequency_hz(unsigned table,unsigned row)
{
    unsigned count=0;
    const struct a733_cpu_opp *opps=a733_cpu_topology_opps(table,&count);
    return opps && row<count ? (uint64_t)opps[row].mhz*1000000 : 0;
}
static const struct ss_cpu_topology snapshot={
    .version=SS_TOPOLOGY_VERSION,.opp_frequency_hz=opp_frequency_hz,.cpus=cpus,.caches=caches,.domains=domains,
    .cpu_count=8,.cache_count=25,.domain_count=2,
    .calibration_epoch=UINT64_C(1793576407894634321),
    .calibration_source="docs/status/cpu-topology.md; entry 1793576407894634321-03a919d6",
    .capacity_workload="capacity_integer:4096 words x 64 rounds; top CPU-L/CPU-B/DSU points",
    .policy=&sunstone_orangepi_zero3w_cpu_policy
};
const struct ss_cpu_topology *a733_cpu_topology(void)
{ return &snapshot; }
const struct a733_cpu_opp *a733_cpu_topology_opps(unsigned domain,unsigned *count)
{
    if(domain>=A733_CPU_DOMAINS || !count) return 0;
    return a733_cpu_clock_opps((enum a733_cpu_domain)domain,count);
}
