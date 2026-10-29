#ifndef SUNSTONE_BOARD_AUDIO_H
#define SUNSTONE_BOARD_AUDIO_H
#include "a733_usb2_native.h"
#define BOARD_AUDIO_BYTES 1088u
/* One historical whole-frame block; padding is zero and no device is stopped. */
int board_audio_encode(uint8_t[BOARD_AUDIO_BYTES],const struct a733_usb2_audio_block *);
#endif
