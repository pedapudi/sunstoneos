/* Host cases for the control interrupt class: the Non-secure/stored priority view rules of
 * include/a733_gic_priority.h, and the lock-free single-producer single-consumer ring of
 * runtime/control_ring.h under a producer and a consumer thread. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include "../include/a733_gic_priority.h"
#include "../runtime/control_ring.h"

#define API_MASK_NS 0xb0u /* the port's mask, configMAX_API_CALL_INTERRUPT_PRIORITY 11 << 4 */
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static void view_rules(void)
{
    unsigned control_values = 0, masked_values = 0;
    for (unsigned write = 0; write < 256; ++write) {
        const unsigned stored = A733_GIC_STORED_FROM_NS(write);
        CHECK(stored >= 0x80 && (stored & 7u) == 0);                   /* the stored byte keeps the Group 1 range */
        const int signalled = A733_GIC_SIGNALLED(stored, API_MASK_NS); /* signalled with the kernel mask raised */
        const int above = stored < A733_GIC_STORED_API_FLOOR(API_MASK_NS);
        CHECK(signalled == above);                                     /* above the floor exactly when it passes the mask */
        control_values += signalled;
        masked_values += !signalled;
        CHECK(A733_GIC_STORED_FROM_NS(A733_GIC_NS_VIEW(stored)) == stored); /* a read, then a write, keeps the value */
    }
    CHECK(control_values && masked_values);
    /* The control class passes the raised mask, the API floor and the irq-table minimum do not. */
    CHECK(A733_GIC_SIGNALLED(A733_GIC_CONTROL_STORED, API_MASK_NS));
    CHECK(!A733_GIC_SIGNALLED(A733_GIC_STORED_API_FLOOR(API_MASK_NS), API_MASK_NS));
    CHECK(!A733_GIC_SIGNALLED(0xd8, API_MASK_NS) && !A733_GIC_SIGNALLED(0xf0, API_MASK_NS));
    /* A Non-secure write reads back as the Non-secure view of the stored value. */
    CHECK(A733_GIC_NS_VIEW(A733_GIC_STORED_FROM_NS(A733_GIC_NS_WRITE_FOR_STORED(A733_GIC_CONTROL_STORED))) ==
          A733_GIC_NS_VIEW(A733_GIC_CONTROL_STORED));
    /* With the mask open (Non-secure view 0xf0) every stored priority is signalled. */
    CHECK(A733_GIC_SIGNALLED(0xf8, 0xf8 + 1) && A733_GIC_SIGNALLED(0xe8, 0xf0));
    printf("view rules: %u of 256 writes pass the raised API mask, %u are held off\n", control_values, masked_values);
}

#define ITEMS 4000000u
static struct ss_ring ring;
static void *producer(void *unused)
{
    (void)unused;
    for (uint32_t i = 0; i < ITEMS;) {
        if (ss_ring_put(&ring, (struct ss_ring_sample){ i, i * 3u, (uint64_t)i << 20 })) ++i;
    }
    return NULL;
}
static void ring_cases(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, producer, NULL);
    uint32_t expected = 0;
    while (expected < ITEMS) {
        struct ss_ring_sample s;
        if (!ss_ring_get(&ring, &s)) continue;
        CHECK(s.seq == expected && s.latency_ticks == expected * 3u && s.entry_ticks == (uint64_t)expected << 20);
        ++expected;
    }
    pthread_join(thread, NULL);
    /* The producer retries a full ring, so ring.dropped counts those retries; every sample still arrived once and in order. */
    /* A full ring drops the new sample, counts it, and keeps the old ones in order. */
    struct ss_ring full = {0};
    for (uint32_t i = 0; i < SS_RING_SIZE + 5; ++i) (void)ss_ring_put(&full, (struct ss_ring_sample){ i, 0, 0 });
    CHECK(full.dropped == 5);
    for (uint32_t i = 0; i < SS_RING_SIZE; ++i) { struct ss_ring_sample s; CHECK(ss_ring_get(&full, &s) && s.seq == i); }
    printf("ring: %u samples in order through a producer and a consumer thread, full ring drops and counts\n", ITEMS);
}
int main(void)
{
    view_rules();
    ring_cases();
    puts("PASSED");
    return 0;
}
