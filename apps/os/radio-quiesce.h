#ifndef SUNSTONE_OS_RADIO_QUIESCE_H
#define SUNSTONE_OS_RADIO_QUIESCE_H
#include <stdint.h>
struct board_radio;
/* The radio task owns shutdown. The client stops starting network work once
 * the return request reaches this owner. */
int os_radio_quiesce_requested(void);
int os_radio_quiesce_parked(void);
/* 0 running, 1 shutdown pending, 2 parked after completion or fault. */
int os_radio_quiesce_poll(struct board_radio *,int result);
int os_radio_quiesce_fault(uint32_t value,const void *caller);
/* 1 when a failed scan station has closed: its failure shutdown read back every step (PM1 low, SDC1 interrupt
 * retired, module and bus clocks gated with the reset asserted, BLDO5 off and its request reaped), and no Bluetooth
 * client shares the module. Such a fault returns inside the record instead of publishing FAULT. */
int os_radio_failure_closed(const struct board_radio *);
#endif
