#ifndef SUNSTONE_HDMI_AUDIO_USB_H
#define SUNSTONE_HDMI_AUDIO_USB_H
#include "usb-authority.h"
#include "a733_usb_platform.h"
#include "a733_pmic.h"
struct hdmi_audio_usb {
    const struct hdmi_audio_usb_authority *authority;
    struct a733_usb_platform platform;
    struct a733_rail_token rail;
    struct a733_pmic_lifetime power;
    struct a733_pmic_request request;
    struct a733_pmic_snapshot snapshots[4];
    unsigned step,rail_index;
    atomic_uint ready;
    uint8_t session_sha256[32];
};
extern struct hdmi_audio_usb usb;
int hdmi_audio_usb_init(void);
void hdmi_audio_usb_task(void *);
#endif
