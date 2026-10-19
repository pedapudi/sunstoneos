/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SUNSTONE_USB3_PHY_PRIVATE_H
#define SUNSTONE_USB3_PHY_PRIVATE_H
#include "a733_usb2_platform.h"
int a733_usb3_phy_poll(struct a733_usb2_platform *);
uint32_t a733_usb3_mask32(uintptr_t);
int a733_usb3_address16(uintptr_t);
int a733_usb3_read32(struct a733_usb2_platform *,uintptr_t,uint32_t *);
int a733_usb3_write32(struct a733_usb2_platform *,uintptr_t,uint32_t);
int a733_usb3_read16(struct a733_usb2_platform *,uintptr_t,uint16_t *);
int a733_usb3_write16(struct a733_usb2_platform *,uintptr_t,uint16_t);
#endif
