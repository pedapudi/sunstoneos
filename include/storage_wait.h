#ifndef SUNSTONE_STORAGE_WAIT_H
#define SUNSTONE_STORAGE_WAIT_H
/* Drive a polled storage operation without spinning: after each poll that did not finish, sleep on the event the core
 * says will end the wait (docs/sd-card.md). The wake source is a task notification: the controller's
 * interrupt handler gives it, and a one-shot deadline timer bounds every sleep. Every wake, however it came, is only a
 * hint; the next poll reads the controller's status. */
#include <stdint.h>
#include "sunxi_mmc.h"

struct storage_wait_ops {
    void *context;
    int (*poll)(void *);                           /* one poll; nonzero once the operation has finished (either way) */
    enum sunxi_mmc_wait (*hint)(void *, uint32_t *delay_us);
    uint64_t (*now_us)(void *);
    void (*sleep_until)(void *, uint64_t absolute_us);   /* returns on a notification or when the time is reached */
    int (*live)(void *);                           /* 0 when the image is going down */
    uint32_t recheck_us;                           /* the longest an interrupt wait sleeps before it re-polls; 0 selects the default */
};
/* An interrupt that never arrives must not hang an operation: an IRQ wait is re-polled after its recheck. The recheck is a safety net, not a
 * poll: the caller sizes it from the request (the expected transfer at the measured bus rate and the card's write-busy allowance, with a margin), so
 * that a normal completion always comes first and every timer wake of an interrupt wait is a real miss. This is the floor and the default. */
#define STORAGE_WAIT_IRQ_RECHECK_US 20000u
/* Returns the nonzero value poll returned, or -1 when the image stopped being live or the deadline passed. */
int storage_wait_run(const struct storage_wait_ops *, uint64_t deadline_us);
#endif
