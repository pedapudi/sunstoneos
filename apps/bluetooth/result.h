#ifndef SUNSTONE_BLUETOOTH_RESULT_H
#define SUNSTONE_BLUETOOTH_RESULT_H
#include "bt_hci.h"
#define BLUETOOTH_RESULT_MAX 177u
/* State0 pending,1 identified,2 retained failure. Only identified carries the
 * copied six-command result. Output is unchanged on invalid input/alias. */
int bluetooth_result_frame(uint8_t [BLUETOOTH_RESULT_MAX],size_t *,unsigned state,
    uint64_t epoch,uint64_t generation,const uint8_t profile[32],
    uint64_t firmware_generation,uint64_t uart_generation,const struct bt_hci_identity *);
#endif
