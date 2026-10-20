#ifndef SUNSTONE_GPU_DISPLAY_USB_AUTHORITY_H
#define SUNSTONE_GPU_DISPLAY_USB_AUTHORITY_H
#include <stdint.h>
/* Authenticated USB0 PIO takeover within the shared display lifetime. These
 * identities supplement the existing HDMI fabric, pin and PMIC handoff. */
struct gpu_display_usb_authority {
    uint64_t owner, handoff;
    uint8_t initial[32], electrical[32], supervision[32];
    uint32_t cold_reference, reserved;
    uint32_t supply_uv[4];
};
_Static_assert(sizeof(struct gpu_display_usb_authority)==136,"USB observation authority");
const struct gpu_display_usb_authority *gpu_display_usb_get(void);
#endif
