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
    A733_POWER_ROW_OFF,     /* released; for domain 5 the domain is OFF */
    A733_POWER_ROW_REFUSED, /* refused before any register write; nothing retained */
    A733_POWER_ROW_FAILED   /* the domain failed; the row and its rail pin are retained */
};

/* Consumer storage with platform lifetime. Each field has one writer: the
 * consumer writes request, the service writes state and the service-only
 * fields. request is (generation << 1) | on; state is
 * (answered generation << 3) | enum a733_power_row_state. */
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
};

/* a733_power_domain_row_init() in a733_power_domains.h registers a row. */
#define A733_POWER_ROW_GENERATION_MASK (UINT32_MAX >> 3)
/* Store ON (1) or OFF (0) with release order and notify the service. Only
 * domain 5 accepts OFF. Refused (0) while the previous request is unanswered.
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
