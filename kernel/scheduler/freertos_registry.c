/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "freertos_registry.h"

_Static_assert((SS_KERNEL_HANDLE_BUCKETS & (SS_KERNEL_HANDLE_BUCKETS - 1)) == 0,
               "handle buckets must be a power of two");
_Static_assert(SS_KERNEL_HANDLE_BUCKETS > SS_OWNER_SLOTS,
               "open-addressed lookup requires an empty bucket");
_Static_assert(ATOMIC_POINTER_LOCK_FREE == 2, "native handle-key loads required");
#define TOMBSTONE UINTPTR_MAX

static unsigned handle_hash(uintptr_t handle)
{
    /* StaticTask_t commonly shares stack alignment. Fold higher address bits
     * before selecting a bucket so equally spaced TCB arrays do not all alias. */
    handle ^= handle >> 11;
    handle ^= handle >> 23;
    return (unsigned)(handle >> 3) & (SS_KERNEL_HANDLE_BUCKETS - 1);
}

void ss_kernel_registry_init(struct ss_kernel_registry *r,
                             const struct ss_owner_backend *backend)
{
    ss_kernel_registry_init_retirement(r, backend, NULL, NULL, 0);
}
void ss_kernel_registry_init_retirement(struct ss_kernel_registry *r,
    const struct ss_owner_backend *backend,
    void (*notify)(void *, unsigned, unsigned), void *context, unsigned coordinator)
{
    r->backend = backend;
    r->next_incarnation = 1;
    for (unsigned i = 0; i < SS_OWNER_SLOTS; ++i) {
        struct ss_owner_retirement retirement = { notify, context, i, coordinator };
        ss_owner_descriptor_init_closed(&r->descriptor[i], 0, &retirement);
        r->registered[i] = r->mapped[i] = false;
    }
    for (unsigned i = 0; i < SS_KERNEL_HANDLE_BUCKETS; ++i) {
        atomic_init(&r->bucket[i].slot, SS_KERNEL_NO_SLOT);
        atomic_init(&r->bucket[i].handle, 0);
    }
}

unsigned ss_kernel_handle_register(struct ss_kernel_registry *r, void *task,
                                    ss_kernel_slot_init initialize, void *context)
{
    uintptr_t handle = (uintptr_t)task;
    if (!handle || handle == TOMBSTONE || !r->next_incarnation)
        return SS_KERNEL_NO_SLOT;
    unsigned first_free = SS_KERNEL_HANDLE_BUCKETS;
    unsigned hash = handle_hash(handle);
    for (unsigned n = 0; n < SS_KERNEL_HANDLE_BUCKETS; ++n) {
        unsigned b = (hash + n) & (SS_KERNEL_HANDLE_BUCKETS - 1);
        uintptr_t key = atomic_load_explicit(&r->bucket[b].handle, memory_order_acquire);
        if (key == handle) return SS_KERNEL_NO_SLOT;
        if (key == TOMBSTONE && first_free == SS_KERNEL_HANDLE_BUCKETS) first_free = b;
        if (!key) {
            if (first_free == SS_KERNEL_HANDLE_BUCKETS) first_free = b;
            break;
        }
    }
    if (first_free == SS_KERNEL_HANDLE_BUCKETS) return SS_KERNEL_NO_SLOT;
    unsigned slot;
    /* A lookup that saw a retired key can briefly increment a closed slot's
     * references before rejecting admission. Skip such a slot without spinning
     * inside administration; another unused slot remains admissible. */
    for (slot = 0; slot < SS_OWNER_SLOTS; ++slot) {
        if (r->registered[slot] || !ss_owner_references_retired(&r->descriptor[slot]))
            continue;
        uint64_t incarnation = r->next_incarnation++;
        if (!incarnation) return SS_KERNEL_NO_SLOT;
        if (initialize) initialize(context, slot, incarnation);
        if (ss_owner_reopen(&r->descriptor[slot], task, incarnation, true)) break;
    }
    if (slot == SS_OWNER_SLOTS) return SS_KERNEL_NO_SLOT;
    r->registered[slot] = r->mapped[slot] = true;
    atomic_store_explicit(&r->bucket[first_free].slot, slot, memory_order_relaxed);
    atomic_store_explicit(&r->bucket[first_free].handle, handle, memory_order_release);
    return slot;
}

bool ss_kernel_handle_pin(struct ss_kernel_registry *r, void *task,
                          struct ss_kernel_pin *out)
{
    uintptr_t handle = (uintptr_t)task;
    *out = (struct ss_kernel_pin){ .slot = SS_KERNEL_NO_SLOT };
    if (!handle || handle == TOMBSTONE) return false;
    unsigned hash = handle_hash(handle);
    for (unsigned n = 0; n < SS_KERNEL_HANDLE_BUCKETS; ++n) {
        unsigned b = (hash + n) & (SS_KERNEL_HANDLE_BUCKETS - 1);
        uintptr_t key = atomic_load_explicit(&r->bucket[b].handle, memory_order_acquire);
        if (!key) return false;
        if (key != handle) continue;
        unsigned slot = atomic_load_explicit(&r->bucket[b].slot, memory_order_relaxed);
        if (slot >= SS_OWNER_SLOTS) return false;
        struct ss_owner_descriptor *d = &r->descriptor[slot];
        uint64_t incarnation = atomic_load_explicit(&d->incarnation, memory_order_acquire);
        if (!ss_owner_pin(d, incarnation, r->backend)) return false;
        /* A reader can see an old key followed by a newly reused bucket's slot.
         * The pointer is inspected only after pin; retirement cannot replace it
         * while the pin remains held. A mismatching mapping never touches a TCB. */
        if (d->task != task) {
            ss_owner_put(d);
            return false;
        }
        *out = (struct ss_kernel_pin){ d, slot, incarnation };
        return true;
    }
    return false;
}

void ss_kernel_handle_put(struct ss_kernel_pin *pin)
{
    if (pin->descriptor) ss_owner_put(pin->descriptor);
    *pin = (struct ss_kernel_pin){ .slot = SS_KERNEL_NO_SLOT };
}

bool ss_kernel_handle_close(struct ss_kernel_registry *r, unsigned slot)
{
    if (slot >= SS_OWNER_SLOTS || !r->registered[slot]) return false;
    ss_owner_close(&r->descriptor[slot], r->backend);
    return true;
}

bool ss_kernel_handle_unmap(struct ss_kernel_registry *r, unsigned slot,
                             bool owners_drained)
{
    if (slot >= SS_OWNER_SLOTS || !r->registered[slot] || !owners_drained)
        return false;
    struct ss_owner_descriptor *d = &r->descriptor[slot];
    if (atomic_load_explicit(&d->admission, memory_order_acquire) ||
        !ss_owner_references_retired(d)) return false;
    if (!r->mapped[slot]) return true;
    uintptr_t handle = (uintptr_t)d->task;
    unsigned hash = handle_hash(handle);
    for (unsigned n = 0; n < SS_KERNEL_HANDLE_BUCKETS; ++n) {
        unsigned b = (hash + n) & (SS_KERNEL_HANDLE_BUCKETS - 1);
        uintptr_t key = atomic_load_explicit(&r->bucket[b].handle, memory_order_acquire);
        if (!key) return false;
        if (key != handle) continue;
        if (atomic_load_explicit(&r->bucket[b].slot, memory_order_relaxed) != slot)
            return false;
        atomic_store_explicit(&r->bucket[b].handle, TOMBSTONE, memory_order_release);
        r->mapped[slot] = false;
        return true;
    }
    return false;
}

bool ss_kernel_handle_retire(struct ss_kernel_registry *r, unsigned slot,
                             bool owners_drained)
{
    if (!ss_kernel_handle_unmap(r, slot, owners_drained)) return false;
    r->registered[slot] = false;
    return true;
}
