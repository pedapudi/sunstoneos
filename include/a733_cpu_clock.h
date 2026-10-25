#ifndef SUNSTONE_A733_CPU_CLOCK_H
#define SUNSTONE_A733_CPU_CLOCK_H
#include "a733_pmic.h"
#include "a733_thermal.h"

/* CPU cluster clock and voltage control with fixed operating points.
 * Sources: linux-orangepi 2ac08e8c ccu-sun60iw2-cpupll.c/ccu_nkmp.c (PLL and
 * mux fields), sun60iw2p1-cpu-vf.dtsi V0.9 (operating points and bin map),
 * sun60iw2p1.dtsi sid@3006000 (dvfs_ori/dvfs_bak), the board DTS (DCDC5
 * supplies cpu0 and the DSU, DCDC3 supplies cpu6, ramp 250 uV/us) and
 * u-boot b791be84 pmu_axp8191.c (DCDC3/DCDC5 selectors). docs/cpu-clock.md. */
enum a733_cpu_domain { A733_CPU_LITTLE, A733_CPU_BIG, A733_CPU_DSU, A733_CPU_DOMAINS };
#define A733_CPU_VF_BINS 9u /* Columns vf0000,0100,0200,0300,0400,0500,0600,0401,0501. */
#define A733_CPU_VF_ENVELOPE A733_CPU_VF_BINS /* Code 0: highest voltage of every column. */
struct a733_cpu_opp { uint32_t mhz; uint16_t mv[A733_CPU_VF_BINS]; };
/* The 26 MHz-reference rows (opp-microvolt-26m-vf*) of one domain's table,
 * ascending. A zero voltage marks a point the bin does not support. */
const struct a733_cpu_opp *a733_cpu_clock_opps(enum a733_cpu_domain,unsigned *count);
/* Voltage of one row for a bin column or the envelope; 0 if unsupported. */
uint32_t a733_cpu_clock_opp_mv(const struct a733_cpu_opp *,unsigned bin);
/* SID word 0x0300624c: code = bits23:16 (dvfs_bak) if nonzero, else bits15:8
 * (dvfs_ori), mapped through vf_mapping_table. Code 0 selects the envelope
 * because an unprogrammed fuse also reads 0. Codes without a column in the
 * tables (0x11 -> vf0101, 0x12 -> vf0201) and unlisted codes return 0. */
int a733_cpu_clock_bin(uint32_t sid_word,unsigned *bin);

/* Read-only decode of one PLL and its cluster mux/P word. Source 0-2 is the
 * DCXO; source 3 is the domain PLL, rate DCXO*N/2^P with N=bits15:8 and
 * P=select bits17:16, when PLL bits31:27 (enable, LDO, lock enable, lock,
 * output) are set. Sources 4 and 5 (PERI0 2X, PLL_CPU_BACK) stay undecoded.
 * A decoded rate is configured, not measured. */
struct a733_cpu_clock_domain { uint32_t pll, select, n, p, source; uint64_t hz; unsigned decoded; };
struct a733_cpu_clock_report {
    uint32_t oscillator, dcxo_hz, sid_word;
    unsigned bin, binned;
    struct a733_cpu_clock_domain domain[A733_CPU_DOMAINS];
};
void a733_cpu_clock_decode(uint32_t dcxo_hz,uint32_t pll,uint32_t select,struct a733_cpu_clock_domain *);
/* Eight word reads: oscillator, SID word, then each PLL and mux/P word. Any
 * caller with read access may use it, for example to name the clock in a
 * timing record. Returns 1 when every read succeeded. */
int a733_cpu_clock_report(int (*read)(void *,uintptr_t,uint32_t *),void *,struct a733_cpu_clock_report *);
/* Native access policy: those eight reads, reads of the CPU-L and CPU-B
 * pattern (PLL+0x4, +0x8) and LFM (PLL+0x18) words, and writes only to the
 * CPU-L and CPU-B PLL and mux/P words. The DSU and PLL_CPU_BACK are never
 * written. */
int a733_cpu_clock_address(uintptr_t,int write);

/* Physical use needs the user's approval for each change. A supervised
 * session states that approval here; no current image supplies a grant, so
 * every request is refused. Each requested transition consumes one change. */
struct a733_cpu_clock_grant {
    uint64_t session;            /* Zero refuses every transition. */
    uint32_t clusters;           /* Bit per admitted enum a733_cpu_domain (LITTLE, BIG). */
    uint32_t max_mhz[2];         /* Highest admitted operating point per cluster. */
    uint32_t changes;            /* Requested transitions admitted. */
};
struct a733_cpu_clock_config { uint64_t owner; struct a733_cpu_clock_grant grant; };

/* Little-endian grant record that a sealed OS session carries. A record that
 * grants nothing has zero epoch, generation, clusters, maxima, changes and
 * approval. A grant names the carrying board session's epoch and generation,
 * admitted clusters, a table point as each admitted cluster's maximum (zero
 * for the others), 1 to 64 changes and the SHA-256 of the reviewed record of
 * the user's approval for those changes. The hash identifies that record; it
 * does not establish the approval. */
#define A733_CPU_CLOCK_GRANT_MAGIC UINT64_C(0x4153373343505531)
#define A733_CPU_CLOCK_GRANT_VERSION 1u
#define A733_CPU_CLOCK_GRANT_CHANGES 64u
struct a733_cpu_clock_grant_record {
    uint64_t magic,version,bytes;      /* Magic, version 1, 96. */
    uint64_t owner;                    /* Clock and rail lease owner; nonzero. */
    uint64_t epoch,generation;         /* The carrying board session. */
    uint32_t clusters,max_mhz[2],changes;
    uint8_t approval[32];
};
_Static_assert(sizeof(struct a733_cpu_clock_grant_record)==96,"CPU clock grant record geometry");
/* Pure check of a record against the carrying session's nonzero epoch and
 * generation. Returns 1 and fills the configuration, with the epoch as the
 * grant session, or 0 without changing it. */
int a733_cpu_clock_grant_load(const struct a733_cpu_clock_grant_record *,uint64_t epoch,
    uint64_t generation,struct a733_cpu_clock_config *);

struct a733_cpu_clock_ops {
    int (*now)(void *,uint64_t *); /* Monotonic microseconds shared with the leases. */
    int (*read)(void *,uintptr_t,uint32_t *);
    int (*write)(void *,uintptr_t,uint32_t);
};
enum a733_cpu_clock_status {
    A733_CPU_CLOCK_READY, A733_CPU_CLOCK_PENDING, A733_CPU_CLOCK_COMPLETE, A733_CPU_CLOCK_BUSY,
    A733_CPU_CLOCK_REFUSED, A733_CPU_CLOCK_INVALID, A733_CPU_CLOCK_EXPIRED, A733_CPU_CLOCK_RETAINED
};
#define A733_CPU_CLOCK_THROTTLE_MHZ 1014u /* Ceiling under THROTTLE; 800 mV in every bin. */
struct a733_cpu_clock {
    struct a733_cpu_clock *self;
    struct a733_cpu_clock_config config;
    struct a733_cpu_clock_ops ops;
    void *cookie;
    struct a733_clock_leases *clocks;
    struct a733_rail_leases *rails;
    struct a733_pmic *pmic;
    const struct a733_thermal_policy *policy;
    struct a733_clock_token clock;
    struct a733_rail_token rail;
    struct a733_pmic_request request;
    struct a733_pmic_snapshot snapshot;
    struct a733_cpu_clock_report report;
    uint64_t last_now, deadline, timer;
    uint32_t changes, applied_mhz[2], restore_mhz[2], target_mhz, from_uv, to_uv, select;
    unsigned state, after, cluster, n, p, selector, raise, thermal, failed, result, retained, critical;
    unsigned throttled;       /* Cluster bits lowered by THROTTLE, to restore at NORMAL. */
    unsigned throttle_failed; /* Cluster bits whose throttle lowering did not complete. */
    unsigned inherited;       /* Inspected cluster bits; thermal changes keep voltage and require grants. */
    unsigned inspect_only;    /* The active transition only reads (a733_cpu_clock_inspect). */
    unsigned spend;           /* The active transition consumes a change at its first clock write. */
    uint32_t rail_uv[2];      /* Last read or confirmed voltage of DCDC5 and DCDC3; 0 unread. */
    uint32_t words[2][3];     /* Last pattern 0, pattern 1 and LFM words read per cluster. */
    uint64_t park_at, parked_max_us; /* Park write time; longest park write to select readback, us. */
};
/* Zeroed private storage once. Managers, the PMIC provider and the thermal
 * policy have platform lifetime; the policy must already be initialized. The
 * grant is copied. No hardware access. */
int a733_cpu_clock_init(struct a733_cpu_clock *,const struct a733_cpu_clock_config *,
    const struct a733_cpu_clock_ops *,void *,struct a733_clock_leases *,
    struct a733_rail_leases *,struct a733_pmic *,const struct a733_thermal_policy *);
/* Owner task. Admits one transition of LITTLE or BIG to an exact operating
 * point when the grant covers it, the thermal policy permits it and no
 * transition is active. The policy permits a point once the cluster's sensor
 * (CPUL for LITTLE, CPUB for BIG) has had a valid reading: any granted point
 * at NORMAL, none above the ceiling at THROTTLE and none at CRITICAL, which
 * this driver keeps once seen. PENDING starts it; poll completes it. No
 * access. */
enum a733_cpu_clock_status a733_cpu_clock_request(struct a733_cpu_clock *,
    enum a733_cpu_domain,uint32_t mhz,uint64_t deadline);
/* Owner task, one bounded step per call, no busy waits. Exclusive cluster
 * clock lease (LITTLE also holds the DSU rate shared) and exclusive rail
 * lease, then inspection and a PMIC snapshot. Inspection refuses a PLL with
 * its pattern or LFM enables set. A raise writes voltage, waits the ramp,
 * then changes frequency; a lowering changes frequency, then voltage.
 * Frequency changes park the mux on the DCXO, write N with the update bit,
 * wait for update (10 ms) and lock (70 ms), set P and select the PLL again.
 * The PLL is touched only when the remaining deadline covers both waits plus
 * a 10 ms margin. COMPLETE, REFUSED or EXPIRED end a transition and release
 * both leases. REFUSED occurs before any write, including for a disabled or
 * undecodable rail snapshot; EXPIRED before the PLL is touched, leaving at
 * most a confirmed voltage that covers the present rate. A fault, including
 * a failed register read or PMIC provider fault before any write, or expiry
 * once the PLL is touched, returns RETAINED: the leases, the PMIC request and
 * the hardware state are kept and every later call returns RETAINED.
 * While idle the thermal level selects operating points. Under THROTTLE, a
 * cluster this driver raised above the ceiling is lowered to it without
 * consuming a change and without lowering its voltage. An inspected inherited
 * point needs a grant covering the ceiling and consumes one change when its
 * first clock write begins; a lowering refused or expired before then consumes
 * none. Its
 * source point, bin and voltage are rechecked; thermal transitions of inherited
 * points never write voltage. A lowering that is refused or expires is not
 * retried in the same THROTTLE episode. At NORMAL,
 * a cluster that THROTTLE lowered returns to its earlier point once, which
 * needs the grant to cover that point and consumes a change; otherwise it
 * stays at the ceiling. Idle with
 * nothing to do: READY. */
enum a733_cpu_clock_status a733_cpu_clock_poll(struct a733_cpu_clock *);
/* Owner task. A read-only pass over one cluster that needs no grant: the
 * leases, the inspection reads and one PMIC snapshot, whose voltage it keeps
 * in rail_uv and which records the rail's boot floor. A supported source point
 * with adequate CPU/DSU voltage becomes eligible for granted thermal lowering;
 * other inherited rates remain reportable without becoming eligible. It writes no clock or
 * PMIC register and consumes no change. PENDING starts it; poll completes it
 * with COMPLETE, or REFUSED for a disabled or undecodable rail. */
enum a733_cpu_clock_status a733_cpu_clock_inspect(struct a733_cpu_clock *,enum a733_cpu_domain,uint64_t deadline);
/* 1 while a frequency change holds the cluster on the oscillator: from the
 * park write to the select readback. Each mux write is followed by a 2 us
 * settle spin on the time source before the next register write. */
int a733_cpu_clock_parked(const struct a733_cpu_clock *);
/* Owner task: one poll, then further polls while the cluster stays parked
 * and the driver's time has advanced less than budget_us since the first.
 * A time source that stops advancing ends in RETAINED after a bounded number
 * of polls. */
enum a733_cpu_clock_status a733_cpu_clock_burst(struct a733_cpu_clock *,uint64_t budget_us);

/* Cycle timing on one core. A sample holds MIDR_EL1, MPIDR_EL1, PMCCNTR_EL0
 * and CNTPCT_EL0 read together. Both samples must name the same A733 core:
 * MPIDR Aff1 is the core, cores 0-5 are Cortex-A55 (LITTLE) and 6-7 are
 * Cortex-A76 (BIG). The cycle counter stops while the core waits for an
 * interrupt, so the measured rate is a lower bound that equals the clock only
 * while the core stayed busy. The configured rate is the report's decoded
 * rate for that cluster, zero without a report or decode. */
struct a733_cpu_cycle_sample { uint64_t midr, mpidr, cycles, ticks; };
struct a733_cpu_timing {
    unsigned core, cluster;
    uint64_t cycles, ticks, measured_hz, configured_hz;
};
/* Pure. Needs counter ticks that advance, at most UINT64_MAX/counter_hz of
 * them, and cycles that do not go back. Returns 1 when filled. */
int a733_cpu_clock_timing(const struct a733_cpu_cycle_sample *begin,const struct a733_cpu_cycle_sample *end,
    uint32_t counter_hz,const struct a733_cpu_clock_report *,struct a733_cpu_timing *);
/* Native, EL1 on the core to be timed (a733_cpu_cycles_native.c). Enable sets
 * PMCR_EL0.E and LC, clears D, writes PMCCFILTR_EL0 zero (count at EL0, EL1
 * and EL3, not EL2) and enables the cycle counter; it never resets a
 * counter. Sample masks IRQs for its reads and refuses a cycle counter that
 * is not configured that way. Both return 1 on success. */
int a733_cpu_cycles_enable(void);
int a733_cpu_cycles_sample(struct a733_cpu_cycle_sample *);
#endif
