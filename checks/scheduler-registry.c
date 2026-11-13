/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../kernel/scheduler/freertos_registry.h"
#include <assert.h>
#include <stdio.h>

/* Handles are addresses of these stand-ins; the registry never dereferences them. */
static _Alignas(64) unsigned char tasks[1024][64];
static struct ss_kernel_registry r;
static unsigned initialized;
static void count_init(void *context, unsigned slot, uint64_t incarnation)
{ (void)context; (void)slot; (void)incarnation; ++initialized; }

static unsigned home(void *task)
{
    /* The home slot is the slot a handle takes in an empty registry. */
    static struct ss_kernel_registry empty;
    ss_kernel_registry_init(&empty, NULL);
    return ss_kernel_handle_register(&empty, task, NULL, NULL);
}
static unsigned pin_slot(void *task)
{
    struct ss_kernel_pin pin;
    if (!ss_kernel_handle_pin(&r, task, &pin)) return SS_KERNEL_NO_SLOT;
    unsigned slot = pin.slot;
    assert(pin.descriptor == &r.descriptor[slot] && r.descriptor[slot].task == task);
    ss_kernel_handle_put(&pin);
    return slot;
}
static void retire(unsigned slot)
{
    assert(ss_kernel_handle_close(&r, slot));
    assert(ss_kernel_handle_retire(&r, slot, true));
}

/* Two handles with one home slot: lookups follow the chain across a tombstone. */
static void collision_chain(void)
{
    ss_kernel_registry_init(&r, NULL);
    unsigned a = 0, b = 1;
    while (home(tasks[b]) != home(tasks[a])) ++b;
    unsigned h = home(tasks[a]);
    assert(ss_kernel_handle_register(&r, tasks[a], NULL, NULL) == h);
    unsigned sb = ss_kernel_handle_register(&r, tasks[b], NULL, NULL);
    assert(sb == ((h + 1) & (SS_OWNER_SLOTS - 1)));
    assert(pin_slot(tasks[a]) == h && pin_slot(tasks[b]) == sb);
    assert(ss_kernel_handle_register(&r, tasks[a], NULL, NULL) == SS_KERNEL_NO_SLOT);
    retire(h);
    assert(r.descriptor[h].key == UINTPTR_MAX);
    assert(pin_slot(tasks[a]) == SS_KERNEL_NO_SLOT);
    assert(pin_slot(tasks[b]) == sb);
    /* The duplicate search continues past the tombstone and finds b. */
    assert(ss_kernel_handle_register(&r, tasks[b], NULL, NULL) == SS_KERNEL_NO_SLOT);
    /* A new handle with the same home reuses the tombstoned home slot. */
    unsigned c = b + 1;
    while (home(tasks[c]) != h) ++c;
    assert(ss_kernel_handle_register(&r, tasks[c], NULL, NULL) == h);
    assert(pin_slot(tasks[c]) == h && pin_slot(tasks[b]) == sb);
}

/* A recycled address maps to a new slot while the old slot is still reserved;
 * the old slot's late retirement never removes the new key. */
static void recycled_address(void)
{
    ss_kernel_registry_init(&r, NULL);
    unsigned old = ss_kernel_handle_register(&r, tasks[7], NULL, NULL);
    assert(old == home(tasks[7]));
    assert(ss_kernel_handle_close(&r, old));
    assert(ss_kernel_handle_unmap(&r, old, true));
    unsigned again = ss_kernel_handle_register(&r, tasks[7], NULL, NULL);
    assert(again != SS_KERNEL_NO_SLOT && again != old);
    assert(ss_kernel_handle_retire(&r, old, true));
    assert(pin_slot(tasks[7]) == again);
}

/* A lookup that pinned a closed slot holds a reference; registration skips
 * that slot instead of waiting for the reference. */
static void late_reference(void)
{
    ss_kernel_registry_init(&r, NULL);
    unsigned h = ss_kernel_handle_register(&r, tasks[3], NULL, NULL);
    retire(h);
    atomic_fetch_add(&r.descriptor[h].references, 1);
    unsigned c = 4;
    while (home(tasks[c]) != h) ++c;
    initialized = 0;
    unsigned slot = ss_kernel_handle_register(&r, tasks[c], count_init, NULL);
    assert(slot == ((h + 1) & (SS_OWNER_SLOTS - 1)) && initialized == 1);
    atomic_fetch_sub(&r.descriptor[h].references, 1);
    assert(pin_slot(tasks[c]) == slot);
}

/* A reader that saw an old key, then the slot's reuse, rejects the mapping
 * and releases its reference. */
static void stale_key(void)
{
    ss_kernel_registry_init(&r, NULL);
    unsigned h = ss_kernel_handle_register(&r, tasks[9], NULL, NULL);
    r.descriptor[h].task = tasks[10]; /* the reuse a racing reader can observe */
    struct ss_kernel_pin pin;
    assert(!ss_kernel_handle_pin(&r, tasks[9], &pin) && pin.slot == SS_KERNEL_NO_SLOT);
    assert(ss_owner_references_retired(&r.descriptor[h]));
}

/* Every slot fills; lookups of absent handles end, and every handle pins. */
static void full_table(void)
{
    ss_kernel_registry_init(&r, NULL);
    unsigned used[SS_OWNER_SLOTS] = {0}, homes = 0;
    for (unsigned i = 0; i < SS_OWNER_SLOTS; ++i) {
        unsigned slot = ss_kernel_handle_register(&r, tasks[i], NULL, NULL);
        assert(slot < SS_OWNER_SLOTS && !used[slot]++);
        homes += slot == home(tasks[i]);
    }
    assert(ss_kernel_handle_register(&r, tasks[SS_OWNER_SLOTS], NULL, NULL) == SS_KERNEL_NO_SLOT);
    assert(pin_slot(tasks[SS_OWNER_SLOTS]) == SS_KERNEL_NO_SLOT);
    for (unsigned i = 0; i < SS_OWNER_SLOTS; ++i) assert(pin_slot(tasks[i]) < SS_OWNER_SLOTS);
    printf("scheduler registry: %u of %u handles in a full table sit in their home slot\n",
           homes, SS_OWNER_SLOTS);
    /* Line-spaced handles must spread: a hash of the low address bits alone
     * put 16 of these 128 in their home slot, so most lookups probed. */
    assert(homes >= SS_OWNER_SLOTS * 3 / 4);
}

int main(void)
{
    assert(sizeof(struct ss_owner_descriptor) == 64 && _Alignof(struct ss_owner_descriptor) == 64);
    collision_chain();
    recycled_address();
    late_reference();
    stale_key();
    full_table();
    puts("scheduler registry: home slots, collision chains, tombstones, recycled addresses "
         "and stale keys pass");
}
