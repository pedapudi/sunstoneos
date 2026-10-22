/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SUNSTONE_A733_POWER_AUTHORITY_H
#define SUNSTONE_A733_POWER_AUTHORITY_H
#include <stdatomic.h>
#include <stdint.h>

/* The power-authority state word: one 64-byte line that carries readiness and
 * revocation for the PMIC provider, the clock registry and the rail ledger.
 * Every bit is set once per boot with an atomic OR and never cleared, so a
 * READY bit cannot overwrite a fault set on another core. A reader makes one
 * acquire load and tests a mask; any set fault bit in the mask refuses the
 * caller. The rail ledger embeds the word, so every object bound to one ledger
 * shares it. */
#define A733_POWER_AUTHORITY_LINE 64u
#define A733_POWER_PROVIDER_READY  (1u << 0)  /* PMIC provider admitted the bus. */
#define A733_POWER_PROVIDER_FAULT  (1u << 8)
#define A733_POWER_CLOCK_FAULT     (1u << 9)
#define A733_POWER_LEDGER_FAULT    (1u << 10)
/* A PMIC transaction depends on the provider, its S_TWI0 clock lease and its
 * rail borrow. */
#define A733_POWER_PMIC_TRANSFER (A733_POWER_PROVIDER_FAULT | A733_POWER_CLOCK_FAULT | A733_POWER_LEDGER_FAULT)

struct a733_power_authority {
    _Alignas(64) atomic_uint word;
    unsigned char unused[A733_POWER_AUTHORITY_LINE - sizeof(atomic_uint)];
};
_Static_assert(sizeof(struct a733_power_authority) == A733_POWER_AUTHORITY_LINE, "one line");

static inline unsigned a733_power_authority_load(const struct a733_power_authority *a)
{ return atomic_load_explicit(&a->word, memory_order_acquire); }
/* True when no fault bit of faults is set. */
static inline int a733_power_authority_admits(const struct a733_power_authority *a, unsigned faults)
{ return !(a733_power_authority_load(a) & faults); }
static inline void a733_power_authority_set(struct a733_power_authority *a, unsigned bits)
{ atomic_fetch_or_explicit(&a->word, bits, memory_order_acq_rel); }
#endif
