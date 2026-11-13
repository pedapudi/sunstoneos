#ifndef SUNSTONE_A733_RADIO_EVENTS_H
#define SUNSTONE_A733_RADIO_EVENTS_H
#include "a733_radio_platform.h"
#include "FreeRTOS.h"
#include "task.h"
#include "gicv3_spi.h"

/* The generated image owns the level NS Group 1 route at API-safe priority.
 * Storage and the owner task survive every normal and failed retirement. */
struct a733_radio_events {
    struct a733_radio_platform *platform;
    TaskHandle_t owner;
    struct gicv3_spi irq;
    atomic_uint pending,failed;
    unsigned initialized,started,registered,masked,retired;
    uint64_t wakes,interrupts,deadline_wakes;
};
int a733_radio_events_init(struct a733_radio_events *,struct a733_radio_platform *,TaskHandle_t);
/* Owner-only route validation before the platform starts. No route mutation. */
int a733_radio_events_begin(struct a733_radio_events *);
void a733_radio_events_irq(struct a733_radio_events *);
/* Arm/recheck without blocking, for the existing owned socket worker. */
int a733_radio_events_prepare(struct a733_radio_events *,uint64_t *until);
/* The caller checks transport software work before this call. until includes
 * its immutable lifetime and protocol timers. 1 woke/work, 0 fault. */
int a733_radio_events_wait(struct a733_radio_events *,uint64_t until);
/* Platform hook. 1 disabled, source quiet, inactive and no pending state;
 * 0 propagation/active state still pending; -1 retained failure. */
int a733_radio_events_retire(void *);
#endif
