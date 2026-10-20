/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SUNSTONE_A733_TYPEC_NATIVE_H
#define SUNSTONE_A733_TYPEC_NATIVE_H
#include "a733_typec.h"
#include "a733_pmic.h"
struct a733_platform_locks;
struct a733_typec_native_config {
    struct a733_clock_leases *clocks;
    struct a733_pins *pins;
    struct a733_rail_leases *rails;
    struct a733_pmic *pmic;
    struct a733_platform_locks *locks;
    void *provider_task,*power_task; /* Different permanent FreeRTOS tasks. */
    struct a733_typec_config electrical; /* ops/context/bus supplied natively. */
    struct a733_pin_config expected[4]; /* PL8,PL9,PL12,PL13, in this order. */
};
struct a733_typec_native {
    struct a733_typec_native *self;
    struct a733_typec_native_config config,sealed;
    struct a733_typec provider;
    struct a733_twi bus;
    struct a733_twi_ops bus_ops;
    struct a733_clock_token clock;
    struct a733_pin_lease_banked pin;
    struct a733_rail_token rail;
    struct a733_pmic_lifetime pmic_lifetime;
    struct a733_pmic_request requests[2];
    struct a733_pmic_snapshot supplies[2];
    struct a733_pin_request_banked pin_requests[4];
    atomic_uint gpio_ready,command,result,output_state;
    atomic_uint_fast64_t command_deadline;
    uint64_t deadline,last_now,power_now,power_shutdown_deadline;
    uint32_t divider,rate_hz,input_hz;
    unsigned stage,index,held,bus_fault,setup_fault,power_fault;
};
/* One fixed TYPEC24 boot owner; zero private storage once before scheduling.
 * Physical qualification includes exact SY6280 population, source capacity,
 * comparator bounds, independent recovery, remote exclusion, Device mappings,
 * EL1 security access, and initialized bank-L latch authority in the pin manager.
 * APBS1 must already select raw DCXO with divider1; no shared parent retune.
 * Derive its24/19.2/26MHz rate from the enabled/stable XO_CTRL0 image.
 * DCDC1/DC1SW2 must be enabled at3.3V. Selected TWI gate/reset start asserted/off.
 * The PMIC provider has its independent task. This function performs no MMIO.
 */
int a733_typec_native_init(struct a733_typec_native *,const struct a733_typec_native_config *);
/* Separate power task, serviced at least every1ms. It consumes bounded atomic
 * requests, observes PL9, and disables PL8 on provider revocation or expired
 * real progress/attachment observations. xHCI never renews these deadlines.
 * It needs no I2C access, even when the provider task retains the I2C mutex.
 * Pre-effect metadata/pin contention revokes admission and permits at most5ms
 * of pending shutdown. Uncertain writes and actual authority loss are terminal.
 * A clock, pin, register-mutex or MMIO failure cannot prove VBUS off: resources
 * remain retained and independent whole-system recovery is then required.
 * Return1 healthy,0 initialization pending,-1 permanently revoked. Continue
 * polling after -1 while power_fault is zero and output_state is not1: a
 * revoked provider can still have a bounded GPIO shutdown pending. Read
 * power_fault only from this designated task; output_state is atomic.
 * A power_fault is terminal.
 */
int a733_typec_native_power_poll(struct a733_typec_native *);
#endif
