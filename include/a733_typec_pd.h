/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SUNSTONE_A733_TYPEC_PD_H
#define SUNSTONE_A733_TYPEC_PD_H
#include <stdint.h>
struct a733_typec;
/* Copied metadata. Pin C=4 or E=16, four DP lanes. HPD comes from PD status,
 * never the unconnected A733 HPD pad. COMB0 ownership belongs to the consumer. */
struct a733_typec_dp_token {
    uintptr_t provider;
    uint64_t owner,generation,observed_us,hpd_irq; /* HPD event sequence: level changes or IRQ. */
    unsigned polarity,pin_assignment,hpd;
};
/* Private to the designated Type-C task; publication uses the CC provider lock. */
struct a733_typec_pd {
    uint64_t deadline,wait_until,transition_until,hpd_irq;
    unsigned setup,phase,transaction,length,tx_length,rx_length,state,after_tx,
        tx_wait,tx_id,rx_id,rx_seen,command,mode,pin_assignment,hpd,ready;
    uint8_t data[40],tx[40],rx[40];
};
/* Native PD2 fixed source/DFP profile: no swaps, VCONN or cable messages.
 * Enable by config.displayport=1 before a733_typec_init. The same native task
 * runs CC and PD; native independent GPIO shutdown remains mandatory. */
int a733_typec_dp_snapshot(struct a733_typec *,struct a733_typec_dp_token *,uint64_t);
/* Validate against a fresh current provider observation; acquisition timestamp
 * does not expire an unchanged mode while CC service keeps renewing freshness.
 * HPD/IRQ change returns -1 without revoking CC; snapshot again to observe it. */
int a733_typec_dp_validate(struct a733_typec *,const struct a733_typec_dp_token *,uint64_t);
/* Internal service: 0 same I2C transaction pending, 1 slice finished, -1 retained
 * failure. It borrows the already-owned native TWI; never creates authority. */
int a733_typec_pd_service(struct a733_typec *);
#endif
