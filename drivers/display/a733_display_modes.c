#include "a733_display_modes.h"
#include "a733_tcon_lcd.h"
#include "a733_dsi_phy.h"
enum sunstone_display_mode_status a733_dsi_panel_timing(const struct sunstone_display_port *port,
    const struct sunstone_display_mode *mode,uint32_t hs,uint32_t ls,struct a733_dsi_video_timing *out)
{
    if(!port||port->connector!=SUNSTONE_DISPLAY_DSI||port->controller!=1||!out||
       !sunstone_display_mode_valid(mode))return SUNSTONE_MODE_INVALID;
    const struct sunstone_panel *p=port->panel;
    if(!sunstone_panel_valid(p))return SUNSTONE_MODE_NO_PANEL;
    if(mode->flags&SUNSTONE_MODE_INTERLACED)return SUNSTONE_MODE_INTERLACE;
    if(mode->width>1920||mode->height>1080)return SUNSTONE_MODE_GEOMETRY;
    if(port->lanes!=4||p->lanes!=port->lanes)return SUNSTONE_MODE_LANES;
    for(unsigned i=0;i<port->lanes;++i)if(port->lane_map[i]!=i)return SUNSTONE_MODE_LANES;
    unsigned found=0;
    for(size_t i=0;i<p->mode_count;++i)found|=sunstone_display_mode_equal(&p->modes[i],mode);
    if(!found)return SUNSTONE_MODE_LINK;
    /* The current host supports continuous clock and EOT packets only. */
    if(p->flags&(SUNSTONE_PANEL_NONCONTINUOUS_CLOCK|SUNSTONE_PANEL_NO_EOT))return SUNSTONE_MODE_UNSUPPORTED_PROFILE;
    if(!(p->flags&(SUNSTONE_PANEL_SYNC_PULSE|SUNSTONE_PANEL_BURST)))return SUNSTONE_MODE_UNSUPPORTED_PROFILE;
    struct a733_tcon_lcd_timing t={.actual_dclk_hz=ls};
    if(!a733_dsi_timing_from_mode(mode,hs,ls,p->lanes,
       p->format==SUNSTONE_PANEL_RGB888?A733_DSI_RGB888:A733_DSI_RGB666_LOOSE,
       p->flags&SUNSTONE_PANEL_BURST?A733_DSI_BURST:A733_DSI_SYNC_PULSE,&t.dsi))return SUNSTONE_MODE_UNSUPPORTED_PROFILE;
    struct a733_dsi_pll_plan pll;
    if(!a733_dsi_pll_plan(hs,ls,&pll))return SUNSTONE_MODE_CLOCK;
    struct a733_tcon_lcd_plan lcd;
    if(!a733_tcon_lcd_plan(&t,&lcd))return SUNSTONE_MODE_UNSUPPORTED_PROFILE;
    *out=t.dsi;return SUNSTONE_MODE_OK;
}
