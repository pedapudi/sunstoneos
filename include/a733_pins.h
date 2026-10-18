#ifndef SUNSTONE_A733_PINS_H
#define SUNSTONE_A733_PINS_H
#include <stddef.h>
#include <stdint.h>

#define A733_PINS_PB_MASK UINT32_C(0x1f0) /* PB4..PB8 only. */
#define A733_PINS_BASE UINT32_C(0x02000000)
#define A733_PINS_COUNT 5u
#define A733_PIN_BANKS 13u
#define A733_PIN_MAX_REQUESTS 32u
#define A733_PINS_R_BASE UINT32_C(0x07025000)
/* Letter indices: B=1 through M=12. A is not implemented. */
enum a733_pin_bank { A733_PIN_B=1, A733_PIN_C, A733_PIN_D, A733_PIN_E,
    A733_PIN_F, A733_PIN_G, A733_PIN_H, A733_PIN_I, A733_PIN_J, A733_PIN_K,
    A733_PIN_L, A733_PIN_M };

enum a733_pins_result {
    A733_PINS_OK, A733_PINS_BUSY, A733_PINS_CONFLICT, A733_PINS_INVALID,
    A733_PINS_RECOVERY
};
/* Raw source-defined codes. Pull: 0 off, 1 up, 2 down; drive: 0..3.
 * Drive labels in the reference are 10/20/30/40 mA, not electrical guarantees.
 * Legacy PB functions accept pin-specific input/alternate/disabled modes.
 * The bank-qualified API also implements GPIO output (1) and interrupt (14). */
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
     * deadline and now use nonwrapping monotonic microseconds. Regressing readings
     * are refused, including observations after a write. */
    int (*enter)(void *, uint64_t deadline);
    void (*leave)(void *);
    int (*now)(void *, uint64_t *microseconds);
    int (*read)(void *, uintptr_t, uint32_t *);
    int (*write)(void *, uintptr_t, uint32_t);
};
struct a733_pin_state {
    uint64_t id, owner;
    struct a733_pin_config initial, current;
    uint8_t initial_output, current_output, initial_irq, current_irq;
    uint8_t irq_enabled, banked;
};
/* Implementation state; zero-initialize once and never modify initialized
 * fields. All managers sharing these pins must be the SAME instance. */
/* The platform authenticates this complete bank-latch record before init.
 * It excludes all other DATA writers for the manager lifetime. Values sampled
 * while pins are inputs or alternate functions cannot establish this record. */
struct a733_pin_latch_handoff {
    uint64_t identity, owner;
    uint32_t known_mask, value;
};
struct a733_pin_handoff {
    uint32_t authorized[A733_PIN_BANKS];
    /* Exclusive authority over bank-wide interrupt debounce; all interrupt
     * clients in each authorized bank must use this manager. */
    uint32_t irq_banks;
    struct a733_pin_latch_handoff latch[2]; /* L, M; zero means unavailable. */
};
struct a733_pin_bank_state {
    uint32_t irq_pins, initial_debounce, debounce;
    uint32_t latch;
};
struct a733_pins {
    struct a733_pins_operations operations;
    void *cookie;
    uint64_t next_id, last_time;
    uint32_t authorized_pb_mask;
    unsigned initialized, failed;
    struct a733_pin_handoff handoff;
    struct a733_pin_bank_state banks[A733_PIN_BANKS];
    struct a733_pin_state pins[A733_PIN_BANKS][32];
};
/* No MMIO. authorized_pb_mask is an explicit platform handoff of unowned pins,
 * not a discovery of unused pins. PIO clock, bank voltage, stable mappings,
 * operation-specific recovery and quiescent peripheral functions are
 * prerequisites. Image admission may authorize supervised RAM-only retention
 * until manual full power removal; it does not recover stalled MMIO or replace
 * stricter recovery requirements of an existing image. */
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
/* Bank-qualified API. Legacy PB functions above keep their input/alternate/
 * disabled admission. Both APIs share this manager, lock, and token sequence.
 * The enlarged manager is a source-compatible, not binary-compatible, change:
 * rebuild every embedding consumer and link a733_pin_tables.c. */
struct a733_pin_request_banked {
    unsigned bank, pin;
    struct a733_pin_config expected, desired;
    uint8_t output_value; /* 0/1, only for desired GPIO output (function 1). */
    uint8_t irq_mode; /* 0 rising, 1 falling, 2 high, 3 low, 4 both edges. */
    uint8_t debounce; /* bit 0: 0 RTC32K, 1 SYS24M; bits 6:4: divide by 2^n. */
};
struct a733_pin_lease_banked {
    uint64_t id, owner;
    uint32_t masks[A733_PIN_BANKS];
};
struct a733_pin_descriptor {
    uint16_t functions;
    /* 1..7 UART0..6, 8..9 S_UART0..1; zero means no UART route. */
    uint8_t uart[16];
    /* Explicit manual-backed route set; zero refuses an unproven UART route. */
    uint8_t uart_group[16];
};
const struct a733_pin_descriptor *a733_pin_descriptor(unsigned bank, unsigned pin);
uint32_t a733_pin_bank_mask(unsigned bank);
int a733_pin_fields_banked(unsigned bank, unsigned pin, struct a733_pin_fields *);
int a733_pin_config_valid_banked(unsigned bank, unsigned pin, struct a733_pin_config);
int a733_pins_init_banks(struct a733_pins *, const struct a733_pins_operations *,
    void *, const struct a733_pin_handoff *);
enum a733_pins_result a733_pins_acquire_banked(struct a733_pins *, uint64_t owner,
    const struct a733_pin_request_banked *, size_t, struct a733_pin_lease_banked *, uint64_t deadline);
/* Fixed PWM0 fan stop: the existing exact PD5 lease remains owned. After
 * independently proving its PWM producer quiescent, transition mux7 to GPIO
 * output-low through disable/preload/mux ordering. No arbitrary rerouting.
 * Main PIO atomic clear preserves neighboring DATA latches. Already-low calls
 * still verify actual state. Any uncertain write retains the token and faults
 * the manager. Physical gate voltage/discharge and fan-off remain qualified
 * separately; digital readback alone is not an analog off-current guarantee. */
enum a733_pins_result a733_pins_pwm0_fan_low(struct a733_pins *,
    const struct a733_pin_lease_banked *,unsigned users_quiescent,uint64_t deadline);
/* Additional inherited GPIO-output conditions for checked acquisition. Each
 * mask bit selects a requested pin with expected AND desired function 1.
 * At least one bit must be selected; levels outside masks are invalid. */
struct a733_pin_output_condition {
    uint32_t masks[A733_PIN_BANKS], levels[A733_PIN_BANKS];
};
/* All input objects and the initially empty output token must be mutually
 * disjoint and disjoint from manager storage, and remain stable for the call.
 * The condition is required and copied before locking. Its inherited levels
 * are checked from the acquisition's DATA observations while holding the
 * shared register lock, before reserving pins or writing hardware. A mismatch
 * returns CONFLICT with an empty token and no hardware writes. Read failures
 * and deadlines retain the ordinary acquisition semantics. B–K conditions
 * observe driven GPIO DATA levels. L/M also require agreement with the
 * authenticated bank-latch shadow. Neither proves an external device's state. */
enum a733_pins_result a733_pins_acquire_banked_checked(struct a733_pins *, uint64_t owner,
    const struct a733_pin_request_banked *, size_t,
    const struct a733_pin_output_condition *, struct a733_pin_lease_banked *, uint64_t deadline);
enum a733_pins_result a733_pins_release_banked(struct a733_pins *,
    struct a733_pin_lease_banked *, unsigned users_quiescent, uint64_t deadline);
enum a733_pins_result a733_pins_read_inputs_banked(struct a733_pins *,
    const struct a733_pin_lease_banked *, unsigned bank, uint32_t mask,
    uint32_t *levels, uint64_t deadline);
enum a733_pins_result a733_pins_write_outputs(struct a733_pins *,
    const struct a733_pin_lease_banked *, unsigned bank, uint32_t mask,
    uint32_t levels, uint64_t deadline);
/* IRQ acquisition leaves lines masked. Bind the corresponding GIC handler
 * before enabling. Pending reads and W1C acknowledge touch only leased EINTs.
 * Release requires the handler and peripheral producers to be quiescent. */
enum a733_pins_result a733_pins_irq_enable(struct a733_pins *,
    const struct a733_pin_lease_banked *, unsigned bank, uint32_t mask,
    unsigned enable, uint64_t deadline);
enum a733_pins_result a733_pins_irq_pending(struct a733_pins *,
    const struct a733_pin_lease_banked *, unsigned bank, uint32_t mask,
    uint32_t *pending, uint64_t deadline);
enum a733_pins_result a733_pins_irq_ack(struct a733_pins *,
    const struct a733_pin_lease_banked *, unsigned bank, uint32_t mask, uint64_t deadline);
enum a733_pins_result a733_pins_held_banked(struct a733_pins *,
    const struct a733_pin_lease_banked *, int *held, uint64_t deadline);
#endif
