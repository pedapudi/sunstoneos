#ifndef SUNSTONE_A733_DISPLAY_MODES_H
#define SUNSTONE_A733_DISPLAY_MODES_H
#include "sunstone_display.h"
#include "a733_dsi_video.h"
#include "a733_hdmi_phy.h"
/* No clock selection occurs here. These exact transport rates must already
 * be authorized by the existing clock/rail/profile owners. */
static inline struct sunstone_display_mode a733_dsi_pixel_mode(const struct a733_dsi_video_timing *t)
{
    return (struct sunstone_display_mode){t->pixel_hz,t->width,t->height,t->hfront,t->hsync,
        t->hback,t->vfront,t->vsync,t->vback,0};
}
static inline int a733_dsi_timing_from_mode(const struct sunstone_display_mode *m,
    uint32_t hs,uint32_t ls,unsigned lanes,enum a733_dsi_video_format format,
    enum a733_dsi_video_mode video,struct a733_dsi_video_timing *out)
{
    if(!out||!sunstone_display_mode_valid(m)||(m->flags&~SUNSTONE_MODE_PREFERRED))return 0;
    *out=(struct a733_dsi_video_timing){m->pixel_hz,hs,ls,m->width,m->height,m->hsync,
        m->hback,m->hfront,m->vsync,m->vback,m->vfront,lanes,format,video};return 1;
}
/* Hardware profile admission is independent of EDID parsing. HDMI currently
 * has exact analog tables for integer 720p60/1080p60; DP has only 720p60. */
enum sunstone_display_mode_status a733_hdmi_mode_from_pixel(const struct sunstone_display_mode *,enum a733_hdmi_mode *);
enum sunstone_display_mode_status a733_dp_mode_valid(const struct sunstone_display_mode *);
enum sunstone_display_mode_status a733_dsi_panel_timing(const struct sunstone_display_port *,
    const struct sunstone_display_mode *,uint32_t hs,uint32_t ls,struct a733_dsi_video_timing *);
#endif
