/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SUNSTONE_A733_HDMI_H
#define SUNSTONE_A733_HDMI_H
#include "a733_tcon_tv.h"
#define A733_HDMI_EDID_BYTES 512u
enum a733_hdmi_state {
    A733_HDMI_NEW,A733_HDMI_ACQUIRE,A733_HDMI_PREFLIGHT,A733_HDMI_PHY,A733_HDMI_HPD,
    A733_HDMI_DDC_INIT,A733_HDMI_EDID,A733_HDMI_CONTROLLER,
    A733_HDMI_TV,A733_HDMI_PREPARED,A733_HDMI_STARTING,
    A733_HDMI_ACTIVE,A733_HDMI_STOPPING,A733_HDMI_STOPPED_RETAINED,
    A733_HDMI_FAULT_RETAINED
};
enum a733_hdmi_error { A733_HDMI_OK,A733_HDMI_AUTHORITY,A733_HDMI_IO,
    A733_HDMI_SINK,A733_HDMI_EDID_INVALID,A733_HDMI_DDC_ERROR,
    A733_HDMI_UNDERFLOW };
struct a733_hdmi_config {
    struct a733_hdmi_ops ops;
    void *context;
    struct a733_hdmi_request request;
};
struct a733_hdmi {
    struct a733_hdmi *self;
    struct a733_hdmi_io io;
    struct a733_hdmi_phy phy;
    struct a733_tcon_tv tv;
    struct a733_hdmi_timing timing;
    enum a733_hdmi_state state;
    enum a733_hdmi_error error;
    uint64_t until,transaction_deadline;
    uint8_t edid[A733_HDMI_EDID_BYTES];
    uint16_t edid_size,position;
    unsigned index,phase,entered,sink_seen,stop_requested;
};
/* Zero private fixed-address storage once. Config and callback/context storage
 * must be disjoint from the object, live and private. No reentry or object copy.
 * Initial profile must be approved by acquire(), never synthesized defaults.
 * Only HDMI sinks with a valid complete EDID (up to3 extensions) containing
 * HDMI VSDB and the selected CTA VIC are admitted. Default limited-range output
 * uses the actual controller CSC to convert the full-range hardware colorbar.
 * Explicit full-range output additionally requires EDID RGB range selection.
 * No diagnostic override,
 * unknown/oversized EDID fallback, fractional-rate approximation or DVI mode.
 * Preparation programs real PHY/controller/TV0 while TV0 remains disabled.
 * start enables the hardware pattern and then unmutes after source50ms delay.
 * Poll ACTIVE regularly for sampled HPD/PLL/TV health. Unobserved HPD pulses
 * between polls cannot be detected; interrupt hotplug remains required.
 * No state proves physical output, power-down safety or framebuffer drain.
 */
int a733_hdmi_init(struct a733_hdmi *,const struct a733_hdmi_config *);
int a733_hdmi_prepare(struct a733_hdmi *,uint64_t deadline);
int a733_hdmi_start(struct a733_hdmi *,uint64_t deadline);
int a733_hdmi_stop(struct a733_hdmi *,uint64_t deadline);
/* Active health observation with a fresh caller deadline. HPD loss schedules
 * bounded stop under that deadline; an I/O/authority failure retains all state. */
int a733_hdmi_service(struct a733_hdmi *,uint64_t deadline);
/* 0 pending,1 stable state,-1 retained fault. A late fault forbids further I/O;
 * retain retry is metadata only. No automatic reset, restart or lease release. */
int a733_hdmi_poll(struct a733_hdmi *);
int a733_hdmi_edid_accept(const uint8_t *,size_t,enum a733_hdmi_mode,enum a733_hdmi_range);
#endif
