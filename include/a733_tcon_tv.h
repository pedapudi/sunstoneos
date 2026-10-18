/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SUNSTONE_A733_TCON_TV_H
#define SUNSTONE_A733_TCON_TV_H
#include "a733_hdmi_phy.h"
struct a733_tcon_tv {
    struct a733_tcon_tv *self;
    struct a733_hdmi_io *io;
    struct a733_hdmi_timing timing;
    unsigned index,prepared,started,stopped;
};
/* The first path uses the real TV0 hardware color-check pattern, RGB888,
 * strict timing, one pixel/clock. It does not access DE or any framebuffer. */
int a733_tcon_tv_init(struct a733_tcon_tv *,struct a733_hdmi_io *);
int a733_tcon_tv_prepare(struct a733_tcon_tv *);
/* Caller must hold the same complete VO1 reservation and have completed the
 * actual PHY/controller preparation. Composition invokes start only then. */
int a733_tcon_tv_start(struct a733_tcon_tv *);
int a733_tcon_tv_stop(struct a733_tcon_tv *);
int a733_tcon_tv_health(struct a733_tcon_tv *);
#endif
