/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SUNSTONE_A733_HDMI_AUDIO_H
#define SUNSTONE_A733_HDMI_AUDIO_H
#include "a733_hdmi.h"
/* Fixed PCM sampler for the internal I2S3 route. The sole HDMI task serializes
 * every call with controller service. Caller separately owns I2S3 and DMA.
 * Hardware auto-CTS follows the actual TMDS clock; no nominal CTS is supplied.
 * All reservations survive mute, completion and failure. No wire drain API. */
struct a733_hdmi_audio {
    struct a733_hdmi_audio *self;
    struct a733_hdmi *hdmi;
    uint64_t until;
    unsigned stage,index,ready,failed;
};
int a733_hdmi_audio_edid(const uint8_t *,size_t);
int a733_hdmi_audio_init(struct a733_hdmi_audio *,struct a733_hdmi *);
int a733_hdmi_audio_prepare(struct a733_hdmi_audio *); /*0 pending,1 ready,-1 retained*/
int a733_hdmi_audio_mute(struct a733_hdmi_audio *,unsigned);
#endif
