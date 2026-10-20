#include "session.h"
#include "a733_device_memory_layout.h"
#include "a733_storage_digest.h"
#include <string.h>
static int present(const uint8_t *p)
{unsigned v=0;for(unsigned i=0;i<32;++i)v|=p[i];return v!=0;}
int hdmi_session_valid(const struct hdmi_session *s)
{
    return s&&s->magic==HDMI_SESSION_MAGIC&&s->version==1&&s->bytes==sizeof(*s)&&
        s->epoch&&s->generation&&s->lease&&s->supervisor&&s->owner&&!s->reserved&&
        present(s->image)&&present(s->evidence)&&present(s->analog)&&present(s->power)&&
        present(s->supervision)&&s->pck_client<A733_GPU_RESOURCE_CLIENTS&&
        s->request.mode==A733_HDMI_720P60&&s->request.range==A733_HDMI_LIMITED&&
        s->request.initial.id&&s->request.initial.generation&&
        s->pmic.owner&&s->pmic.owner!=s->owner&&s->pmic.identity==s->generation&&
        !s->pmic.startup_deadline
#if SUNSTONE_HDMI_FRAMEBUFFER
        &&s->request.source==A733_HDMI_FRAMEBUFFER&&hdmi_framebuffer_session_valid(&s->framebuffer)
#else
        &&s->request.source==A733_HDMI_PATTERN
#endif
        ;
}

__attribute__((used,retain)) const uint64_t gpu_display_geometry[12]={
    GPU_DISPLAY_SESSION_MAGIC,1,offsetof(struct gpu_display_session,inputs),
    sizeof(struct gpu_display_boot_contract),A733_STORAGE_SESSION_ADDRESS,
    GPU_DISPLAY_ENVELOPE_LIMIT,20,sizeof(struct hdmi_session),
    offsetof(struct gpu_display_session,gpu_initial),sizeof(struct gpu_display_inputs),1280,720};
int gpu_display_session_valid(const struct gpu_display_session *s)
{
    if(!s || s->magic!=GPU_DISPLAY_SESSION_MAGIC || s->version!=1 ||
       s->bytes<sizeof(*s) || s->bytes>GPU_DISPLAY_ENVELOPE_LIMIT ||
       !s->memory_authority ||
       s->inputs.bytes!=s->bytes-offsetof(struct gpu_display_session,inputs) ||
       !hdmi_session_valid(&s->hdmi) ||
       s->hdmi.pck_client==7 || (s->hdmi.owner>=9&&s->hdmi.owner<=11) ||
       (s->hdmi.pmic.owner>=9&&s->hdmi.pmic.owner<=11) ||
       !present(s->gpu_initial)||!present(s->gpu_memory)||!present(s->gpu_electrical)||
       !present(s->gpu_supervision))return 0;
    /* EL3 checks only integer bounds and hashes. Vertex floating-point checks
     * run at EL1 before providers, through gpu_display_inputs_valid. */
    const struct gpu_display_inputs *in=&s->inputs;
    if(in->magic!=GPU_DISPLAY_INPUT_MAGIC || in->version!=GPU_DISPLAY_INPUT_VERSION ||
       in->bytes<sizeof(*in))return 0;
    uint32_t next=sizeof(*in);
    for(unsigned i=0;i<2;++i){
        const struct gpu_display_input_frame *f=&in->frame[i];
        uint32_t stride,texture;
        if(f->reserved || f->offset!=next || !f->bytes || f->bytes>in->bytes-next ||
           !pvr_render_texture_geometry(f->input.width,f->input.height,&stride,&texture) ||
           f->input.stride!=stride || !f->input.vertex_count || f->input.vertex_count>PVR_RENDER_TEXTURE_VERTICES ||
           f->input.index_count<3 || f->input.index_count>PVR_RENDER_TEXTURE_INDICES || f->input.index_count%3 ||
           (uint64_t)f->input.vertex_count*24+(uint64_t)f->input.index_count*2+texture!=f->bytes)return 0;
        uint8_t digest[32];storage_digest_bytes((const uint8_t *)in+next,f->bytes,digest);
        if(memcmp(digest,f->digest,32))return 0;
        next+=f->bytes;
    }
    return next==in->bytes;
}
int a733_smp_contract_valid(const struct a733_smp_contract *c,uint64_t now)
{
    const struct gpu_display_session *s=(const void *)A733_STORAGE_SESSION_ADDRESS;
    const struct a733_device_memory_layout layout={A733_DEVICE_MEMORY_LAYOUT,
        A733_DEVICE_WB_START,A733_DEVICE_WB_END,A733_DEVICE_NC_START,A733_DEVICE_NC_END};
    if(!c || c->magic!=GPU_DISPLAY_CONTRACT_MAGIC || c->version!=2 ||
       c->bytes!=sizeof(struct gpu_display_boot_contract) || !c->epoch || !c->generation || !c->lease ||
       c->inverse_generation!=~c->generation || !a733_device_memory_handoff_valid(&layout,c->reserved) ||
       memcmp(c->build_id,a733_smp_build_id,32) || !gpu_display_session_valid(s))return 0;
    const struct gpu_display_boot_contract *record=gpu_display_boot_record(c);
    if(!record || c->reserved[1]!=s->memory_authority || memcmp(s->hdmi.image,a733_smp_build_id,32) ||
       c->epoch!=s->hdmi.epoch || c->generation!=s->hdmi.generation || c->lease!=s->hdmi.lease ||
       memcmp(record->entry_digest,s->hdmi.evidence,32) || c->armed_at>now ||
       c->armed_at>UINT64_MAX-HDMI_WINDOW_TICKS || c->startup_deadline!=c->armed_at+A733_SMP_COUNTER_HZ ||
       now>=c->startup_deadline || c->diagnostic_deadline!=c->armed_at+HDMI_WINDOW_TICKS ||
       c->expires_at!=c->diagnostic_deadline || c->watchdog_mode || c->watchdog_configuration ||
       c->watchdog_output || c->fel_flag || c->redirect_flag || c->redirect_entry || c->alternate_flag)return 0;
    uint8_t digest[32];storage_digest_bytes(s,s->bytes,digest);
    return !memcmp(digest,record->session_digest,32);
}
int a733_smp_power_address(unsigned core,uintptr_t address,int write)
{
    if(core<1||core>=8)return 0;
    uintptr_t cfg=UINT32_C(0x08001000)+(uintptr_t)core*0x1000;
    if(address==cfg||address==cfg+4||address==cfg+8||address==UINT32_C(0x07050200)+core*4||
       address==UINT32_C(0x07050220)+core*4)return 1;
    return !write&&address==UINT32_C(0x07052008)+(uintptr_t)core*0x1000;
}
