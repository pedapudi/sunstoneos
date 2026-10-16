#ifndef SUNSTONE_A733_USB_H
#define SUNSTONE_A733_USB_H
#include <stdbool.h>
#include <stdint.h>
void a733_usb_prepare(void);
void a733_usb_poll_enable(bool enable);
void a733_usb_poll(void);
void a733_usb_trace(uint32_t event, uint32_t first, uint32_t second);
#endif
