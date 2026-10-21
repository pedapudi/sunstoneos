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
    atomic_uint level; /* Latest enum a733_thermal_level; 3 until first reported. */
    struct a733_clock_token clock;
    struct a733_rail_token rail;
    struct a733_pmic_request request;
    struct a733_pmic_snapshot snapshot;
    struct a733_cpu_clock_report report;
    uint64_t last_now, deadline, timer;
    uint32_t changes, applied_mhz[2], target_mhz, from_uv, to_uv, select;
    unsigned state, after, cluster, n, p, selector, raise, thermal, failed, result, retained;
    unsigned throttle_failed; /* Cluster bits whose throttle lowering did not complete. */
};
/* Zeroed private storage once. Managers and the PMIC provider have platform
 * lifetime. The grant is copied. No hardware access. */
int a733_cpu_clock_init(struct a733_cpu_clock *,const struct a733_cpu_clock_config *,
    const struct a733_cpu_clock_ops *,void *,struct a733_clock_leases *,
    struct a733_rail_leases *,struct a733_pmic *);
/* Owner task. Admits one transition of LITTLE or BIG to an exact operating
 * point when the grant covers it, the latest thermal level permits it and no
 * transition is active. PENDING starts it; poll completes it. No access. */
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
 * the hardware state are kept and every later call returns RETAINED. While
 * idle under THROTTLE, a cluster this driver raised above the throttle
 * ceiling is lowered to it without consuming a change and without lowering
 * its voltage. If that lowering is refused or expires, it is not retried
 * until the owner requests that cluster again. Idle with nothing to do:
 * READY. */
enum a733_cpu_clock_status a733_cpu_clock_poll(struct a733_cpu_clock *);
/* Any task: the thermal policy's level. Before the first call every request
 * is refused. NORMAL admits granted points, THROTTLE admits none above the
 * ceiling, CRITICAL refuses all transitions and is terminal. Returns 1 when
 * recorded. No access and no waiting. */
int a733_cpu_clock_thermal(struct a733_cpu_clock *,enum a733_thermal_level);
#endif
