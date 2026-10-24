/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SUNSTONE_A733_POWER_DOMAINS_H
#define SUNSTONE_A733_POWER_DOMAINS_H
/* The power-domain owner and the PMIC service task (docs/power-domains.md,
 * docs/designs/power-domains.md).
 *
 * One owner per image is the only writer of PCK600 domains 1 (DE_SYS),
 * 4 (NPU) and 5 (GPU_TOP). Only domain 5 supports a healthy OFF request.
 * One service task per image runs a733_power_service_step(): every PMIC
 * transaction, the once-per-boot supply verification and every domain
 * transition. The task must be the PMIC provider's designated task. */
#include "a733_pmic.h"
#include "a733_power_domain_row.h"
#include "a733_range.h"

/* State-word bits set by the owner, beside those of a733_power_authority.h. */
#define A733_POWER_SUPPLY_DONE    (1u << 1)  /* supply verification ended */
#define A733_POWER_DCDC2_VERIFIED (1u << 2)  /* DCDC2 enabled at 800 mV at boot */
#define A733_POWER_DCDC4_VERIFIED (1u << 3)  /* DCDC4 enabled at 800 mV at boot */
#define A733_POWER_DOMAIN1_FAULT  (1u << 11)
#define A733_POWER_DOMAIN4_FAULT  (1u << 12)
#define A733_POWER_DOMAIN5_FAULT  (1u << 13)
_Static_assert(((A733_POWER_SUPPLY_DONE | A733_POWER_DCDC2_VERIFIED | A733_POWER_DCDC4_VERIFIED |
    A733_POWER_DOMAIN1_FAULT | A733_POWER_DOMAIN4_FAULT | A733_POWER_DOMAIN5_FAULT) &
    (A733_POWER_PROVIDER_READY | A733_POWER_PMIC_TRANSFER)) == 0, "state-word bits are disjoint");

#define A733_POWER_PCK_DOMAIN(domain) (UINT32_C(0x07060000) + UINT32_C(0x1000) * (domain))
/* Provider start to supply verification: the 1,000 us enable delay plus a
 * 1,040 mV ramp at 250 uV/us (board device tree lines 2123-2146). */
#define A733_POWER_SUPPLY_SETTLE_US 5160u
/* Verification ends within this time after the settle time has passed and
 * the provider is READY; the provider's startup deadline bounds the wait for
 * READY. */
#define A733_POWER_SUPPLY_BUDGET_US 100000u
/* Provisional spin after a PCK600 command; board evidence replaces it. */
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
    uint64_t command_at, deadline;
};
#define A733_POWER_DOMAINS 3u /* 1, 4 and 5 */
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
    /* The boot reading of DCDC4, which the GPU supply consumer copies. Valid
     * once A733_POWER_DCDC4_VERIFIED is set, which publishes it. */
    struct a733_pmic_snapshot dcdc4;
    uint64_t verify_at, verify_deadline, last_now;
    unsigned running, supply_stage, supply_verified, failed;
};

/* Before the scheduler: bind zeroed storage to the PMIC provider, whose
 * submit then calls ops.notify, and whose GPU supply consumer reads the
 * verification record. No hardware access. */
int a733_power_domains_init(struct a733_power_domains *, struct a733_pmic *,
    const struct a733_power_domain_ops *, void *context, const struct a733_power_service_config *);
/* Register a zeroed row before the service task first runs: domain 1, 4 or 5,
 * the consumer's owner identity and its rail token, which must hold the
 * domain's supply (DCDC2 for domains 1 and 4, DCDC4 for domain 5) by the time
 * of the first request. The token storage stays at this address and is not
 * changed while the row is counted. Returns 1 on success, no hardware access.
 * Inline, so that the GPU platform registers its row without linking the
 * owner. The NPU startup uses a domain-4 row and retains it at local CLOSE. */
static inline int a733_power_domain_row_init(struct a733_power_domain_row *r, struct a733_power_domains *d,
    unsigned domain, uint64_t owner, const struct a733_rail_token *rail)
{
    if (!r || r->domains || !d || d->self != d || d->running || (domain != 1 && domain != 4 && domain != 5) ||
        !owner || !rail || !a733_ranges_disjoint(r, sizeof(*r), d, sizeof(*d))) return 0;
    *r = (struct a733_power_domain_row){.domains = d, .rail = rail, .next = d->rows, .notify = d->ops.notify,
        .notify_context = d->context, .owner = owner, .domain = domain};
    atomic_init(&r->request, 0);
    atomic_init(&r->state, 0);
    d->rows = r;
    return 1;
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
 * and clocks before its row request (the NPU): MBUS gates, the PPU clock, and
 * command and status both OFF or both ON. 1 consistent, 0 refused. When state
 * is not NULL and the check passes, it receives the status read, 0 for OFF or
 * 8 for ON. Any task; no write. Only domains 1, 4 and 5. */
int a733_power_domain_check(struct a733_power_domains *, unsigned domain, unsigned *state);
#endif
