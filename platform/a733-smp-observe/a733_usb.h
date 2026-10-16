#ifndef SUNSTONE_OBSERVE_USB_H
#define SUNSTONE_OBSERVE_USB_H
#include <stdbool.h>
#include <stdint.h>
void a733_usb_prepare(void);
void a733_usb_poll_enable(bool);
void a733_usb_poll(void);
void a733_usb_trace(uint32_t,uint32_t,uint32_t);
void a733_usb_assert(void);
#endif
