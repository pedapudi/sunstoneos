/* SPDX-License-Identifier: Apache-2.0 */
/* Single-producer single-consumer ring for the control interrupt class. The producer is an interrupt
 * handler that calls no kernel API; the consumer is a task. Each index has one writer, so neither side
 * takes a lock or waits: a full ring drops the sample and counts it. The size is a power of two. */
#ifndef SS_CONTROL_RING_H
#define SS_CONTROL_RING_H
#include <stdint.h>

#define SS_RING_SIZE 256u
struct ss_ring_sample { uint32_t seq, latency_ticks; uint64_t entry_ticks; };
struct ss_ring {
    struct ss_ring_sample slot[SS_RING_SIZE];
    volatile uint32_t head __attribute__((aligned(64))); /* written by the producer only */
    volatile uint32_t tail __attribute__((aligned(64))); /* written by the consumer only */
    uint32_t dropped;                                    /* producer only */
};
#define SS_RING_LOAD_ACQ(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define SS_RING_STORE_REL(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)

static inline int ss_ring_put(struct ss_ring *r, struct ss_ring_sample s)
{
    const uint32_t head = r->head, tail = SS_RING_LOAD_ACQ(&r->tail);
    if (head - tail >= SS_RING_SIZE) { ++r->dropped; return 0; }
    r->slot[head % SS_RING_SIZE] = s;
    SS_RING_STORE_REL(&r->head, head + 1);
    return 1;
}
static inline int ss_ring_get(struct ss_ring *r, struct ss_ring_sample *s)
{
    const uint32_t tail = r->tail, head = SS_RING_LOAD_ACQ(&r->head);
    if (tail == head) return 0;
    *s = r->slot[tail % SS_RING_SIZE];
    SS_RING_STORE_REL(&r->tail, tail + 1);
    return 1;
}
#endif
