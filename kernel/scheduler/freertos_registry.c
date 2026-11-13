/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "freertos_registry.h"

_Static_assert((SS_OWNER_SLOTS & (SS_OWNER_SLOTS - 1)) == 0,
               "the descriptor table is open-addressed by a power-of-two mask");
_Static_assert(ATOMIC_POINTER_LOCK_FREE == 2, "native handle-key loads required");
#define TOMBSTONE UINTPTR_MAX

static unsigned handle_hash(uintptr_t handle)
{
    /* Task control blocks are equally spaced in static arrays and share their
     * low alignment bits. A multiplicative hash takes the home slot from the
     * product's top bits, which depend on every address bit. */
    uint64_t mixed = (uint64_t)handle * UINT64_C(0x9e3779b97f4a7c15);
    return (unsigned)(mixed >> (64 - __builtin_ctz(SS_OWNER_SLOTS)));
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
        atomic_init(&r->descriptor[i].key, 0);
        r->registered[i] = r->mapped[i] = false;
    }
}

unsigned ss_kernel_handle_register(struct ss_kernel_registry *r, void *task,
                                    ss_kernel_slot_init initialize, void *context)
{
    uintptr_t handle = (uintptr_t)task;
    if (!handle || handle == TOMBSTONE || !r->next_incarnation)
        return SS_KERNEL_NO_SLOT;
    unsigned hash = handle_hash(handle), chain = SS_OWNER_SLOTS;
    /* A mapped key's chain from its home slot holds no zero key, because keys
     * never return to zero. The duplicate search can stop at the first zero. */
    for (unsigned n = 0; n < SS_OWNER_SLOTS; ++n) {
        unsigned slot = (hash + n) & (SS_OWNER_SLOTS - 1);
        uintptr_t key = atomic_load_explicit(&r->descriptor[slot].key, memory_order_acquire);
        if (key == handle) return SS_KERNEL_NO_SLOT;
        if (!key) { chain = n + 1; break; }
    }
    /* Take the first free slot on the chain. A lookup that saw a retired key
     * can briefly increment a closed slot's references before rejecting
     * admission. Skip such a slot without spinning inside administration. */
    for (unsigned n = 0; n < chain; ++n) {
        unsigned slot = (hash + n) & (SS_OWNER_SLOTS - 1);
        struct ss_owner_descriptor *d = &r->descriptor[slot];
        if (r->registered[slot] || !ss_owner_references_retired(d))
            continue;
        uint64_t incarnation = r->next_incarnation++;
        if (!incarnation) return SS_KERNEL_NO_SLOT;
        if (initialize) initialize(context, slot, incarnation);
        if (!ss_owner_reopen(d, task, incarnation, true)) continue;
        r->registered[slot] = r->mapped[slot] = true;
        atomic_store_explicit(&d->key, handle, memory_order_release);
        return slot;
    }
    return SS_KERNEL_NO_SLOT;
}

bool ss_kernel_handle_pin_with(struct ss_kernel_registry *r,
                               const struct ss_owner_backend *backend, void *task,
                               struct ss_kernel_pin *out)
{
    uintptr_t handle = (uintptr_t)task;
    *out = (struct ss_kernel_pin){ .slot = SS_KERNEL_NO_SLOT };
    if (!handle || handle == TOMBSTONE) return false;
    unsigned hash = handle_hash(handle);
    for (unsigned n = 0; n < SS_OWNER_SLOTS; ++n) {
        unsigned slot = (hash + n) & (SS_OWNER_SLOTS - 1);
        struct ss_owner_descriptor *d = &r->descriptor[slot];
        uintptr_t key = atomic_load_explicit(&d->key, memory_order_acquire);
        if (!key) return false;
        if (key != handle) continue;
        uint64_t incarnation = atomic_load_explicit(&d->incarnation, memory_order_acquire);
        if (!ss_owner_pin(d, incarnation, backend)) return false;
        /* A reader can see an old key followed by the slot's reuse for another
         * handle. The pointer is inspected only after pin; retirement cannot
         * replace it while the pin remains held. A mismatching mapping never
         * touches a TCB. */
        if (d->task != task) {
            ss_owner_put(d);
            return false;
        }
        *out = (struct ss_kernel_pin){ d, slot, incarnation };
        return true;
    }
    return false;
}

bool ss_kernel_handle_pin(struct ss_kernel_registry *r, void *task,
                          struct ss_kernel_pin *out)
{
    return ss_kernel_handle_pin_with(r, r->backend, task, out);
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
    if (atomic_load_explicit(&d->key, memory_order_relaxed) != (uintptr_t)d->task)
        return false;
    atomic_store_explicit(&d->key, TOMBSTONE, memory_order_release);
    r->mapped[slot] = false;
    return true;
}

bool ss_kernel_handle_retire(struct ss_kernel_registry *r, unsigned slot,
                             bool owners_drained)
{
    if (!ss_kernel_handle_unmap(r, slot, owners_drained)) return false;
    r->registered[slot] = false;
    return true;
}
