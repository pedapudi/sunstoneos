/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SUNSTONE_A733_POWER_DOMAINS_H
#define SUNSTONE_A733_POWER_DOMAINS_H
/* The power-domain owner and the PMIC service task (docs/power-domains.md,
 * docs/designs/power-domains.md).
 *
 * One owner per image is the only writer of PCK600 domains 1 (DE_SYS),
 * 4 (NPU) and 5 (GPU_TOP). Domains 4 and 5 support a healthy OFF request.
 * One service task per image runs a733_power_service_step(): every PMIC
 * transaction, the once-per-boot supply verification and every domain
 * transition. The task must be the PMIC provider's designated task. */
#include "a733_pmic.h"
#include "a733_power_domain_row.h"
#include "a733_range.h"

/* State-word bits set by the owner, beside those of a733_power_authority.h. */
#define A733_POWER_SUPPLY_DONE    (1u << 1)  /* supply verification ended */
#define A733_POWER_DCDC2_VERIFIED (1u << 2)  /* DCDC2 enabled at 800 mV at boot */
#define A733_POWER_DCDC4_VERIFIED (1u << 3)  /* DCDC4 enabled at 800 or 960 mV at boot */
#define A733_POWER_DOMAIN1_FAULT  (1u << 11)
#define A733_POWER_DOMAIN4_FAULT  (1u << 12)
#define A733_POWER_DOMAIN5_FAULT  (1u << 13)
#define A733_POWER_DOMAIN2_FAULT  (1u << 14)
_Static_assert(((A733_POWER_SUPPLY_DONE | A733_POWER_DCDC2_VERIFIED | A733_POWER_DCDC4_VERIFIED |
    A733_POWER_DOMAIN1_FAULT | A733_POWER_DOMAIN2_FAULT | A733_POWER_DOMAIN4_FAULT | A733_POWER_DOMAIN5_FAULT) &
    (A733_POWER_PROVIDER_READY | A733_POWER_PMIC_TRANSFER)) == 0, "state-word bits are disjoint");

#define A733_POWER_PCK_DOMAIN(domain) (UINT32_C(0x07060000) + UINT32_C(0x1000) * (domain))
/* Provider start to supply verification: the 1,000 us enable delay plus a
 * 1,040 mV ramp at 250 uV/us (board device tree lines 2123-2146). An owner
 * marked with a733_power_domains_settled_supply() verifies at once instead. */
#define A733_POWER_SUPPLY_SETTLE_US 5160u
/* Verification ends within this time after the settle time has passed and
 * the provider is READY; the provider's startup deadline bounds the wait for
 * READY. */
#define A733_POWER_SUPPLY_BUDGET_US 100000u
/* Provisional spin after a PCK600 command. It stays until at least three
 * OFF-to-ON transitions of domain 2, or of domain 5 as its stand-in, have been
 * timed on the board with measure_transitions set (struct
 * a733_power_transition); their command-to-status times then size it. */
#define A733_POWER_PCK_SPIN_US 50u
/* Bound from pck600_domains.c lines 112-121, capped by the service deadline.
 * Target status must be observed and checked strictly before that bound. */
#define A733_POWER_PCK_BOUND_US 10000u
/* Longest PMIC byte-phase spin in one service step. */
#define A733_POWER_PMIC_SPIN_US 1000u

/* read and write perform one ordered 32-bit Device access to an owned PPU
 * block, or a read of CCU 0x02002000 or R_CCU 0x07010000; write returns after
 * the write has reached the device. notify wakes the service task and may be
 * called from any task. All return 1 for success where they return a value. */
struct a733_power_domain_ops {
    int (*read)(void *, uintptr_t, uint32_t *);
    int (*write)(void *, uintptr_t, uint32_t);
    void (*notify)(void *);
};
/* owner: rail-ledger owner of the supply-verification token. deadline: the
 * session end in the PMIC clock's microseconds; no new PCK command is admitted
 * at or after it, and pending PCK completion must precede it.
 * tick_us: one scheduler tick.
 * idle_us: the longest idle block, for images whose service reports progress
 * to a watchdog; 0 blocks until the deadline. */
struct a733_power_service_config {
    uint64_t owner, deadline;
    uint32_t tick_us, idle_us;
};
/* Domain states, service task only. */
enum { A733_POWER_DOMAIN_UNKNOWN, A733_POWER_DOMAIN_OFF, A733_POWER_DOMAIN_ON_WAIT,
       A733_POWER_DOMAIN_ON, A733_POWER_DOMAIN_OFF_WAIT, A733_POWER_DOMAIN_FAILED };
struct a733_power_domain {
    unsigned id, state, count;
    unsigned delays_ready; /* all five delay words written and read back */
    uint64_t command_at, deadline;
};
#define A733_POWER_DOMAINS 4u /* 1, 4, 5 and 2 */
struct a733_power_domains {
    struct a733_power_domains *self;
    struct a733_pmic *pmic;
    struct a733_power_domain_ops ops;
    void *context;
    struct a733_power_service_config config;
    struct a733_power_domain_row *rows;
    struct a733_power_domain domain[A733_POWER_DOMAINS];
    /* Supply verification, service task only until the done bit. */
    struct a733_rail_token supply_token;
    struct a733_pmic_request supply_request;
    /* The boot reading of DCDC4, for diagnostics. Valid once
     * A733_POWER_DCDC4_VERIFIED is set, which publishes it. The GPU platform
     * may raise DCDC4 afterwards, so its supply consumer samples it again. */
    struct a733_pmic_snapshot dcdc4;
    /* The owner's clock, in microseconds, at the step that first publishes
     * A733_POWER_DCDC2_VERIFIED; written once, before that bit, which
     * publishes it. Read it only after that bit is seen. */
    uint64_t verified_at;
    /* The owner's clock at the service's first step; written before any verification bit is published, so readable
     * once one is seen. */
    uint64_t started_at;
    uint64_t verify_at, verify_deadline, last_now;
    unsigned running, supply_stage, supply_verified, failed;
    unsigned settled_supply; /* Before the service runs only; then read-only. */
    unsigned measure_transitions; /* likewise; see a733_power_domains_measure_transitions() */
    struct a733_power_transition transition[A733_POWER_DOMAINS]; /* by slot; service task only */
};

/* Before the scheduler: bind zeroed storage to the PMIC provider, whose
 * submit then calls ops.notify, and whose GPU supply consumer reads the
 * verification record. No hardware access. */
int a733_power_domains_init(struct a733_power_domains *, struct a733_pmic *,
    const struct a733_power_domain_ops *, void *context, const struct a733_power_service_config *);
/* Before the service task first runs: verify the supplies at the service's
 * first step, with no A733_POWER_SUPPLY_SETTLE_US wait. The owner never
 * enables a rail or changes its voltage, and verification passes only for a
 * rail found enabled at its target selector. The Linux regulator core applies
 * its enable delay only to an off-to-on enable and its ramp delay only to a
 * selector change (drivers/regulator/core.c lines 2905-2930 and 3660-3713,
 * linux-orangepi 2ac08e8c). Returns 0 once the service has run. */
int a733_power_domains_settled_supply(struct a733_power_domains *);
/* Before the service task first runs, in a measurement run only (the video
 * decoder's first board stage): after each PCK600 command the service reads
 * status back to back until the target or the A733_POWER_PCK_BOUND_US bound,
 * instead of spinning for A733_POWER_PCK_SPIN_US and then reading once per
 * step, so that the transition record brackets the time to the target within
 * one read gap. The service task then holds its core for up to that bound.
 * Returns 0 once the service has run. */
int a733_power_domains_measure_transitions(struct a733_power_domains *);
/* Register a zeroed row before the service task first runs: domain 1, 2, 4 or
 * 5, the consumer's owner identity and its rail token, which must hold the
 * domain's supply (DCDC2 for domains 1, 2 and 4, DCDC4 for domain 5) by the time
 * of the first request. The token storage stays at this address and is not
 * changed while the row is counted. Returns 1 on success, no hardware access.
 * Inline, so that the GPU platform registers its row without linking the
 * owner. The NPU startup uses a domain-4 row and retains it at local CLOSE;
 * the video decoder uses a domain-2 row. */
static inline int a733_power_domain_row_init(struct a733_power_domain_row *r, struct a733_power_domains *d,
    unsigned domain, uint64_t owner, const struct a733_rail_token *rail)
{
    if (!r || r->domains || !d || d->self != d || d->running || (domain != 1 && domain != 2 && domain != 4 && domain != 5) ||
        !owner || !rail || !a733_ranges_disjoint(r, sizeof(*r), d, sizeof(*d))) return 0;
    *r = (struct a733_power_domain_row){.domains = d, .rail = rail, .next = d->rows, .notify = d->ops.notify,
        .notify_context = d->context, .owner = owner, .domain = domain};
    atomic_init(&r->request, 0);
    atomic_init(&r->state, 0);
    d->rows = r;
    return 1;
}
/* The bounds on the owner's answer to a row, for the consumer that waits on it (the video decoder, G2D and the NPU
 * startup). Both are stateless and callable from any task: they read only the owner's immutable configuration, the
 * provider's startup deadline and its clock, a733_pmic_now, never a field that the service task writes. The result is
 * on that clock, the architectural counter in microseconds: the same epoch and scale as a733_counter_clock_now and
 * sunstone_deadline_now_us, so a consumer compares it with either. Each is clipped to limit, the caller's own
 * deadline on the same clock. The caller stores the bound when its wait begins and reads the row state and the
 * authority word itself.
 *
 * Before supply verification has ended: verification starts A733_POWER_SUPPLY_SETTLE_US after the service's first
 * step at the latest, ends within A733_POWER_SUPPLY_BUDGET_US once the provider is READY, and READY comes by the
 * provider's startup deadline. The bound is anchored at that deadline or now, whichever is later, plus one tick for
 * the service's wake when it publishes the result. 0 when the clock read fails. The OS image sets the startup deadline
 * to the session deadline (apps/board/main.c), so there the anchor lies past any caller's limit and the caller's limit
 * is the supply bound: 500 ms for G2D. */
static inline uint64_t a733_power_supply_deadline(const struct a733_power_domains *d, uint64_t limit)
{
    uint64_t t;
    if (a733_pmic_now(d->pmic, &t) != 1) return 0;
    const uint64_t anchor = d->pmic->config.startup_deadline > t ? d->pmic->config.startup_deadline : t;
    const uint64_t bound = a733_deadline_after_saturating(anchor,
        (uint64_t)A733_POWER_SUPPLY_SETTLE_US + A733_POWER_SUPPLY_BUDGET_US + d->config.tick_us);
    return bound < limit ? bound : limit;
}
/* Once supply verification has ended, from now, the time the consumer first saw it ended or made its request: the
 * owner admits the row at its next step and the transition ends within A733_POWER_PCK_BOUND_US of its command. Two
 * ticks: one for the service's wake on the request, one for an admission that a busy rail ledger delays by one retry
 * (admit returns a one-tick retry when a733_rail_pin reports BUSY). They exclude preemption of a rail-ledger holder
 * and of the PMIC task, which no tick count bounds; a longer delay ends as a row timeout, and each consumer fails
 * safe on it (the decoder refuses and cancels, G2D faults before its CCU writes, the NPU startup faults). */
static inline uint64_t a733_power_row_deadline(const struct a733_power_domains *d, uint64_t now, uint64_t limit)
{
    const uint64_t bound = a733_deadline_after_saturating(now, (uint64_t)A733_POWER_PCK_BOUND_US + 2u * (uint64_t)d->config.tick_us);
    return bound < limit ? bound : limit;
}
/* Service task only. Advances provider startup, every queued PMIC request,
 * supply verification and every domain transition, and returns the ticks to
 * block in ulTaskNotifyTake(pdTRUE, ticks): 1 while PMIC work, a held I2C
 * mutex, a BUSY lock or a domain transition is pending; otherwise the ticks
 * to the next timed event or the session deadline, capped by idle_us.
 * UINT32_MAX means no timed event remains. */
uint32_t a733_power_service_step(struct a733_power_domains *);
/* Service task only: the PMIC provider is idle, no supply-verification request
 * is outstanding and every domain is ON, OFF or FAILED with no row request
 * unanswered. A park predicate requires it before parking the service. */
int a733_power_service_idle(const struct a733_power_domains *);
/* 1 once the provider is READY, -1 after a provider, clock or ledger fault,
 * 0 before. One acquire load; any task. */
int a733_power_service_ready(const struct a733_power_domains *);
/* Read-only domain-level check for a consumer that must write its own resets
 * and clocks before its row request (the NPU): the domain's MBUS gates,
 * the PPU clock, and command and status both OFF or both ON. 1 consistent,
 * 0 refused. When state is not NULL and the check passes, it receives the
 * status read, 0 for OFF or 8 for ON. Any task; no write. Domain 2's profile
 * has no gates, so its check reads the PPU clock, policy and status. */
int a733_power_domain_check(struct a733_power_domains *, unsigned domain, unsigned *state);
#endif
