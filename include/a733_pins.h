#ifndef SUNSTONE_A733_PINS_H
#define SUNSTONE_A733_PINS_H
#include <stddef.h>
#include <stdint.h>

#define A733_PINS_PB_MASK UINT32_C(0x1f0) /* PB4..PB8 only. */
#define A733_PINS_BASE UINT32_C(0x02000000)
#define A733_PINS_COUNT 5u

enum a733_pins_result {
    A733_PINS_OK, A733_PINS_BUSY, A733_PINS_CONFLICT, A733_PINS_INVALID,
    A733_PINS_RECOVERY
};
/* Raw source-defined codes. Pull: 0 off, 1 up, 2 down; drive: 0..3.
 * Drive labels in the reference are 10/20/30/40 mA, not electrical guarantees.
 * Supported functions are pin-specific input/alternate/disabled modes.
 * GPIO output (1) and external interrupt (14) are outside this component. */
struct a733_pin_config { uint8_t function, pull, drive; };
struct a733_pin_request {
    unsigned pb_pin;
    struct a733_pin_config expected, desired;
};
struct a733_pin_fields {
    uintptr_t mux_address, pull_address, drive_address;
    unsigned mux_shift, pull_shift, drive_shift;
};
/* Pure field calculation and source-defined mux admission, no I/O. */
int a733_pin_fields(unsigned pb_pin, struct a733_pin_fields *);
int a733_pin_config_valid(unsigned pb_pin, struct a733_pin_config);

/* A token is an identity, not a security credential. Privileged callers must
 * authenticate their clients. Never edit it or discard a retained token. */
struct a733_pin_lease { uint64_t id, owner; uint32_t pb_mask; };
struct a733_pins_operations {
    /* One shared scheduling-aware lock for every writer of these PIO words.
     * enter returns 1 only while owning it; false has no effects. The absolute
     * deadline and now use nonwrapping monotonic microseconds. */
    int (*enter)(void *, uint64_t deadline);
    void (*leave)(void *);
    int (*now)(void *, uint64_t *microseconds);
    int (*read)(void *, uintptr_t, uint32_t *);
    int (*write)(void *, uintptr_t, uint32_t);
};
struct a733_pin_state {
    uint64_t id, owner;
    struct a733_pin_config initial, current;
};
/* Implementation state; zero-initialize once and never modify initialized
 * fields. All managers sharing these pins must be the SAME instance. */
struct a733_pins {
    struct a733_pins_operations operations;
    void *cookie;
    uint64_t next_id;
    uint32_t authorized_pb_mask;
    unsigned initialized, failed;
    struct a733_pin_state pins[A733_PINS_COUNT];
};
/* No MMIO. authorized_pb_mask is an explicit platform handoff of unowned pins,
 * not a discovery of unused pins. PIO clock, bank voltage, stable mappings,
 * watchdog recovery and quiescent peripheral functions are prerequisites. */
int a733_pins_init(struct a733_pins *, const struct a733_pins_operations *, void *, uint32_t);
/* Input array and output token must be disjoint from manager state and remain
 * stable through the call. Token output starts zero. Checks all conflicts and
 * expected inherited fields before writes. BUSY/CONFLICT/INVALID leaves output
 * zero. RECOVERY after any possible write retains a populated token and all
 * requested pins, even if their final settings are uncertain. No auto-rollback.
 * Switch through disabled mux, set drive/pull, then select requested functions.
 * All involved peripheral producers must already be quiescent. */
enum a733_pins_result a733_pins_acquire(struct a733_pins *, uint64_t owner,
    const struct a733_pin_request *, size_t count, struct a733_pin_lease *, uint64_t deadline);
/* users_quiescent must be 1 after the caller stops the owning peripheral.
 * Restore only this token's original fields and preserve current neighboring
 * fields. A failed release retains the token. There is no forced release. */
enum a733_pins_result a733_pins_release(struct a733_pins *,
    struct a733_pin_lease *, unsigned users_quiescent, uint64_t deadline);
/* Read a subset of leased GPIO input pins; output is unchanged on failure. */
enum a733_pins_result a733_pins_read_inputs(struct a733_pins *,
    const struct a733_pin_lease *, uint32_t pb_mask, uint32_t *levels, uint64_t deadline);
/* Read ownership under the shared lock, including after RECOVERY; no MMIO. */
enum a733_pins_result a733_pins_held(struct a733_pins *,
    const struct a733_pin_lease *, int *held, uint64_t deadline);
#endif
