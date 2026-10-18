#ifndef SUNSTONE_BLUETOOTH_IMAGE_PRIVATE_H
#define SUNSTONE_BLUETOOTH_IMAGE_PRIVATE_H
#include "../board/application.h"
#include "bluetooth.h"
extern struct bluetooth_application bluetooth;
extern struct bluetooth_profile bluetooth_profile;
const struct board_radio_chip_id *bluetooth_chip_id(void);
int bluetooth_boot_profile(const struct a733_board_session *,const struct bluetooth_profile *);
#endif
