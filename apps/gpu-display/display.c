/* SPDX-License-Identifier: GPL-2.0-or-later */
/* DE clock/reset sequence adapted from de_top.c and the A733 domain driver.
 * Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved. */
#include "application.h"
#include "../hdmi/framebuffer.h"
#include "a733_display_native.h"
#include "a733_memory_uncached.h"
#include "a733_media_clock_native.h"
#include "aarch64_device_memory.h"
#include <string.h>
#define CCU UINT32_C(0x02002000)
#define PCK UINT32_C(0x07061000)
#define IOVA UINT64_C(0x10000000)
#define PIXEL_BYTES (1280u*720u*2u)
#define FRAME_OFFSET GPU_DISPLAY_FRAME_OFFSET
#define FRAME_SPAN (4096u+GPU_DISPLAY_BANK_BYTES)
#define EXCLUSION (A733_CLOCK_IOMMU_SYS|A733_CLOCK_VI|A733_CLOCK_VE_DEC|A733_CLOCK_VE_ENC)
struct hdmi_framebuffer framebuffer;
static int present(const uint8_t *p)
{unsigned x=0;for(unsigned i=0;i<32;++i)x|=p[i];return x!=0;}
int hdmi_framebuffer_session_valid(const struct hdmi_framebuffer_session *s)
{return s&&s->authority&&present(s->isolation)&&present(s->memory)&&
    s->iommu_gates[0]==0x10007&&s->iommu_gates[1]==0x10007;}
static int current(void)
{return framebuffer.self==&framebuffer&&!framebuffer.failed&&hdmi_live(NULL)&&
    xTaskGetCurrentTaskHandle()==app.handles[1]&&
    !memcmp(&framebuffer.session,&app.session.framebuffer,sizeof(framebuffer.session));}
static int now(void *v,uint64_t *out)
{
    uint64_t t;
    if(v!=&framebuffer||!current()||a733_counter_clock_now(&app.counter,&t)!=1||
       t<framebuffer.last_now||t>=app.deadline)return 0;
    framebuffer.last_now=t;*out=t;return 1;
}
static int timely(void)
{uint64_t t;return now(&framebuffer,&t)&&!atomic_load_explicit(&app.clocks.failed,memory_order_acquire);}
static int owns(void)
{
    if(!current())return 0;
    if(framebuffer.exclusion.id&&(framebuffer.exclusion.owner!=app.session.owner||
       framebuffer.exclusion.exclusive!=EXCLUSION||
       framebuffer.exclusion.shared!=(A733_CLOCK_FABRIC|A733_CLOCK_REF|A733_CLOCK_OSC)||
       a733_clock_token_status(&app.clocks,&framebuffer.exclusion)!=A733_CLOCK_OK))return 0;
    if(framebuffer.media.token.id&&(framebuffer.media.self!=&framebuffer.media||
       framebuffer.media.registry!=&app.clocks||framebuffer.media.pck_manager!=&app.pck||
       framebuffer.media.cookie!=&framebuffer||framebuffer.media.token.owner!=app.session.owner||
       framebuffer.media.token.exclusive!=A733_CLOCK_DE_SYS||
       memcmp(&framebuffer.media.config,&framebuffer.media_config,sizeof(framebuffer.media_config))||
       a733_clock_token_status(&app.clocks,&framebuffer.media.token)!=A733_CLOCK_OK))return 0;
    return a733_clock_token_status(&app.clocks,&app.hdmi.clock)==A733_CLOCK_OK&&
        a733_rail_status(&app.rails,&app.hdmi.rail)==A733_RAIL_READY&&
        a733_pmic_live(&app.pmic,&app.hdmi.pmic_lifetime)==A733_PMIC_READY&&
        a733_gpu_resources_status(&app.pck,app.session.pck_client,&app.hdmi.pck_lease)==A733_GPU_RESOURCE_OK;
}
static int mapped(uintptr_t a)
{
    struct aarch64_translation r,w;
    return aarch64_device_translate(a,0,&r)&&aarch64_device_translate(a,1,&w)&&
        r.physical==a&&w.physical==a&&r.attributes==0&&w.attributes==0;
}
static int address(uintptr_t a)
{
    return !(a&3)&&((a>=A733_DE_BASE&&a<A733_DE_BASE+0x400000)||
        (a>=A733_IOMMU_BASE&&a<A733_IOMMU_BASE+0x20000)||
        (a>=CCU&&a<CCU+0x2000)||(a>=PCK&&a<PCK+0x1000)||
        a==0x07090160||a==0x070101ac);
}
static int enter(void *v,uint64_t d)
{
    if(v!=&framebuffer||d!=app.deadline||!owns()||!timely()||framebuffer.held)return -1;
    int r=a733_platform_lock_enter(&app.locks,A733_PLATFORM_REGISTER_LOCK,d);
    if(r!=1)return r;
    framebuffer.held=1;return timely()?1:-1;
}
static void leave(void *v)
{
    if(v!=&framebuffer||!framebuffer.held||!timely()){hdmi_framebuffer_retain();return;}
    a733_platform_lock_leave(&app.locks,A733_PLATFORM_REGISTER_LOCK);framebuffer.held=0;
}
static int raw_read(void *v,uintptr_t a,uint32_t *out)
{
    if(v!=&framebuffer||!out||!framebuffer.held||!address(a)||!mapped(a)||!timely())return 0;
    __asm__ volatile("dsb sy":::"memory");*out=*(volatile const uint32_t *)a;
    __asm__ volatile("dsb sy":::"memory");return timely();
}
static int raw_write(void *v,uintptr_t a,uint32_t value)
{
    if(v!=&framebuffer||!framebuffer.held||!address(a)||!mapped(a)||!timely())return 0;
    __asm__ volatile("dsb sy":::"memory");*(volatile uint32_t *)a=value;
    __asm__ volatile("dsb sy":::"memory");return timely();
}
void hdmi_framebuffer_retain(void)
{
    framebuffer.failed=1;
    if(framebuffer.held&&a733_platform_lock_retain(&app.locks,A733_PLATFORM_REGISTER_LOCK)==1)framebuffer.held=0;
    if(framebuffer.exclusion.id)(void)a733_clock_retain(&app.clocks,&framebuffer.exclusion);
    if(framebuffer.media.token.id)(void)a733_clock_retain(&app.clocks,&framebuffer.media.token);
    /* Even partial construction keeps its complete pool parent. There is no
     * drain inference or transition back to CPU/FREE on any path. */
    if(framebuffer.pool&&framebuffer.lease.generation){
        struct device_memory_allocation *a=device_memory_lookup(framebuffer.pool,framebuffer.lease);
        if(a)a->state=DEVICE_MEMORY_RETAINED;
    }
}
static void retain(void *v){if(v==&framebuffer)hdmi_framebuffer_retain();}
static int read32(void *v,uintptr_t a,uint32_t *out)
{
    if(enter(v,app.deadline)!=1)return 0;
    if(!raw_read(v,a,out)){retain(v);return 0;}leave(v);return timely();
}
static int write32(void *v,uintptr_t a,uint32_t value)
{
    if(enter(v,app.deadline)!=1)return 0;
    if(!raw_write(v,a,value)){retain(v);return 0;}leave(v);return timely();
}
static int update(uintptr_t a,uint32_t mask,uint32_t value)
{
    int r=enter(&framebuffer,app.deadline);if(r!=1)return r;
    uint32_t old,actual;
    if(!raw_read(&framebuffer,a,&old)||!raw_write(&framebuffer,a,(old&~mask)|value)||
       !raw_read(&framebuffer,a,&actual)||actual!=((old&~mask)|value)){retain(&framebuffer);return -1;}
    leave(&framebuffer);return timely()?1:-1;
}
static int memory(void)
{
    struct device_memory_extent e;enum device_memory_state state;
    return framebuffer.pool&&device_memory_describe(framebuffer.pool,framebuffer.lease,&e,&state)&&
        state==DEVICE_MEMORY_DEVICE&&!memcmp(&e,&framebuffer.extent,sizeof(e))&&
        framebuffer.pool==a733_memory_uncached_init() && e.physical==e.cpu &&
        e.bytes==GPU_DISPLAY_PARENT_BYTES && e.cpu>=framebuffer.pool->backing.cpu &&
        e.bytes<=framebuffer.pool->backing.bytes &&
        e.cpu-framebuffer.pool->backing.cpu<=framebuffer.pool->backing.bytes-e.bytes;
}
static int source_ready(void)
{
    uint64_t pa;
    return memory()&&a733_media_clock_prepared(&framebuffer.media)&&
        framebuffer.iommu.state==A733_IOMMU_ACTIVE&&framebuffer.iommu.translated_masters==0x100&&
        framebuffer.iommu.accessible_masters==0x100&&
        !memcmp(&framebuffer.iommu.tables,&framebuffer.tables,sizeof(framebuffer.tables))&&
        framebuffer.tables.words==(void *)framebuffer.extent.cpu&&framebuffer.tables.physical==framebuffer.extent.physical&&
        framebuffer.tables.count==1&&framebuffer.tables.spans[0].iova==IOVA&&
        framebuffer.tables.spans[0].physical==framebuffer.extent.physical+32768&&
        framebuffer.tables.spans[0].bytes==FRAME_SPAN&&framebuffer.tables.spans[0].access==A733_IOMMU_READ&&
        framebuffer.de_config.lease==framebuffer.media.token.id&&
        framebuffer.de_config.output==A733_DE_TV0_RGB565&&
        framebuffer.de_config.surface.device_address==IOVA+4096&&
        framebuffer.de_config.surface.mapped_bytes==FRAME_SPAN-4096&&
        framebuffer.de_config.surface.owner==app.session.owner&&framebuffer.de_config.surface.pitch==2560&&
        a733_iommu_validate(&framebuffer.tables)==1&&
        a733_iommu_translate(&framebuffer.tables,IOVA+4096,A733_IOMMU_READ,&pa)==1&&
        pa==framebuffer.extent.physical+FRAME_OFFSET&&
        !memcmp(&framebuffer.de.config,&framebuffer.de_config,sizeof(framebuffer.de_config));
}
int hdmi_framebuffer_source(void *v,unsigned starting)
{
    if(v!=&framebuffer||starting>1||!current())return -1;
    /* HDMI may prepare its independent PHY/TV while this source is still NEW.
     * Only the start boundary can authorize fetching the framebuffer. */
    if(!framebuffer.stage)return starting?-1:1;
    if(!owns()||!memory())return -1;
    if(!starting)return 1;
    return source_ready()&&framebuffer.de.state==A733_DE_ARMED?1:-1;
}
static int media_authorize(void *v,const struct a733_media_clock_config *c,
    const struct a733_clock_token *token,const struct a733_gpu_resource_lease *pck)
{
    return v==&framebuffer&&owns()&&memory()&&c==&framebuffer.media.config&&
        !memcmp(c,&framebuffer.media_config,sizeof(*c))&&token==&framebuffer.media.token&&
        token->exclusive==A733_CLOCK_DE_SYS&&pck==&framebuffer.media.pck&&
        !memcmp(pck,&app.hdmi.pck_lease,sizeof(*pck));
}
static int media_write(void *v,uintptr_t a,uint32_t value)
{
    if(v!=&framebuffer||!framebuffer.held||!mapped(a)||!timely())return 0;
    int r=a733_media_clock_native_write(v,a,value);return r==1&&timely();
}
static int barrier(void *v)
{if(v!=&framebuffer||!owns()||!timely())return 0;__asm__ volatile("dsb sy":::"memory");return timely();}
static int de_admit(void *v,const struct a733_de_config *c,enum a733_de_action action)
{
    if(v!=&framebuffer||!owns()||!source_ready()||c!=&framebuffer.de.config)return 0;
    enum a733_hdmi_state h=app.hdmi.controller.state;
    if(action==A733_DE_PREPARE||action==A733_DE_START)return h==A733_HDMI_PREPARED;
    if(action==A733_DE_REPLAY)return h==A733_HDMI_ACTIVE &&
        atomic_load_explicit(&app.frames_ready,memory_order_acquire) &&
        a733_de_replay_valid((void *)(framebuffer.extent.cpu+FRAME_OFFSET+2u*GPU_DISPLAY_FRAME_BYTES),
            IOVA+4096,GPU_DISPLAY_FRAME_BYTES);
    if(action==A733_DE_STOP)return h==A733_HDMI_STOPPED_RETAINED;
    return h==A733_HDMI_ACTIVE;
}
static int iommu_admit(void *v,const struct a733_iommu_tables *t,uint64_t d,struct a733_iommu_admission *a)
{
    if(v!=&framebuffer||d!=app.deadline||!owns()||!memory()||!a733_media_clock_prepared(&framebuffer.media)||
       memcmp(t,&framebuffer.tables,sizeof(*t)))return -1;
    /* The immutable isolation record authenticates prior drain and exclusion
     * of every master. This state is not inferred from ENABLE or status bits. */
    *a=(struct a733_iommu_admission){.token=framebuffer.session.authority,
        .excluded_masters=A733_IOMMU_REAL_MASTERS,.powered_masters=0x100,
        .inherited_resolved=1,.transactions_drained=1,.clocks_held=1,.retained_power_removal=1};
    return 1;
}
static int iommu_read(void *v,uint64_t token,uint32_t off,uint32_t *out,uint64_t d)
{return token==framebuffer.session.authority&&off<0x20000&&d==app.deadline&&read32(v,A733_IOMMU_BASE+off,out)?1:-1;}
static int iommu_write(void *v,uint64_t token,uint32_t off,uint32_t value,uint64_t d)
{return token==framebuffer.session.authority&&off<0x20000&&d==app.deadline&&write32(v,A733_IOMMU_BASE+off,value)?1:-1;}
static int publish(void *v,uint64_t token,const void *p,size_t bytes,uint64_t d)
{return token==framebuffer.session.authority&&p==framebuffer.tables.words&&bytes==32768&&d==app.deadline&&barrier(v)?1:-1;}
static int bank_cpu_owned(void)
{
    struct device_memory_extent e;enum device_memory_state state;
    return framebuffer.self==&framebuffer && !framebuffer.failed && framebuffer.boot_ready &&
        framebuffer.pool==a733_memory_uncached_init() &&
        device_memory_describe(framebuffer.pool,framebuffer.lease,&e,&state) &&
        state==DEVICE_MEMORY_CPU && !memcmp(&e,&framebuffer.extent,sizeof(e));
}
static int allocate(void)
{
    framebuffer.pool=a733_memory_uncached_init();
    if(!framebuffer.pool || !device_memory_acquire_aligned(framebuffer.pool,app.session.owner,
        GPU_DISPLAY_PARENT_BYTES,16384,&framebuffer.lease,&framebuffer.extent))return 0;
    struct device_memory_extent *e=&framebuffer.extent;
    if(e->physical!=e->cpu || e->bytes!=GPU_DISPLAY_PARENT_BYTES ||
       e->cpu<framebuffer.pool->backing.cpu || e->bytes>framebuffer.pool->backing.bytes ||
       e->cpu-framebuffer.pool->backing.cpu>framebuffer.pool->backing.bytes-e->bytes)return 0;
    memset((void *)e->cpu,0xa5,e->bytes);
    struct a733_iommu_span span={IOVA,e->physical+32768,FRAME_SPAN,A733_IOMMU_READ};
    return a733_iommu_build(&framebuffer.tables,(void *)e->cpu,e->physical,&span,1)==1;
}
int gpu_display_bank_copy(unsigned frame,uint32_t sequence,uint32_t offset,const void *data,uint32_t bytes)
{
    if(frame>1 || sequence!=frame+1 || !hdmi_live(NULL) ||
       xTaskGetCurrentTaskHandle()!=app.handles[2] || !bank_cpu_owned() ||
       atomic_load_explicit(&app.frames_ready,memory_order_acquire) ||
       offset!=app.copied[frame] || !bytes || ((offset|bytes)&3) ||
       offset>GPU_DISPLAY_WIDTH*GPU_DISPLAY_HEIGHT*4u ||
       bytes>GPU_DISPLAY_WIDTH*GPU_DISPLAY_HEIGHT*4u-offset)return 0;
    struct pvr_render_service *source=&app.gpu_graphics.submission;
    const void *owned;uint32_t owned_bytes;
    if(source->phase!=PVR_RENDER_SERVICE_COMPLETE || source->sequence!=sequence ||
       source->texture_output_frame!=sequence || source->texture_output_offset!=offset ||
       !pvr_gpu_service_texture_output(&app.gpu_service,source->texture_output_ticket,&owned,&owned_bytes) ||
       owned!=data || owned_bytes!=bytes)return 0;
    const uint8_t *in=data;
    uint16_t *out=(void *)(framebuffer.extent.cpu+FRAME_OFFSET+frame*GPU_DISPLAY_FRAME_BYTES+offset/2);
    for(uint32_t i=0;i<bytes;i+=4)
        out[i/4]=(uint16_t)(((uint16_t)(in[i]>>3)<<11)|((uint16_t)(in[i+1]>>2)<<5)|(in[i+2]>>3));
    if(!gpu_display_evidence_chunk(frame,offset,in,out,bytes))return 0;
    app.copied[frame]+=bytes;app.frame_sequence[frame]=sequence;
    return hdmi_live(NULL);
}
int gpu_display_bank_seal(void)
{
    if(!hdmi_live(NULL) || xTaskGetCurrentTaskHandle()!=app.handles[2] || !bank_cpu_owned() ||
       !atomic_load_explicit(&app.gpu_closed,memory_order_acquire) ||
       app.copied[0]!=GPU_DISPLAY_WIDTH*GPU_DISPLAY_HEIGHT*4u ||
       app.copied[1]!=GPU_DISPLAY_WIDTH*GPU_DISPLAY_HEIGHT*4u ||
       app.frame_sequence[0]!=1 || app.frame_sequence[1]!=2)return 0;
    void *commands=(void *)(framebuffer.extent.cpu+FRAME_OFFSET+2u*GPU_DISPLAY_FRAME_BYTES);
    if(!a733_de_replay_build(commands,IOVA+4096,GPU_DISPLAY_FRAME_BYTES))return 0;
    /* NC mappings were verified by the actual pool constructor; no cached alias
     * or CPU cache clean is substituted for publication. No device has begun
     * fetching this bank. Every byte remains immutable after this barrier. */
    if(!a733_display_native_barrier())return 0;
    struct device_memory_allocation *a=device_memory_lookup(framebuffer.pool,framebuffer.lease);
    if(!a || a->state!=DEVICE_MEMORY_CPU || !hdmi_live(NULL))return 0;
    a->state=DEVICE_MEMORY_DEVICE;return 1;
}
static int rcq_submit(void *v)
{
    uint64_t time,deadline=framebuffer.de.deadline;
    if(v!=&framebuffer || !owns() || !source_ready() ||
       framebuffer.de.action!=A733_DE_REPLAY || deadline>app.stop_at ||
       app.hdmi.controller.state!=A733_HDMI_ACTIVE || framebuffer.held ||
       !mapped(A733_DE_BASE+0x8144) || !mapped(A733_DE_BASE+0x8150) ||
       !a733_counter_clock_now(&app.counter,&time) || time>=deadline ||
       a733_platform_lock_enter(&app.locks,A733_PLATFORM_REGISTER_LOCK,deadline)!=1)return -1;
    framebuffer.held=1;
    if(!a733_counter_clock_now(&app.counter,&time) || time>=deadline){
        /* No command was submitted. Return the healthy mutex; the DE leaf
         * retains the failed replay and every memory/resource owner. */
        leave(v);return -1;
    }
    int submitted=a733_display_native_rcq_submit();
    if(!a733_counter_clock_now(&app.counter,&time) || time>=deadline || !timely()){
        retain(v);return -1;
    }
    leave(v);return timely()?submitted:-1;
}
int hdmi_framebuffer_boot(void)
{
    if(xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED||
       framebuffer.self!=&framebuffer||framebuffer.boot_ready||framebuffer.pool||
       framebuffer.failed||!hdmi_framebuffer_session_valid(&framebuffer.session)||
       memcmp(&framebuffer.session,&app.session.framebuffer,sizeof(framebuffer.session)))return 0;
    if(!allocate()){hdmi_framebuffer_retain();return 0;}
    framebuffer.boot_ready=1;return 1;
}
/* Every selected register is owned by the real DE token. Existing PCK/rail
 * ownership is borrowed from HDMI, never fabricated or released. */
static int cold_step(void)
{
    uint32_t value,other;
    switch(framebuffer.index){
    case 0:
        if(!read32(&framebuffer,PCK,&value)||!read32(&framebuffer,PCK+8,&other)||
           (value&15)!=(other&15)||((value&15)!=0&&(value&15)!=8))return -1;
        framebuffer.domain_on=(value&15)==8;
        if(!read32(&framebuffer,CCU+0x5c0,&value)||!(value&(1u<<5))||
           !read32(&framebuffer,CCU+0x5e0,&value)||!(value&(1u<<11)))return -1;
        if(!read32(&framebuffer,CCU+0xa00,&value)||(value&0x80000000)||
           !read32(&framebuffer,CCU+0xa04,&value)||(value&1))return -1;
        for(unsigned i=0;i<2;++i)if(!read32(&framebuffer,CCU+(i?0x5b4:0x58c),&value)||
            (value&0x10007)!=framebuffer.session.iommu_gates[i])return -1;
        break;
    case 1:case 2:{
        int r=update(CCU+(framebuffer.index==1?0xa74:0xa04),0x10000,0);if(r!=1)return r;
        if(framebuffer.index==2)framebuffer.until=framebuffer.last_now+10;break;}
    case 3:if(!timely())return -1;if(framebuffer.last_now<framebuffer.until)return 0;break;
    case 4:case 5:case 6:case 7:case 8:{
        static const unsigned off[]={0x170,0x174,0xc00,0xc04,0xc10};
        static const uint32_t values[]={0x1f1f1f,0x1f1f,0x08080808,0x0808,8};
        if(!framebuffer.domain_on){int r=update(PCK+off[framebuffer.index-4],UINT32_MAX,values[framebuffer.index-4]);if(r!=1)return r;}break;}
    case 9:{int r=update(PCK,15,8);if(r!=1)return r;framebuffer.until=framebuffer.last_now+10000;break;}
    case 10:
        if(!read32(&framebuffer,PCK+8,&value)||framebuffer.last_now>=framebuffer.until)return -1;
        if((value&15)!=8)return 0;break;
    default:return 1;
    }
    ++framebuffer.index;return 0;
}
int hdmi_framebuffer_prepare(void)
{
    struct hdmi_framebuffer *f=&framebuffer;
    if(!current()||app.hdmi.controller.state!=A733_HDMI_PREPARED)return -1;
    int r;
    switch(f->stage){
    case 0:
        if(!f->boot_ready||!memory())goto fail;
        f->media_config=(struct a733_media_clock_config){.owner=app.session.owner,
            .handoff=f->session.authority,.deadline_us=app.deadline,.groups=A733_MEDIA_DE};
        memcpy(f->media_config.fabric,f->session.fabric,sizeof(f->session.fabric));
        memcpy(f->media_config.rate_hz,f->session.fabric_hz,sizeof(f->session.fabric_hz));
        {struct a733_media_clock_ops ops={now,media_authorize,enter,leave,retain,raw_read,media_write};
        if(!a733_media_clock_init(&f->media,&f->media_config,&ops,f,&app.clocks,&app.pck,
            app.session.pck_client,&app.hdmi.pck_lease))goto fail;}++f->stage;return 0;
    case 1:{
        enum a733_clock_result c=a733_clock_acquire(&app.clocks,app.session.owner,
            A733_CLOCK_FABRIC|A733_CLOCK_REF|A733_CLOCK_OSC,EXCLUSION,app.deadline,&f->exclusion);
        if(c==A733_CLOCK_BUSY)return 0;if(c!=A733_CLOCK_OK)goto fail;++f->stage;return 0;}
    case 2:
        r=a733_media_clock_poll(&f->media);
        if(r==A733_MEDIA_BUSY)return 0;
        if(r!=A733_MEDIA_PENDING||!f->media.token.id)goto fail;
        ++f->stage;return 0;
    case 3:
        r=cold_step();if(r<0)goto fail;if(r){f->index=0;++f->stage;}return 0;
    case 4:
        r=a733_media_clock_poll(&f->media);
        if(r==A733_MEDIA_BUSY||r==A733_MEDIA_PENDING)return 0;
        if(r!=A733_MEDIA_PREPARED)goto fail;++f->stage;return 0;
    case 5:{
        /* Exact de_top_set_clk_enable() display1+MBUS setup; unrelated fields
         * survive. No other output/RCQ owner exists under the whole-DE token. */
        static const unsigned offsets[]={0x8000,0x8004,0x8008};
        uint32_t mask=f->index==2?0x11:0x10010;
        r=update(A733_DE_BASE+offsets[f->index],mask,mask);if(r<0)goto fail;if(!r)return 0;
        if(++f->index==3){f->index=0;++f->stage;}return 0;}
    case 6:{
        struct a733_iommu_ops ops={now,iommu_admit,iommu_read,iommu_write,publish};
        if(a733_iommu_init_display(&f->iommu,&ops,f,&f->tables)!=1||a733_iommu_open(&f->iommu,app.deadline)!=1)goto fail;
        ++f->stage;return 0;}
    case 7:
        r=a733_iommu_poll(&f->iommu);if(r<0)goto fail;if(!r)return 0;
        f->de_config=(struct a733_de_config){.ops={de_admit,read32,write32,barrier,now,rcq_submit},
            .context=f,.lease=f->media.token.id,.surface={IOVA+4096,FRAME_SPAN-4096,app.session.owner,2560},
            .output=A733_DE_TV0_RGB565};
        if(!a733_de_init(&f->de,&f->de_config,app.deadline))goto fail;++f->stage;return 0;
    case 8:
        r=a733_de_poll(&f->de);if(r==A733_DE_PENDING)return 0;
        if(r!=A733_DE_READY||!a733_de_request(&f->de,A733_DE_START,app.deadline))goto fail;
        ++f->stage;return 0;
    case 9:
        r=a733_de_poll(&f->de);if(r==A733_DE_PENDING)return 0;
        if(r!=A733_DE_ARMED)goto fail;++f->stage;return 1;
    case 10:return hdmi_framebuffer_source(f,1);
    default:goto fail;
    }
fail:hdmi_framebuffer_retain();return -1;
}
int hdmi_framebuffer_stop(void)
{
    if(!current()||app.hdmi.controller.state!=A733_HDMI_STOPPED_RETAINED)return -1;
    if(framebuffer.iommu.state==A733_IOMMU_SCAN){
        int r=a733_iommu_poll(&framebuffer.iommu);if(r<=0)return r;
    }
    if(!framebuffer.de.initialized||framebuffer.de.state==A733_DE_READY)return 1;
    if(framebuffer.de.state==A733_DE_PENDING&&framebuffer.de.action!=A733_DE_STOP){
        hdmi_framebuffer_retain();return -1; /* Never finish START after output stop. */
    }
    if(framebuffer.de.state==A733_DE_ARMED&&
       !a733_de_request(&framebuffer.de,A733_DE_STOP,app.deadline))return -1;
    enum a733_de_state s=a733_de_poll(&framebuffer.de);
    if(s==A733_DE_PENDING)return 0;
    if(s==A733_DE_DISABLED_RETAINED)return 1;
    hdmi_framebuffer_retain();return -1;
}

int hdmi_framebuffer_service(void)
{
    if(!current()||!owns()||!memory())return -1;
    if(framebuffer.iommu.state==A733_IOMMU_ACTIVE&&a733_iommu_scan(&framebuffer.iommu,app.deadline)!=1)return -1;
    return a733_iommu_poll(&framebuffer.iommu);
}

static int output_fault(void)
{
    hdmi_fail(NULL);hdmi_framebuffer_retain();
    (void)a733_hdmi_fail(&app.hdmi.controller.io);
    atomic_store_explicit(&app.state,3,memory_order_release);return -1;
}
int gpu_display_output_step(void)
{
    if(xTaskGetCurrentTaskHandle()!=app.handles[1])return -1;
    unsigned terminal=atomic_load_explicit(&app.state,memory_order_acquire);
    if(terminal==2)return 1;
    if(terminal==3)return output_fault();
    if(!hdmi_live(NULL))return output_fault();
    uint64_t time;
    if(!a733_counter_clock_now(&app.counter,&time)||time<app.last_now||time>=app.deadline)return output_fault();
    app.last_now=time;
    if(atomic_load_explicit(&app.state,memory_order_acquire)==2)return 1;
    struct a733_hdmi *h=&app.hdmi.controller;
    if(time>=app.stop_at && app.stage<4){
        if(h->state==A733_HDMI_ACTIVE||h->state==A733_HDMI_PREPARED||h->state==A733_HDMI_STARTING){
            if(a733_hdmi_stop(h,app.deadline)!=1)return output_fault();app.stage=4;
        }else return output_fault();
    }
    if(!atomic_load_explicit(&app.pmic_ready,memory_order_acquire))return 0;
    int r;
    switch(app.stage){
    case 0:
        if(a733_hdmi_prepare(h,app.deadline)!=1)return output_fault();app.stage=1;return 0;
    case 1:
        r=a733_hdmi_poll(h);if(r<0)return output_fault();if(!r)return 0;
        if(h->state!=A733_HDMI_PREPARED)return output_fault();
        if(!atomic_load_explicit(&app.frames_ready,memory_order_acquire))return 0;
        r=hdmi_framebuffer_prepare();if(r<0)return output_fault();if(!r)return 0;
        if(a733_hdmi_start(h,app.deadline)!=1)return output_fault();app.stage=2;return 0;
    case 2:
        r=a733_hdmi_poll(h);if(r<0)return output_fault();if(!r)return 0;
        if(h->state!=A733_HDMI_ACTIVE)return output_fault();
        atomic_store_explicit(&app.state,1,memory_order_release);app.stage=3;return 0;
    case 3:
        /* HPD and controller underflow stay observed while RCQ is pending.
         * Only the IOMMU scan is mutually exclusive with replay polling. */
        r=a733_hdmi_service(h,app.deadline);if(r<0 || h->state!=A733_HDMI_ACTIVE)return output_fault();
        if(app.presentations<3){
            static const unsigned slots[3]={0,1,0};
            if(framebuffer.de.state==A733_DE_PENDING){
                if(framebuffer.de.action!=A733_DE_REPLAY)return output_fault();
                enum a733_de_state state=a733_de_poll(&framebuffer.de);
                if(state==A733_DE_PENDING)return 0;
                if(state!=A733_DE_ARMED || framebuffer.de.replay_slot!=slots[app.presentations] ||
                   !framebuffer.de.replay_rcq_done || framebuffer.de.replay_frame_end<framebuffer.de.replay_rcq_done)
                    return output_fault();
                uint64_t observed;
                if(!a733_counter_clock_now(&app.counter,&observed)||observed<time||
                   observed<framebuffer.de.replay_frame_end||observed>=app.deadline)return output_fault();
                app.presented[app.presentations]=app.frame_sequence[slots[app.presentations]];
                app.presentation[app.presentations]=(struct gpu_display_presentation){
                    .slot=slots[app.presentations],.sequence=app.presented[app.presentations],
                    .rcq_done_us=framebuffer.de.replay_rcq_done,.frame_end_us=framebuffer.de.replay_frame_end,
                    .verified_source_address=framebuffer.de.config.surface.device_address+
                        slots[app.presentations]*GPU_DISPLAY_FRAME_BYTES,
                    .raw_status=framebuffer.de.raw_status,.flags=1,.observed_us=observed};
                ++app.presentations;return 0;
            }
            /* Never run a fault scan concurrently with RCQ admission/polling.
             * The replay callback requires the completed ACTIVE IOMMU state. */
            r=hdmi_framebuffer_service();if(r<0)return output_fault();if(!r)return 0;
            if(framebuffer.iommu.state!=A733_IOMMU_ACTIVE ||
               !a733_de_replay_request(&framebuffer.de,slots[app.presentations],GPU_DISPLAY_FRAME_BYTES,app.stop_at))
                return output_fault();
            return 0;
        }
        r=hdmi_framebuffer_service();if(r<0)return output_fault();return r;
    case 4:
        r=a733_hdmi_poll(h);if(r<0)return output_fault();if(!r)return 0;
        if(h->state!=A733_HDMI_STOPPED_RETAINED)return output_fault();
        r=hdmi_framebuffer_stop();if(r<0)return output_fault();if(!r)return 0;
        if(app.presentations!=3 || app.presented[0]!=1 || app.presented[1]!=2 || app.presented[2]!=1)
            return output_fault();
        atomic_store_explicit(&app.pmic_park_request,1,memory_order_release);
        if(!atomic_load_explicit(&app.pmic_parked,memory_order_acquire))return 0;
        atomic_store_explicit(&app.state,2,memory_order_release);return 1;
    default:return output_fault();
    }
}
void gpu_display_output_task(void *unused)
{(void)unused;for(;;){(void)gpu_display_output_step();gpu_display_observation_poll();vTaskDelay(1);}}
