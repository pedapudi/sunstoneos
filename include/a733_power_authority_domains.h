/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SUNSTONE_A733_POWER_AUTHORITY_DOMAINS_H
#define SUNSTONE_A733_POWER_AUTHORITY_DOMAINS_H
#include "a733_pmic.h"

/* One owner for PCK600 power domains 1 (DE_SYS), 4 (NPU), 5 (GPU_TOP) and
 * 10 (VO1), and the once-per-boot verification of their supplies. The other
 * domains and the CPU cluster and core domains keep their current owners.
 *
 * Supply verification reads DCDC2, DCDC4, CLDO2 and CLDO5 once, after the
 * full-range ramp, and records the result in the power-authority state word.
 * A row request then makes one acquire load of that word and submits no PMIC
 * read. This relies on no agent writing those four outputs after boot: the
 * PMIC provider writes only DCDC3 and DCDC5, and the OS issues no PSCI
 * CPU_OFF or suspend call. Firmware writes and a second S_TWI0 master are
 * excluded only by board evidence that docs/power.md lists.
 *
 * Each consumer (a row) holds a rail token for the domain's supplies; the
 * owner pins that token while the row is counted, so the rail cannot be
 * released under a powered domain. */
enum a733_power_domain_result {
    A733_DOMAIN_READY,    /* Row counted and domain ON; or check passed; or released. */
    A733_DOMAIN_PENDING,  /* Supply verification has not finished. */
    A733_DOMAIN_BUSY,     /* Owner lock held: wait one tick and call again. */
    A733_DOMAIN_AGAIN,    /* Transition still in progress: wait one tick and call again. */
    A733_DOMAIN_INVALID,  /* Bad domain, row, token or token rails. */
    A733_DOMAIN_REFUSED,  /* Supply or domain-level read failed; nothing counted or written. */
    A733_DOMAIN_RETAINED  /* Domain FAILED or a fault bit set; everything stays held. */
};
/* PCK block base, command at +0, status at +8; the pinned sun60iw2 pck600
 * table gives every domain the same offsets and delay row. */
#define A733_PCK_BLOCK(domain) (UINT32_C(0x07060000)+UINT32_C(0x1000)*(domain))
#define A733_POWER_DOMAIN_ROWS 4u
#define A733_POWER_DOMAIN_COUNT 4u  /* Domains 1, 4, 5 and 10, in that order. */
#define A733_POWER_DOMAIN_WAIT_US 10000u
/* Provisional bound on one acknowledgement spin; board evidence replaces it
 * with the longest measured acknowledgement plus 50%. */
#define A733_POWER_DOMAIN_SPIN_US 50u
/* The full-range ramp and enable delay in the device tree. */
#define A733_POWER_SUPPLY_SETTLE_US 5160u

/* now: monotonic microseconds, called once at the start of a step. ticks: the
 * raw counter (ISB; MRS CNTPCT_EL0) for acknowledgement spins, ticks_per_us
 * of it per microsecond. read and write are plain Device accesses to the
 * CCU, R_CCU and PCK; barrier is DSB SY. */
struct a733_power_domain_ops {
    int (*now)(void *, uint64_t *);
    uint64_t (*ticks)(void *);
    int (*read)(void *, uintptr_t, uint32_t *);
    int (*write)(void *, uintptr_t, uint32_t);
    void (*barrier)(void *);
};
/* A consumer's counted row. Caller storage, zeroed before the first request,
 * kept until release returns READY. */
struct a733_power_row {
    unsigned domain;
    uint64_t id;
    struct a733_rail_token token;
};
struct a733_power_domain {
    unsigned state, rows;
    uint64_t deadline;          /* 10 ms after the last command. */
    uint64_t row_ids[A733_POWER_DOMAIN_ROWS];
    struct a733_rail_token tokens[A733_POWER_DOMAIN_ROWS];
};
struct a733_power_domains {
    struct a733_power_domains *self;
    atomic_uint busy;
    struct a733_power_domain_ops ops;
    void *context;
    struct a733_rail_leases *rails;
    uint32_t ticks_per_us;
    uint64_t next_row, last_now;
    struct a733_power_domain domain[A733_POWER_DOMAIN_COUNT];
    /* Supply verification, run by the PMIC service task. */
    uint64_t verify_owner, verify_after, verify_deadline, verify_last;
    struct a733_pmic_lifetime lifetime;
    struct a733_rail_token supply;
    struct a733_pmic_request request;
    unsigned verify_stage;
};
/* Zeroed storage once, before concurrency; no hardware access. */
int a733_power_domains_init(struct a733_power_domains *, const struct a733_power_domain_ops *,
    void *, struct a733_rail_leases *, uint32_t ticks_per_us);
/* Once per boot, PMIC service task only. Waits until verify_after (provider
 * start plus A733_POWER_SUPPLY_SETTLE_US), then snapshots DCDC2, DCDC4, CLDO2
 * and CLDO5 under one shared rail token. Each output enabled at its expected
 * voltage (800 mV, 800 mV, 1,800 mV, 1,800 mV) sets its verified bit; then
 * SUPPLY_DONE is set and the token released. PENDING until then; RETAINED if
 * the provider or ledger refuses. */
enum a733_power_domain_result a733_power_domains_verify(struct a733_power_domains *,
    struct a733_pmic *, const struct a733_pmic_lifetime *, uint64_t owner,
    uint64_t verify_after, uint64_t deadline);
/* Read-only: the PCK bus clock, the domain's MBUS gates and command/status
 * agreement, adopting the domain on first use. A consumer calls it before its
 * first reset or clock write. Writes nothing. */
enum a733_power_domain_result a733_power_domain_check(struct a733_power_domains *, unsigned domain);
/* Count a row and power the domain on. The first call passes the rail token,
 * which must hold the domain's supplies (domain 1, 4: DCDC2; 5: DCDC4; 10:
 * DCDC2 and CLDO2 or CLDO5) and is pinned while the row is counted. Later
 * calls with the same row continue a transition. Domain 4 is requested after
 * the NPU's resets, rate and clocks, the others before their clocks. */
enum a733_power_domain_result a733_power_domain_request(struct a733_power_domains *, unsigned domain,
    const struct a733_rail_token *, struct a733_power_row *);
/* Drop a row. Only domain 5 powers off, at count zero, after the GPU has
 * drained its DMA; an open GPU gate or reset refuses the power-off and keeps
 * the row. Later calls continue the power-off; READY clears the row. */
enum a733_power_domain_result a733_power_domain_release(struct a733_power_domains *, struct a733_power_row *);
/* The NPU runs only at 492 MHz, where the pinned operating-point table gives
 * 800 mV for every speed bin, so its supply selection is the verified DCDC2
 * setting: 800000 once verified, otherwise 0. */
uint32_t a733_power_npu_supply_uv(const struct a733_rail_leases *);
#endif
