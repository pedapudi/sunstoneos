/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SUNSTONE_A733_POWER_DOMAIN_ROW_H
#define SUNSTONE_A733_POWER_DOMAIN_ROW_H
/* One consumer's claim on a PCK600 power domain (docs/power-domains.md).
 *
 * This header holds only the row and its consumer calls, so that consumer
 * headers such as a733_gpu_power.h can embed a row without including the PMIC
 * provider. The owner and its service task are in a733_power_domains.h. */
#include <stdatomic.h>
#include <stdint.h>

struct a733_power_domains;
struct a733_rail_token;

enum a733_power_row_state {
    A733_POWER_ROW_IDLE,    /* never requested */
    A733_POWER_ROW_PENDING, /* the service has not answered the latest request */
    A733_POWER_ROW_ON,      /* admitted; the domain is ON */
    A733_POWER_ROW_OFF,     /* released; the claim and rail pin are dropped */
    A733_POWER_ROW_REFUSED, /* no transition; an existing ON claim stays retained */
    A733_POWER_ROW_FAILED   /* the domain failed; the row and its rail pin are retained */
};

/* Consumer storage with platform lifetime. Each field has one writer: the
 * consumer writes request, the service writes state and the service-only
 * fields. request is (generation << 1) | on; state is
 * (answered generation << 3) | enum a733_power_row_state. */
/* The latest admission and PCK600 transition of one domain, service task
 * only; it adds no register access. It exists to size A733_POWER_PCK_SPIN_US
 * from board data. It, a733_power_domains_measure_transitions() and their
 * model cases are deleted once that is done, at the latest on 2026-12-14. The
 * samples are OFF-to-ON transitions of domain 2, at most one per board power
 * cycle since domain 2 stays ON across the decoder's close, or of domain 5,
 * which stands in for it: a GPU image powers domain 5 on and off within one
 * power lifetime. The 50 us spin stays as it is until three samples have been
 * timed.
 *
 * The owner fails a transition at any status read whose bits 3:0 are neither
 * the starting value nor the target, so a successful one reads the starting
 * value reads - 1 times and then the target once; all_and and all_or show
 * whether any other bit (bit 8, dynamic enable in Arm's PPU layout, among
 * them) changed in any read. Times are the service clock just before each
 * read; command_at is taken after the policy write completed. */
struct a733_power_transition {
    uint32_t policy_before, status_before; /* full words at the latest admission or release */
    uint32_t policy_written, first_word, pending_word, reached_word, all_and, all_or;
    uint32_t reads, spin_reads, commands;
    uint64_t command_at, first_at, pending_at, reached_at;
};
struct a733_power_domain_row {
    struct a733_power_domains *domains;
    const struct a733_rail_token *rail;
    struct a733_power_domain_row *next;
    void (*notify)(void *); /* the owner's service wake, copied at init */
    void *notify_context;
    uint64_t owner;
    unsigned domain;
    unsigned counted; /* service task only: admitted, its rail pin held */
    atomic_uint request;
    atomic_uint state;
    /* Optional consumer wake, set by the consumer before the service first
     * runs: the service calls it after publishing an answer to this row and
     * after publishing the supply-verification result. */
    void (*wake)(void *);
    void *wake_context;
    /* The owner's transition record of this row's domain, copied by the
     * service before each answer's release store. A consumer reads it after
     * a733_power_domain_state() returns that answer (acquire order), until it
     * makes its next request. */
    struct a733_power_transition transition;
};

/* a733_power_domain_row_init() in a733_power_domains.h registers a row. */
#define A733_POWER_ROW_GENERATION_MASK (UINT32_MAX >> 3)
/* Store ON (1) or OFF (0) with release order and notify the service.
 * Only domain 5 accepts OFF. Domain 4 stays ON for shared SRAM/ROM use.
 * Refused (0) while the previous request is unanswered.
 * Run the consumer's own authorization immediately before the call. */
static inline int a733_power_domain_request(struct a733_power_domain_row *r, unsigned on)
{
    if (!r || !r->domains || !r->notify || on > 1 || (!on && r->domain != 5)) return 0;
    const unsigned q = atomic_load_explicit(&r->request, memory_order_relaxed);
    const unsigned g = (q >> 1) & A733_POWER_ROW_GENERATION_MASK;
    if ((atomic_load_explicit(&r->state, memory_order_acquire) >> 3) != g) return 0;
    atomic_store_explicit(&r->request, ((g + 1) & A733_POWER_ROW_GENERATION_MASK) << 1 | on,
                          memory_order_release);
    r->notify(r->notify_context);
    return 1;
}
/* The answer to the latest request: one acquire load of the service's word
 * and a relaxed load of the consumer's own; no lock. */
static inline enum a733_power_row_state a733_power_domain_state(const struct a733_power_domain_row *r)
{
    const unsigned s = atomic_load_explicit(&r->state, memory_order_acquire);
    const unsigned g = (atomic_load_explicit(&r->request, memory_order_relaxed) >> 1) &
        A733_POWER_ROW_GENERATION_MASK;
    return (s >> 3) == g ? (enum a733_power_row_state)(s & 7) : A733_POWER_ROW_PENDING;
}
#endif
