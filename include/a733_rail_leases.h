#ifndef SUNSTONE_A733_RAIL_LEASES_H
#define SUNSTONE_A733_RAIL_LEASES_H
#include <stdatomic.h>
#include <stdint.h>
#include "a733_clock_leases.h"
#include "a733_gpu_resources.h"
#include "a733_power_authority.h"

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
/* The fifteen outputs that production code acquires. DC1SW1 and DC1SW2 share
 * DCDC1's voltage word and are accepted as its aliases; every other output is
 * refused. */
#define A733_RAIL_ACCEPTED (A733_RAIL_BIT(A733_RAIL_DCDC1) | A733_RAIL_BIT(A733_RAIL_DCDC2) | \
    A733_RAIL_BIT(A733_RAIL_DCDC3) | A733_RAIL_BIT(A733_RAIL_DCDC4) | A733_RAIL_BIT(A733_RAIL_DCDC5) | \
    A733_RAIL_BIT(A733_RAIL_DCDC8) | A733_RAIL_BIT(A733_RAIL_DCDC9) | A733_RAIL_BIT(A733_RAIL_BLDO1) | \
    A733_RAIL_BIT(A733_RAIL_BLDO2) | A733_RAIL_BIT(A733_RAIL_BLDO5) | A733_RAIL_BIT(A733_RAIL_CLDO1) | \
    A733_RAIL_BIT(A733_RAIL_CLDO2) | A733_RAIL_BIT(A733_RAIL_CLDO3) | A733_RAIL_BIT(A733_RAIL_CLDO5) | \
    A733_RAIL_BIT(A733_RAIL_ELDO6) | A733_RAIL_BIT(A733_RAIL_DC1SW1) | A733_RAIL_BIT(A733_RAIL_DC1SW2))
/* 32 holder slots cover the 31 holders the images create; 10 borrows cover the
 * eight PMIC request slots, the camera's lifetime borrow and one spare. */
#define A733_RAIL_CLIENTS 32u
#define A733_RAIL_BORROWS 10u
#define A733_RAIL_SLOT_BITS 5u
#define A733_RAIL_BORROW_BITS 4u
enum a733_rail_result {
    A733_RAIL_READY, A733_RAIL_PENDING, A733_RAIL_BUSY, A733_RAIL_INVALID,
    A733_RAIL_EXPIRED, A733_RAIL_RETAINED, A733_RAIL_EXHAUSTED
};
/* A token identifier holds its slot index in the low A733_RAIL_SLOT_BITS bits
 * and a never-reused generation above them. */
struct a733_rail_token { uint64_t id, owner; a733_rail_mask_t shared, exclusive; };
struct a733_rail_borrow {
    uint64_t id;
    struct a733_rail_token parent;
    a733_rail_mask_t rails;
};
/* clocks and gpu name the platform's registry and GPU manager for the drivers
 * that share them; the ledger takes neither lease. now/context must equal the
 * registry's monotonic microsecond provider. */
struct a733_rail_binding {
    struct a733_clock_leases *clocks;
    struct a733_gpu_resource_manager *gpu;
    uint64_t bridge_owner;
    int (*now)(void *, uint64_t *);
    void *context;
};
/* A free holder slot has id 0. The borrow slot names its parent by token id. */
struct a733_rail_slot { uint64_t id, owner; a733_rail_mask_t shared, exclusive; };
struct a733_rail_borrow_slot {
    uint64_t id, parent;
    const struct a733_rail_borrow *address;
    a733_rail_mask_t rails;
};
struct a733_rail_leases {
    struct a733_power_authority authority;
    /* Second line: everything status() reads besides the authority word and
     * the holder slot. */
    struct a733_rail_leases *self;
    atomic_uint busy;
    uint32_t free_slots; /* Bit i set: holder slot i is free. */
    a733_rail_mask_t shared_held, exclusive_held;
    uint64_t next_generation, next_borrow, last_now;
    struct a733_rail_binding binding;
    int (*sealed_now)(void *, uint64_t *);
    void *sealed_context;
    uint8_t pins[A733_RAIL_CLIENTS];   /* Borrows and counted domain rows. */
    uint8_t sharers[A733_RAIL_COUNT];  /* Shared holders of each output. */
    uint32_t floor_uv[2];              /* DCDC3 and DCDC5 boot baselines; 0 unrecorded. */
    _Alignas(64) struct a733_rail_slot slots[A733_RAIL_CLIENTS];
    struct a733_rail_borrow_slot borrows[A733_RAIL_BORROWS];
};
/* Zero private storage once; initialize before concurrency. The ledger and
 * its binding have platform lifetime and cannot be copied, rebound or
 * reinitialized. now/context must equal the registry's provider and share its
 * monotonic microsecond history. Initialization binds the registry's fault
 * reporting to this ledger's state word. No hardware access.
 * The ledger is the only rail authority: the PMIC provider writes an output
 * only under an exclusive token of it, and only DCDC3 and DCDC5 are writable.
 * S_TWI0/PL0/PL1, remote-firmware exclusion and electrical proof are separate.
 */
int a733_rail_leases_init(struct a733_rail_leases *, const struct a733_rail_binding *);
/* DC1SW1/2 share DCDC1's voltage word. Any of these bits expands to all three;
 * exclusive dominates shared. Shared enable-register bytes still require the
 * PMIC provider's bus lock and preservation of unrelated bits. An exclusive
 * mask excludes every other holder of those outputs; it grants a selector
 * write only for DCDC3 and DCDC5.
 * acquire returns PENDING and a token, and poll returns READY on its first
 * call without bus access. The pair keeps the existing poll call sites
 * working; a pending token is already admitted. BUSY and refusals leave the
 * output unchanged. A token provides no voltage, enable, settling or
 * device-access proof. Nonblocking calls never spin; no I2C, MMIO or register
 * mutex is acquired by this API.
 */
enum a733_rail_result a733_rail_acquire(struct a733_rail_leases *, uint64_t owner,
    a733_rail_mask_t shared, a733_rail_mask_t exclusive, uint64_t deadline,
    struct a733_rail_token *);
enum a733_rail_result a733_rail_poll(struct a733_rail_leases *, const struct a733_rail_token *);
/* Read-only exact authentication of one holder slot plus the state word.
 * READY permits a consumer to continue its separately authorized operation.
 * BUSY cannot establish health. No timer or hardware callback is made. */
enum a733_rail_result a733_rail_status(struct a733_rail_leases *, const struct a733_rail_token *);
/* A borrow pins an exact parent until end_borrow. Provider storage for the
 * ticket remains at its original address until completion; copies cannot
 * authenticate or end it. Only a nonempty subset of held rails is permitted.
 * These three calls are nonblocking metadata operations with no time or
 * hardware callbacks. Uncertain active I2C work keeps its ticket indefinitely.
 */
enum a733_rail_result a733_rail_borrow(struct a733_rail_leases *, const struct a733_rail_token *,
    a733_rail_mask_t rails, struct a733_rail_borrow *);
enum a733_rail_result a733_rail_borrow_status(struct a733_rail_leases *, const struct a733_rail_borrow *);
enum a733_rail_result a733_rail_end_borrow(struct a733_rail_leases *, const struct a733_rail_borrow *);
/* A counted power-domain row pins its rail token the way a borrow does. pin
 * requires a live token that holds every output of rails; unpin drops one pin.
 * Both are nonblocking metadata operations. */
enum a733_rail_result a733_rail_pin(struct a733_rail_leases *, const struct a733_rail_token *,
    a733_rail_mask_t rails);
enum a733_rail_result a733_rail_unpin(struct a733_rail_leases *, const struct a733_rail_token *);
/* Voltage floor of a CPU rail, DCDC3 or DCDC5, under an exclusive token of
 * it. The first call records observed_uv, the selector voltage the holder read
 * before any write, as the rail's boot baseline; every call returns that
 * baseline in floor_uv. No writer lowers the rail below it. */
enum a733_rail_result a733_rail_floor(struct a733_rail_leases *, const struct a733_rail_token *,
    enum a733_rail, uint32_t observed_uv, uint32_t *floor_uv);
/* Call only after independent consumer quiescence/restoration or cancellation
 * before device access. A pinned token (outstanding borrow or counted domain
 * row) returns BUSY. READY means the slot is free and the token is stale.
 */
enum a733_rail_result a733_rail_release(struct a733_rail_leases *, const struct a733_rail_token *, uint64_t deadline);
/* Latch a terminal ledger fault in the state word while preserving live
 * reservations and borrows. No physical action or implicit retry. */
enum a733_rail_result a733_rail_retain(struct a733_rail_leases *, const struct a733_rail_token *);
#endif
