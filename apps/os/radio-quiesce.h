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
#endif
