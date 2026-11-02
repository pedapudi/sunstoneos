/* Host cost proxy for the owner scheduler (included with -include in the
 * latency-partitioned-model image only). QEMU timing cannot attribute time to
 * one core, so the model counts the C11 atomic operations that the scheduler
 * executes on each core. On the A733 an atomic read-modify-write or a store to
 * a line that another core owns costs a cache-line transfer, which dominates
 * the masked spans. The recorder (runtime/latency.c, SS_LATENCY_OPS) reports
 * the count between mask open and close in place of ticks. */
#ifndef SUNSTONE_LATENCY_OPS_H
#define SUNSTONE_LATENCY_OPS_H
#include <stdatomic.h>
#include <stdint.h>
extern uint64_t ss_ops_rmw[8], ss_ops_store[8], ss_ops_load[8];
static inline unsigned ss_ops_core(void)
{ uint64_t v; __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(v)); return (unsigned)((v >> 48) & 7u); }
#define SS_OPS_COUNT(kind) (__atomic_fetch_add(&ss_ops_##kind[ss_ops_core()], 1, __ATOMIC_RELAXED))
#undef atomic_load_explicit
#undef atomic_store_explicit
#undef atomic_fetch_add_explicit
#undef atomic_fetch_sub_explicit
#undef atomic_fetch_or_explicit
#undef atomic_fetch_and_explicit
#undef atomic_exchange_explicit
#undef atomic_compare_exchange_strong_explicit
#define atomic_load_explicit(o, m) (SS_OPS_COUNT(load), __c11_atomic_load(o, m))
#define atomic_store_explicit(o, v, m) (SS_OPS_COUNT(store), __c11_atomic_store(o, v, m))
#define atomic_fetch_add_explicit(o, v, m) (SS_OPS_COUNT(rmw), __c11_atomic_fetch_add(o, v, m))
#define atomic_fetch_sub_explicit(o, v, m) (SS_OPS_COUNT(rmw), __c11_atomic_fetch_sub(o, v, m))
#define atomic_fetch_or_explicit(o, v, m) (SS_OPS_COUNT(rmw), __c11_atomic_fetch_or(o, v, m))
#define atomic_fetch_and_explicit(o, v, m) (SS_OPS_COUNT(rmw), __c11_atomic_fetch_and(o, v, m))
#define atomic_exchange_explicit(o, v, m) (SS_OPS_COUNT(rmw), __c11_atomic_exchange(o, v, m))
#define atomic_compare_exchange_strong_explicit(o, e, d, s, f) \
    (SS_OPS_COUNT(rmw), __c11_atomic_compare_exchange_strong(o, e, d, s, f))
#endif
