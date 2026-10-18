/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SUNSTONE_A733_TYPEC_H
#define SUNSTONE_A733_TYPEC_H
#include <stdint.h>
#include <stdatomic.h>
#include "a733_twi.h"

#define A733_TYPEC_FRESH_US UINT64_C(10000)
#define A733_TYPEC_SERVICE_GAP_US UINT64_C(1000)
struct a733_typec;
/* Copied observation, valid only for this permanent provider and attachment.
 * No USB operation may advance from an expired or revoked observation. */
struct a733_typec_token {
    uintptr_t provider;
    uint64_t owner,generation,observed_us;
    unsigned polarity; /* 1: CC1, 2: CC2. Fixed source/DFP, default USB current. */
};
struct a733_typec_ops {
    int (*outside)(void *,const void *,unsigned long); /* Exclude native private backing. */
    int (*now)(void *,uint64_t *); /* Same concurrent-safe clock as host. */
    int (*servicing)(void *); /* Designated provider task, no interrupt/critical nesting. */
    int (*prepare)(void *,uint64_t); /* Actual native resources and TWI: 1/0/-1. */
    int (*live)(void *); /* Actual provider authority: 1/0/-1. No MMIO. */
    int (*vbus)(void *,unsigned,uint64_t); /* Actual PL8 write/readback: 1/0/-1. */
    int (*irq)(void *,unsigned *,uint64_t); /* Actual PL9 level: 1/0/-1. */
};
struct a733_typec_config {
    struct a733_typec_ops ops;
    void *context;
    struct a733_twi *bus;
    uint64_t owner,generation,qualification,recovery;
    /* Image-qualified electrical facts, not values learned by the token.
     * SY6280 (not A) populated; disabled reverse blocking and discharge qualified.
     * Code0 VBUS comparator upper threshold must be <=800mV across conditions.
     * Board VCC5V and wiring must support default USB current (at least500mA).
     * GPIO/bus initial ownership excludes all remote/other role writers.
     */
    unsigned source_min_mv,source_max_mv,source_ma,zero_threshold_max_mv;
    uint8_t device_id; /* Exact qualified FUSB302MPX version/revision, 0x80..0x82. */
};
enum a733_typec_state {
    A733_TYPEC_NEW,A733_TYPEC_PREPARE,A733_TYPEC_OFF,A733_TYPEC_ID,
    A733_TYPEC_RESET,A733_TYPEC_SETUP,A733_TYPEC_LOOK,A733_TYPEC_MEASURE,
    A733_TYPEC_CLASSIFY,A733_TYPEC_DEBOUNCE,A733_TYPEC_SAFE0,A733_TYPEC_ENABLE,
    A733_TYPEC_RISE,A733_TYPEC_MONITOR,A733_TYPEC_SHUTDOWN,
    A733_TYPEC_DISCHARGE,A733_TYPEC_OFF_RETAINED,A733_TYPEC_UNCERTAIN
};
/* Private platform-lifetime storage. Only the provider task touches service
 * fields. snapshot/validate use immutable fields, an atomic revocation flag and
 * the short publication lock; they never read service fields concurrently.
 * All pointed-to storage stays at its original address and private to this owner.
 */
struct a733_typec {
    struct a733_typec *self;
    struct a733_typec_config config,sealed;
    atomic_uint entered,publishing,revoked;
    atomic_uint_fast64_t progress_until,power_until;
    struct a733_typec_token published;
    uint64_t now,last_service,startup_deadline,transaction_deadline,until,
        candidate_since,shutdown_deadline,cycle_start;
    enum a733_typec_state state,after_measure;
    unsigned step,substep,transaction,measuring,candidate,polarity,cc[2],
        measure_result,classification_step,irq_level,
        enabled,prepared,bus_failed,shutdown_attempted;
    uint8_t value,status,measure_switch,measure_word;
};
/* Zero private storage once, before concurrency. No hardware access. */
int a733_typec_init(struct a733_typec *,const struct a733_typec_config *);
/* One attempt; no automatic reattach, role swap, reset retry or release. */
int a733_typec_begin(struct a733_typec *,uint64_t deadline);
/* Independent provider task continues this call after host failure. 0 pending,
 * 1 active observation, -1 permanently revoked (shutdown may still be pending).
 * Keep polling until OFF_RETAINED or UNCERTAIN; neither permits resource reuse.
 * If I2C faults, GPIO shutdown remains independent of the retained I2C mutex;
 * discharge cannot then be observed and remains uncertain.
 */
int a733_typec_poll(struct a733_typec *);
/* Independent power task may close admission atomically; it never edits service
 * fields. It lowers GPIO through its own actual pin-manager path. */
void a733_typec_revoke(struct a733_typec *);
/* Concurrent host-side metadata: 1 live,0 publication busy/not attached,-1 lost.
 * Output stays unchanged on refusal. Caller storage must be private/disjoint.
 * The first accepted generation is pinned by the host; a new one is never
 * silently adopted. Clock failure, expiry or identity mismatch revokes access.
 */
int a733_typec_snapshot(struct a733_typec *,struct a733_typec_token *,uint64_t deadline);
int a733_typec_validate(struct a733_typec *,const struct a733_typec_token *,uint64_t deadline);
#endif
