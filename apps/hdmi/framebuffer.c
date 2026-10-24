/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "application.h"
#include "framebuffer.h"
#include <string.h>
struct a733_display_framebuffer framebuffer;
static struct hdmi_framebuffer_session sealed;
static int current(void *v)
{return v==&app&&hdmi_live(NULL)&&xTaskGetCurrentTaskHandle()==app.handles[1]&&
    !memcmp(&sealed,&app.session.framebuffer,sizeof(sealed));}
static int output_owned(void *v)
{
    return v==&app&&a733_clock_token_status(&app.clocks,&app.hdmi.clock)==A733_CLOCK_OK&&
        a733_rail_status(&app.rails,&app.hdmi.rail)==A733_RAIL_READY&&
        a733_pmic_live(&app.pmic,&app.hdmi.pmic_lifetime)==A733_PMIC_READY&&
        a733_gpu_resources_status(&app.pck,app.session.pck_client,&app.hdmi.pck_lease)==A733_GPU_RESOURCE_OK;
}
static int admit(void *v,enum a733_de_action action)
{
    enum a733_hdmi_state h=app.hdmi.controller.state;
    if(v!=&app)return 0;
    if(action==A733_DE_PREPARE||action==A733_DE_START)return h==A733_HDMI_PREPARED;
    if(action==A733_DE_STOP)return h==A733_HDMI_STOPPED_RETAINED;
    return h==A733_HDMI_ACTIVE;
}
int hdmi_framebuffer_boot(void)
{
    if(!hdmi_framebuffer_session_valid(&app.session.framebuffer))return 0;
    sealed=app.session.framebuffer;
    struct a733_display_framebuffer_config c={.counter=&app.counter,.clocks=&app.clocks,
        .locks=&app.locks,.pck=&app.pck,.pck_lease=&app.hdmi.pck_lease,
        .pck_client=app.session.pck_client,.owner=app.session.owner,.authority=sealed.authority,
        .deadline=app.deadline,.parent_bytes=2097152,.frame_span=2097152-32768,
        .output=A733_DE_TV0_RGB565,.context=&app,.current=current,.output_owned=output_owned,.admit=admit,
        .domains=&app.domains,.rail=&app.hdmi.rail};
    memcpy(c.fabric,sealed.fabric,sizeof(c.fabric));memcpy(c.fabric_hz,sealed.fabric_hz,sizeof(c.fabric_hz));
    memcpy(c.iommu_gates,sealed.iommu_gates,sizeof(c.iommu_gates));
    return a733_display_framebuffer_boot(&framebuffer,&c,1);
}
int hdmi_framebuffer_prepare(void)
{return app.hdmi.controller.state==A733_HDMI_PREPARED?a733_display_framebuffer_prepare(&framebuffer):-1;}
int hdmi_framebuffer_stop(void)
{return app.hdmi.controller.state==A733_HDMI_STOPPED_RETAINED?a733_display_framebuffer_stop(&framebuffer):-1;}
