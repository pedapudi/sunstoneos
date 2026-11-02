/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SS_FREERTOS_REGISTRY_H
#define SS_FREERTOS_REGISTRY_H
#include "owner.h"
#include <stddef.h>

#define SS_KERNEL_HANDLE_BUCKETS 256u
#define SS_KERNEL_NO_SLOT SS_OWNER_SLOTS

/* Administrator serializes register/retire. Readers need no administration
 * lock and may run in API-capable IRQ context. TCB memory is read only after
 * ss_kernel_handle_pin returns a permanent pinned descriptor. */
struct ss_kernel_handle_bucket {
    _Atomic uintptr_t handle;
    _Atomic unsigned slot;
};
struct ss_kernel_registry {
    struct ss_owner_descriptor descriptor[SS_OWNER_SLOTS];
    struct ss_kernel_handle_bucket bucket[SS_KERNEL_HANDLE_BUCKETS];
    bool registered[SS_OWNER_SLOTS];
    uint64_t next_incarnation;
    const struct ss_owner_backend *backend;
};
struct ss_kernel_pin {
    struct ss_owner_descriptor *descriptor;
    unsigned slot;
    uint64_t incarnation;
};
void ss_kernel_registry_init(struct ss_kernel_registry *,
                             const struct ss_owner_backend *);
/* Boot-only permanent routing; register/reopen never changes this callback. */
void ss_kernel_registry_init_retirement(struct ss_kernel_registry *,
    const struct ss_owner_backend *,
    void (*notify)(void *, unsigned, unsigned), void *, unsigned coordinator);
/* NULL and UINTPTR_MAX are reserved keys. Registration initializes all slot
 * state before release-publishing a key. Returns SS_KERNEL_NO_SLOT on exhaustion
 * or duplicate handle. An optional nonblocking initializer fills external slot
 * state while admission remains closed; it may run for more than one candidate
 * if a late rejected pin delays reopening. It never dereferences the handle. */
typedef void (*ss_kernel_slot_init)(void *, unsigned slot, uint64_t incarnation);
unsigned ss_kernel_handle_register(struct ss_kernel_registry *, void *,
                                    ss_kernel_slot_init, void *);
/* Caller supplies valid handle lifetime as required by FreeRTOS. A lookup
 * rejected during deletion reads no TCB. put is mandatory after success. */
bool ss_kernel_handle_pin(struct ss_kernel_registry *, void *,
                          struct ss_kernel_pin *);
void ss_kernel_handle_put(struct ss_kernel_pin *);
/* Close before owner eviction and publisher retirement. Retire neither spins
 * nor frees memory. The administrator retries it after acknowledgments, with
 * API interrupts enabled and no object/control lock held. */
bool ss_kernel_handle_close(struct ss_kernel_registry *, unsigned);
bool ss_kernel_handle_retire(struct ss_kernel_registry *, unsigned,
                             bool owners_drained);
#endif
