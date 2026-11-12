/* Fail only after both existing empty-port owners reach their checkpoints. */
#include "usb_empty_fail-app.h"

void usb_empty_fail_main(const struct ss_grant *grant)
{
    if (grant->devices != SS_DEVICE_USB_KEYBOARD || grant->region_count) ss_fail(0x791);
    ss_fail_value(0x790, 3);
}
