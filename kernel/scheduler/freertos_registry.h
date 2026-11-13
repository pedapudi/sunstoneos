/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SS_FREERTOS_REGISTRY_H
#define SS_FREERTOS_REGISTRY_H
#include "owner.h"
#include <stddef.h>

#define SS_KERNEL_NO_SLOT SS_OWNER_SLOTS

/* Administrator serializes register/retire. Readers need no administration
 * lock and may run in API-capable IRQ context. TCB memory is read only after
 * ss_kernel_handle_pin returns a permanent pinned descriptor.
 *
 * The descriptor table is itself the open-addressed handle table: a handle
 * hashes to its home slot and registration takes the first free slot on the
 * probe chain from there. Each descriptor's key is zero until first use,
 * the handle while mapped, and a tombstone after unmap. A lookup without a
 * collision therefore touches one line, the target's descriptor. */
struct ss_kernel_registry {
    struct ss_owner_descriptor descriptor[SS_OWNER_SLOTS];
    bool registered[SS_OWNER_SLOTS], mapped[SS_OWNER_SLOTS];
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
/* As ss_kernel_handle_pin with the barrier backend supplied by the caller, so
 * that a kernel whose backend is the architectural default reads no registry
 * line besides the descriptor. */
bool ss_kernel_handle_pin_with(struct ss_kernel_registry *, const struct ss_owner_backend *,
                               void *, struct ss_kernel_pin *);
void ss_kernel_handle_put(struct ss_kernel_pin *);
/* Close before owner eviction and publisher retirement. Retire neither spins
 * nor frees memory. The administrator retries it after acknowledgments, with
 * brief API interrupt exclusion and no object/control lock held. */
bool ss_kernel_handle_close(struct ss_kernel_registry *, unsigned);
/* Remove a closed handle key after owner drainage, while reserving its slot
 * through resource destruction. A recycled raw address can then map another
 * slot; late retirement of the old slot never removes that new key. */
bool ss_kernel_handle_unmap(struct ss_kernel_registry *, unsigned,
                            bool owners_drained);
bool ss_kernel_handle_retire(struct ss_kernel_registry *, unsigned,
                             bool owners_drained);
#endif
