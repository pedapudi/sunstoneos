#ifndef SUNSTONE_HDMI_AUDIO_USB_AUTHORITY_H
#define SUNSTONE_HDMI_AUDIO_USB_AUTHORITY_H
#include <stdint.h>
#include <stddef.h>
/* Optional copied observation owns USB0 PIO for the same immutable20s session.
 * cold_reference is a733_usb_start_mode=1, not a USB controller role. */
struct hdmi_audio_usb_authority {
    uint64_t owner,handoff;
    uint8_t initial[32],electrical[32],supervision[32];
    uint32_t cold_reference,reserved,supply_uv[4],fabric[3],fabric_hz[3];
};
_Static_assert(sizeof(struct hdmi_audio_usb_authority)==160&&
    offsetof(struct hdmi_audio_usb_authority,cold_reference)==112&&
    offsetof(struct hdmi_audio_usb_authority,fabric)==136,"audio USB authority ABI");
#endif
