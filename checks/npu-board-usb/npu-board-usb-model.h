#ifndef SUNSTONE_NPU_BOARD_USB_MODEL_H
#define SUNSTONE_NPU_BOARD_USB_MODEL_H
/* Compiles apps/npu-board/usb.c into checks/npu-session-runtime.c, case 5.
 * The check supplies a modeled TinyUSB vendor class with the buffering of the
 * image's own configuration, apps/npu-board/tusb_config.h, which this header
 * includes: FIFO sizes from that file and no dedicated hardware FIFO, so received bytes go
 * controller FIFO -> endpoint buffer (PIO) -> receive FIFO -> caller, and
 * transmitted bytes go caller -> transmit FIFO -> endpoint buffer -> controller
 * FIFO (PIO). It counts every copy. */
#include "../../apps/npu-board/client.h"
#include "../../apps/npu-board/tusb_config.h"
#include <stdbool.h>
#include <stdint.h>
extern struct npu_board_client board_client;
extern const uint64_t npu_board_usb_epoch, npu_board_usb_generation;
_Noreturn void npu_board_usb_stop(void);
#define CLIENT (&board_client)
#define SESSION_EPOCH npu_board_usb_epoch
#define SESSION_GENERATION npu_board_usb_generation
enum { TUSB_SPEED_FULL = 0, TUSB_SPEED_HIGH = 2 };
int tud_speed_get(void);
uint32_t tud_vendor_available(void);
uint32_t tud_vendor_read(void *buffer, uint32_t bytes);
uint32_t tud_vendor_write(const void *buffer, uint32_t bytes);
uint32_t tud_vendor_flush(void);
void tud_vendor_rx_cb(uint8_t interface, const uint8_t *buffer, uint32_t bytes);
void tud_vendor_tx_cb(uint8_t interface, uint32_t bytes);
void tud_vendor_tx_complete_cb(uint8_t interface);
/* One pass of usb.c's USB0 loop callback. */
void npu_board_usb_exchange(void);
#endif
