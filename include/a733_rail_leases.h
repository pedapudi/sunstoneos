#ifndef SUNSTONE_A733_RAIL_LEASES_H
#define SUNSTONE_A733_RAIL_LEASES_H
#include <stdatomic.h>
#include <stdint.h>
#include "a733_clock_leases.h"
#include "a733_gpu_resources.h"

typedef uint64_t a733_rail_mask_t;
/* AXP8191 output order follows the pinned pmu_axp8191_ctrl_tbl. */
enum a733_rail {
    A733_RAIL_DCDC1, A733_RAIL_DCDC2, A733_RAIL_DCDC3, A733_RAIL_DCDC4,
    A733_RAIL_DCDC5, A733_RAIL_DCDC6, A733_RAIL_DCDC7, A733_RAIL_DCDC8,
    A733_RAIL_DCDC9,
    A733_RAIL_ALDO1, A733_RAIL_ALDO2, A733_RAIL_ALDO3, A733_RAIL_ALDO4,
    A733_RAIL_ALDO5, A733_RAIL_ALDO6,
    A733_RAIL_BLDO1, A733_RAIL_BLDO2, A733_RAIL_BLDO3, A733_RAIL_BLDO4,
    A733_RAIL_BLDO5,
    A733_RAIL_CLDO1, A733_RAIL_CLDO2, A733_RAIL_CLDO3, A733_RAIL_CLDO4,
    A733_RAIL_CLDO5,
    A733_RAIL_DLDO1, A733_RAIL_DLDO2, A733_RAIL_DLDO3, A733_RAIL_DLDO4,
    A733_RAIL_DLDO5, A733_RAIL_DLDO6,
    A733_RAIL_ELDO1, A733_RAIL_ELDO2, A733_RAIL_ELDO3, A733_RAIL_ELDO4,
    A733_RAIL_ELDO5, A733_RAIL_ELDO6,
    A733_RAIL_DC1SW1, A733_RAIL_DC1SW2, A733_RAIL_COUNT
};
#define A733_RAIL_BIT(rail) (UINT64_C(1) << (rail))
#define A733_RAIL_ALL ((UINT64_C(1) << A733_RAIL_COUNT) - 1)
#define A733_RAIL_CLIENTS 128u
#define A733_RAIL_BORROWS 64u
enum a733_rail_result {
    A733_RAIL_READY, A733_RAIL_PENDING, A733_RAIL_BUSY, A733_RAIL_INVALID,
    A733_RAIL_EXPIRED, A733_RAIL_RETAINED, A733_RAIL_EXHAUSTED
};
enum a733_rail_state {
    A733_RAIL_FREE, A733_RAIL_OPENING, A733_RAIL_ACTIVE,
    A733_RAIL_CLOSING, A733_RAIL_FAULTED
};
struct a733_rail_token { uint64_t id, owner; a733_rail_mask_t shared, exclusive; };
struct a733_rail_borrow {
    uint64_t id;
    struct a733_rail_token parent;
    a733_rail_mask_t rails;
};
struct a733_rail_binding {
    struct a733_clock_leases *clocks;
    struct a733_gpu_resource_manager *gpu;
    uint64_t bridge_owner;
    int (*now)(void *, uint64_t *);
    void *context;
};
struct a733_rail_slot {
    struct a733_rail_token token;
    struct a733_clock_token dcdc2;
    struct a733_gpu_rail_guard dcdc4;
    uint64_t deadline;
    unsigned borrows;
    enum a733_rail_state state;
};
struct a733_rail_borrow_slot {
    struct a733_rail_borrow ticket;
    const struct a733_rail_borrow *address;
};
struct a733_rail_leases {
    struct a733_rail_leases *self;
    struct a733_rail_binding binding, sealed_binding;
    atomic_uint busy;
    uint64_t next_id, next_borrow, last_now;
    unsigned failed;
    struct a733_rail_slot slots[A733_RAIL_CLIENTS];
    struct a733_rail_borrow_slot borrows[A733_RAIL_BORROWS];
};
/* Zero private storage once; initialize before concurrency. Ledger, bindings,
 * common registry and GPU manager have platform lifetime and cannot be copied,
 * rebound or reinitialized. now/context must equal the common registry's
 * provider and share its monotonic microsecond history. No hardware access.
 * All PMIC output writers must participate; raw legacy DCDC2 and GPU DCDC4
 * users are excluded through actual guards held privately by this ledger.
 * S_TWI0/PL0/PL1, remote-firmware exclusion and electrical proof are separate.
 */
int a733_rail_leases_init(struct a733_rail_leases *, const struct a733_rail_binding *);
/* DC1SW1/2 share DCDC1's voltage word. Any of these bits expands to all three;
 * exclusive dominates shared. Shared enable-register bytes still require the
 * PMIC provider's bus lock and preservation of unrelated bits.
 * acquire returns PENDING and a token before bridge acquisition. poll grants
 * READY only after acquiring and authenticating every required real guard.
 * BUSY/refusals before reservation leave output unchanged. A pending token
 * provides no voltage, enable, settling or device-access proof. OPENING expiry
 * retains all admitted reservations; the caller must not discard the token.
 * Nonblocking calls never spin. Ledger lock precedes common/GPU metadata locks;
 * no I2C, MMIO or register mutex is acquired by this API.
 */
enum a733_rail_result a733_rail_acquire(struct a733_rail_leases *, uint64_t owner,
    a733_rail_mask_t shared, a733_rail_mask_t exclusive, uint64_t deadline,
    struct a733_rail_token *);
enum a733_rail_result a733_rail_poll(struct a733_rail_leases *, const struct a733_rail_token *);
/* Read-only exact authentication. PENDING includes OPENING or CLOSING; only
 * READY permits a consumer to continue its separately authorized operation.
 * BUSY cannot establish health. No timer or hardware callback is made. */
enum a733_rail_result a733_rail_status(struct a733_rail_leases *, const struct a733_rail_token *);
/* A borrow pins an ACTIVE exact parent until end_borrow. Provider storage for
 * the ticket remains at its original address until completion; copies cannot
 * authenticate or end it. Only a nonempty subset of held rails is permitted.
 * Validate at request admission, before access and before result publication.
 * These three calls are nonblocking metadata operations with no time/hardware
 * callbacks. Uncertain active I2C work keeps its ticket indefinitely.
 */
enum a733_rail_result a733_rail_borrow(struct a733_rail_leases *, const struct a733_rail_token *,
    a733_rail_mask_t rails, struct a733_rail_borrow *);
enum a733_rail_result a733_rail_borrow_status(struct a733_rail_leases *, const struct a733_rail_borrow *);
enum a733_rail_result a733_rail_end_borrow(struct a733_rail_leases *, const struct a733_rail_borrow *);
/* Call only after independent consumer quiescence/restoration or cancellation
 * before device access. Outstanding borrows refuse release. CLOSING permits
 * metadata cleanup only; repeated release resumes cleanup without device I/O.
 * Each successful bridge release is irreversible and recorded immediately.
 * Each guard release has a deadline admission check. No fallible callback runs
 * after the final guard release; slot clear is infallible metadata completion.
 * A later refusal retains only remaining guards, never claims a released guard
 * is still owned. READY means cleanup completed and the token is stale.
 */
enum a733_rail_result a733_rail_release(struct a733_rail_leases *, const struct a733_rail_token *, uint64_t deadline);
/* Latch a terminal ledger fault while preserving live reservations/borrows.
 * No physical action or implicit retry/reclamation. */
enum a733_rail_result a733_rail_retain(struct a733_rail_leases *, const struct a733_rail_token *);
#endif
