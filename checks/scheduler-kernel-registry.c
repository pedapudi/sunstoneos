/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../kernel/scheduler/freertos_registry.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>

static struct ss_kernel_registry registry;
struct task_storage { uint64_t sequence, inverse; };
static struct task_storage task;
static _Atomic bool stop;
static _Atomic unsigned observations;

static void *reader(void *unused)
{
    (void)unused;
    while (!atomic_load_explicit(&stop, memory_order_acquire)) {
        struct ss_kernel_pin pin;
        if (!ss_kernel_handle_pin(&registry, &task, &pin)) continue;
        assert(pin.slot < SS_OWNER_SLOTS && pin.incarnation);
        assert(pin.descriptor->task == &task);
        assert(task.sequence == ~task.inverse);
        for (unsigned i = 0; i < 4; ++i) sched_yield();
        assert(task.sequence == ~task.inverse);
        atomic_fetch_add_explicit(&observations, 1, memory_order_relaxed);
        ss_kernel_handle_put(&pin);
        assert(!pin.descriptor && pin.slot == SS_KERNEL_NO_SLOT);
    }
    return NULL;
}

static void capacity_and_tombstones(void)
{
    ss_kernel_registry_init(&registry, NULL);
    unsigned slots[SS_OWNER_SLOTS];
    /* Deliberately regular addresses exercise hash folding, collision chains
     * and tombstones. No function may dereference these opaque keys. */
    for (unsigned i = 0; i < SS_OWNER_SLOTS; ++i) {
        void *handle = (void *)(uintptr_t)(0x100000 + 4096u * i);
        slots[i] = ss_kernel_handle_register(&registry, handle, NULL, NULL);
        assert(slots[i] < SS_OWNER_SLOTS);
        assert(ss_kernel_handle_register(&registry, handle, NULL, NULL) == SS_KERNEL_NO_SLOT);
    }
    assert(ss_kernel_handle_register(&registry, (void *)(uintptr_t)0x900000, NULL, NULL) == SS_KERNEL_NO_SLOT);
    for (unsigned i = 0; i < SS_OWNER_SLOTS; i += 2) {
        assert(ss_kernel_handle_close(&registry, slots[i]));
        assert(!ss_kernel_handle_retire(&registry, slots[i], false));
        assert(ss_kernel_handle_retire(&registry, slots[i], true));
    }
    for (unsigned i = 1; i < SS_OWNER_SLOTS; i += 2) {
        struct ss_kernel_pin pin;
        assert(ss_kernel_handle_pin(&registry, (void *)(uintptr_t)(0x100000 + 4096u * i), &pin));
        assert(pin.slot == slots[i]);
        ss_kernel_handle_put(&pin);
    }
    for (unsigned i = 0; i < SS_OWNER_SLOTS; i += 2)
        assert(ss_kernel_handle_register(&registry, (void *)(uintptr_t)(0x900000 + 4096u * i), NULL, NULL) < SS_OWNER_SLOTS);
    struct ss_kernel_pin pin;
    assert(!ss_kernel_handle_pin(&registry, NULL, &pin));
    assert(!ss_kernel_handle_pin(&registry, (void *)UINTPTR_MAX, &pin));
    assert(ss_kernel_handle_register(&registry, NULL, NULL, NULL) == SS_KERNEL_NO_SLOT);
    assert(ss_kernel_handle_register(&registry, (void *)UINTPTR_MAX, NULL, NULL) == SS_KERNEL_NO_SLOT);
}

int main(void)
{
    capacity_and_tombstones();
    ss_kernel_registry_init(&registry, NULL);
    task = (struct task_storage){ 1, ~UINT64_C(1) };
    unsigned slot = ss_kernel_handle_register(&registry, &task, NULL, NULL);
    assert(slot < SS_OWNER_SLOTS);
    struct ss_kernel_pin held;
    assert(ss_kernel_handle_pin(&registry, &task, &held));
    assert(!ss_kernel_handle_retire(&registry, slot, true));
    assert(ss_kernel_handle_close(&registry, slot));
    assert(!ss_kernel_handle_retire(&registry, slot, true));
    struct ss_kernel_pin refused;
    assert(!ss_kernel_handle_pin(&registry, &task, &refused));
    ss_kernel_handle_put(&held);
    assert(ss_kernel_handle_retire(&registry, slot, true));
    slot = ss_kernel_handle_register(&registry, &task, NULL, NULL);
    assert(slot < SS_OWNER_SLOTS);
    pthread_t readers[8];
    for (unsigned i = 0; i < 8; ++i) assert(!pthread_create(&readers[i], NULL, reader, NULL));
    for (unsigned i = 0; i < 2000; ++i) {
        /* Let each incarnation admit readers before closing. The only task
         * storage mutation is after close, retirement and owner acknowledgments. */
        sched_yield();
        assert(ss_kernel_handle_close(&registry, slot));
        while (!ss_kernel_handle_retire(&registry, slot, true)) sched_yield();
        ++task.sequence;
        task.inverse = ~task.sequence;
        slot = ss_kernel_handle_register(&registry, &task, NULL, NULL);
        assert(slot < SS_OWNER_SLOTS);
    }
    atomic_store_explicit(&stop, true, memory_order_release);
    for (unsigned i = 0; i < 8; ++i) assert(!pthread_join(readers[i], NULL));
    assert(atomic_load_explicit(&observations, memory_order_relaxed));
    assert(ss_kernel_handle_close(&registry, slot));
    assert(ss_kernel_handle_retire(&registry, slot, true));
    puts("scheduler kernel registry: capacity, tombstone lookup, held-reference retirement and eight-reader delete/reuse passed");
}
