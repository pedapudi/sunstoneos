/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SUNSTONE_BOARD_BUNDLES_POWER_H
#define SUNSTONE_BOARD_BUNDLES_POWER_H
/* The PMIC provider, the power-domain owner and the one service task of
 * bundles 6 and 7 (docs/designs/board-bundles-next.md), on the returning
 * single-core platform of rank-9-usb-v4. Initialization follows the
 * provider path that rank-9-usb-v4 ran (platform/a733/native_usb.c), without
 * the USB provider.
 *
 * Both callback tables are the production native adapters with a recording
 * layer between them and their users: every S_TWI0 access of the provider
 * passes the TWI observer, and every access of the owner is stored in an
 * access trace with its CNTPCT stamp. The layer adds no register access, with
 * one exception that only bundle 7 enables: after the owner's ON command to
 * domain 1, the domain's status is read back to back until it reads ON or
 * 10 ms pass, before the owner continues. */
#include <stdatomic.h>
#include "FreeRTOS.h"
#include "task.h"
#include "a733_counter_clock.h"
#include "a733_platform_locks.h"
#include "a733_pins_native.h"
#include "a733_gpu_resources_native.h"
#include "a733_pmic_native.h"
#include "a733_power_domains_native.h"
#include "trace.h"

#define BB_POWER_PMIC_OWNER 2u
#define BB_POWER_CLIENT_OWNER 3u
#define BB_POWER_STEPS 120u
#define BB_POWER_ACCESSES 64u
#define BB_POWER_STATUS_BOUND (24000000u / 100u) /* 10 ms of CNTPCT */

/* One service step that advanced a bus phase. */
struct bb_power_step {
    uint64_t ticks;                    /* CNTPCT across the step */
    uint32_t interrupts;               /* scheduler ticks inside it */
    uint16_t conditions, bytes, stops, first_set, first_clear, polls_max;
};
struct bb_power_access { uint32_t address, value, write; uint64_t at; };
struct bb_power {
    struct a733_counter_clock counter;
    struct a733_platform_locks locks;
    struct a733_clock_leases clocks;
    struct a733_pins_native pin_native;
    struct a733_pin_handoff handoff;
    struct a733_pins pins;
    struct a733_gpu_resource_native gpu_native;
    struct a733_gpu_resource_manager gpu;
    struct a733_rail_leases rails;
    struct a733_pmic_native pmic_native;
    struct a733_pmic pmic;
    struct a733_power_domains_native domains_native;
    struct a733_power_domains domains;
    struct a733_power_domain_row row;  /* domain 1, owner BB_POWER_CLIENT_OWNER */
    struct a733_rail_token token;      /* the client's rail token, which the row names */
    TaskHandle_t service, client;
    uint64_t deadline;                 /* microseconds of the PMIC clock */
    struct bb_twi twi;
    struct bb_power_step steps[BB_POWER_STEPS];
    unsigned stepped, steps_dropped;
    struct bb_power_access accesses[BB_POWER_ACCESSES];
    unsigned accessed, writes;
    unsigned spin_after_on;            /* bundle 7 only */
    struct bb_status_trace status;
    uint64_t status_start;
    atomic_uint stop, parked;
};
extern struct bb_power bb_power;

/* Before the scheduler: every manager, both tasks (the service at priority
 * 2, client at priority 1), the provider with S_TWI0 pins PL0 and PL1 at
 * function 2, pull 1 and drive 1 (the rank-9-usb-v4 input), the owner, and
 * the domain-1 row naming bb_power.token. deadline_us bounds the provider's
 * startup and the service session. Returns 1 on success. */
int bb_power_init(uint64_t deadline_us, TaskFunction_t client, const char *name);
/* Client task helpers. */
uint64_t bb_power_now(void);             /* PMIC clock microseconds */
void bb_power_wait_ready(void);          /* provider READY and supply verification done, or FAIL */
void bb_power_acquire(uint64_t rails);   /* the client's shared token of these rails, or FAIL */
void bb_power_park(void);                /* service idle and parked, or FAIL */
int bb_power_read(uintptr_t, uint32_t *);/* one owner-allowlisted read, through the trace */
_Noreturn void bb_power_fail(const char *);
void bb_power_put(const char *name, uint64_t value);  /* name and decimal value */
void bb_power_put_hex(const char *name, uint64_t value);
void bb_power_report_steps(void);
void bb_power_report_accesses(void);
#endif
