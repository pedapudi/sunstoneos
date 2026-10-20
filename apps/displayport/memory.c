/* SPDX-License-Identifier: GPL-2.0-or-later */
/* DE clock/reset sequence adapted from de_top.c and the A733 domain driver.
 * Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved. */
#include "application.h"
#include "a733_memory_uncached.h"
#include "a733_device_memory_layout.h"
#include "a733_storage_digest.h"
#include "a733_media_clock_native.h"
#include "aarch64_device_memory.h"
#include <string.h>
#define CCU UINT32_C(0x02002000)
#define PCK UINT32_C(0x07061000)
#define IOVA UINT64_C(0x10000000)
#define PIXEL_BYTES (1280u*720u*2u)
#define FRAME_OFFSET 36864u
#define FRAME_SPAN (2097152u-32768u)
#define EXCLUSION (A733_CLOCK_IOMMU_SYS|A733_CLOCK_VI|A733_CLOCK_VE_DEC|A733_CLOCK_VE_ENC)
#define framebuffer dp_app.memory
#define app dp_app

static int current(void)
{return framebuffer.self==&framebuffer&&!framebuffer.failed&&dp_live()&&dp_current();}
static int now(void *v,uint64_t *out)
{
    uint64_t t;
    if(v!=&framebuffer||!current()||a733_counter_clock_now(&providers.shared.counter,&t)!=1||
       t<framebuffer.last_now||t>=app.deadline)return 0;
    framebuffer.last_now=t;*out=t;return 1;
}
static int timely(void)
{uint64_t t;return now(&framebuffer,&t)&&!atomic_load_explicit(&providers.shared.clocks.failed,memory_order_acquire);}
static int owns(void)
{
    if(!current())return 0;
    if(framebuffer.exclusion.id&&(framebuffer.exclusion.owner!=app.session.owner||
       framebuffer.exclusion.exclusive!=EXCLUSION||
       framebuffer.exclusion.shared!=(A733_CLOCK_FABRIC|A733_CLOCK_REF|A733_CLOCK_OSC)||
       a733_clock_token_status(&providers.shared.clocks,&framebuffer.exclusion)!=A733_CLOCK_OK))return 0;
    if(framebuffer.media.token.id&&(framebuffer.media.self!=&framebuffer.media||
       framebuffer.media.registry!=&providers.shared.clocks||framebuffer.media.pck_manager!=&providers.shared.gpu||
       framebuffer.media.cookie!=&framebuffer||framebuffer.media.token.owner!=app.session.owner||
       framebuffer.media.token.exclusive!=A733_CLOCK_DE_SYS||
       memcmp(&framebuffer.media.config,&framebuffer.media_config,sizeof(framebuffer.media_config))||
       a733_clock_token_status(&providers.shared.clocks,&framebuffer.media.token)!=A733_CLOCK_OK))return 0;
    return a733_clock_token_status(&providers.shared.clocks,&app.native.clock)==A733_CLOCK_OK&&
        a733_rail_status(&providers.shared.rails,&app.native.rail)==A733_RAIL_READY&&
        a733_pmic_live(&providers.shared.pmic,&app.native.pmic_lifetime)==A733_PMIC_READY&&
        a733_gpu_resources_status(&providers.shared.gpu,app.session.pck_client,&app.native.pck_lease)==A733_GPU_RESOURCE_OK;
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
    int r=a733_platform_lock_enter(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK,d);
    if(r!=1)return r;
    framebuffer.held=1;return timely()?1:-1;
}
static void leave(void *v)
{
    if(v!=&framebuffer||!framebuffer.held||!timely()){dp_memory_retain();return;}
    a733_platform_lock_leave(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK);framebuffer.held=0;
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
void dp_memory_retain(void)
{
    framebuffer.failed=1;
    if(framebuffer.held&&a733_platform_lock_retain(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK)==1)framebuffer.held=0;
    if(framebuffer.exclusion.id)(void)a733_clock_retain(&providers.shared.clocks,&framebuffer.exclusion);
    if(framebuffer.media.token.id)(void)a733_clock_retain(&providers.shared.clocks,&framebuffer.media.token);
    /* Even partial construction keeps its complete pool parent. There is no
     * drain inference or transition back to CPU/FREE on any path. */
    if(framebuffer.pool&&framebuffer.lease.generation){
        struct device_memory_allocation *a=device_memory_lookup(framebuffer.pool,framebuffer.lease);
        if(a)a->state=DEVICE_MEMORY_RETAINED;
    }
}
static void retain(void *v){if(v==&framebuffer)dp_memory_retain();}
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
        e.cpu>=A733_DEVICE_NC_START&&e.cpu+e.bytes<=A733_DEVICE_NC_END&&e.physical==e.cpu&&e.bytes==2097152;
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
        framebuffer.de_config.output==A733_DE_TV1_RGB565&&
        framebuffer.de_config.surface.device_address==IOVA+4096&&
        framebuffer.de_config.surface.mapped_bytes==FRAME_SPAN-4096&&
        framebuffer.de_config.surface.owner==app.session.owner&&framebuffer.de_config.surface.pitch==2560&&
        a733_iommu_validate(&framebuffer.tables)==1&&
        a733_iommu_translate(&framebuffer.tables,IOVA+4096,A733_IOMMU_READ,&pa)==1&&
        pa==framebuffer.extent.physical+FRAME_OFFSET&&
        !memcmp(&framebuffer.de.config,&framebuffer.de_config,sizeof(framebuffer.de_config));
}
int dp_memory_source(void *v,unsigned starting)
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
        !memcmp(pck,&app.native.pck_lease,sizeof(*pck));
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
    enum a733_dp_state h=app.native.controller.result.state;
    if(action==A733_DE_PREPARE||action==A733_DE_START)return h==A733_DP_READY;
    if(action==A733_DE_STOP)return h==A733_DP_STOPPED_RETAINED;
    return h==A733_DP_ACTIVE;
}
static int iommu_admit(void *v,const struct a733_iommu_tables *t,uint64_t d,struct a733_iommu_admission *a)
{
    if(v!=&framebuffer||d!=app.deadline||!owns()||!memory()||!a733_media_clock_prepared(&framebuffer.media)||
       memcmp(t,&framebuffer.tables,sizeof(*t)))return -1;
    /* The immutable isolation record authenticates prior drain and exclusion
     * of every master. This state is not inferred from ENABLE or status bits. */
    *a=(struct a733_iommu_admission){.token=app.session.authority,
        .excluded_masters=A733_IOMMU_REAL_MASTERS,.powered_masters=0x100,
        .inherited_resolved=1,.transactions_drained=1,.clocks_held=1,.retained_power_removal=1};
    return 1;
}
static int iommu_read(void *v,uint64_t token,uint32_t off,uint32_t *out,uint64_t d)
{return token==app.session.authority&&off<0x20000&&d==app.deadline&&read32(v,A733_IOMMU_BASE+off,out)?1:-1;}
static int iommu_write(void *v,uint64_t token,uint32_t off,uint32_t value,uint64_t d)
{return token==app.session.authority&&off<0x20000&&d==app.deadline&&write32(v,A733_IOMMU_BASE+off,value)?1:-1;}
static int publish(void *v,uint64_t token,const void *p,size_t bytes,uint64_t d)
{return token==app.session.authority&&p==framebuffer.tables.words&&bytes==32768&&d==app.deadline&&barrier(v)?1:-1;}
static int allocate(void)
{
    framebuffer.pool=a733_memory_uncached_init();
    if(!framebuffer.pool||!device_memory_acquire_aligned(framebuffer.pool,app.session.owner,2097152,16384,
        &framebuffer.lease,&framebuffer.extent))return 0;
    struct device_memory_extent *e=&framebuffer.extent;
    if(e->cpu<A733_DEVICE_NC_START||e->cpu+e->bytes>A733_DEVICE_NC_END||e->physical!=e->cpu||e->bytes!=2097152)return 0;
    memset((void *)e->cpu,0xa5,e->bytes);
    uint16_t *pixels=(void *)(e->cpu+FRAME_OFFSET);
    static const uint16_t colors[]={0xffff,0xffe0,0x07ff,0x07e0,0xf81f,0xf800,0x001f,0};
    for(unsigned y=0;y<720;++y)for(unsigned x=0;x<1280;++x)
        pixels[y*1280+x]=(x<8||x>=1272||y<8||y>=712||x==y||x==1279-y)?0:colors[x/160];
    storage_digest_bytes(pixels,PIXEL_BYTES,framebuffer.pixel_digest);
    struct a733_iommu_span span={IOVA,e->physical+32768,FRAME_SPAN,A733_IOMMU_READ};
    if(a733_iommu_build(&framebuffer.tables,(void *)e->cpu,e->physical,&span,1)!=1)return 0;
    __asm__ volatile("dsb sy":::"memory");
    struct device_memory_allocation *a=device_memory_lookup(framebuffer.pool,framebuffer.lease);
    if(!a||a->state!=DEVICE_MEMORY_CPU)return 0;a->state=DEVICE_MEMORY_DEVICE;
    return memory();
}
int dp_memory_boot(void)
{
    if(xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED||
       framebuffer.self!=&framebuffer||framebuffer.boot_ready||framebuffer.pool||
       framebuffer.failed||!a733_displayport_session_valid(&app.session))return 0;
    if(!allocate()){dp_memory_retain();return 0;}
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
            (value&0x10007)!=app.session.iommu_gates[i])return -1;
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
int dp_memory_prepare(void)
{
    struct dp_memory *f=&framebuffer;
    if(!current()||app.native.controller.result.state!=A733_DP_READY)return -1;
    int r;
    switch(f->stage){
    case 0:
        if(!f->boot_ready||!memory())goto fail;
        f->media_config=(struct a733_media_clock_config){.owner=app.session.owner,
            .handoff=app.session.authority,.deadline_us=app.deadline,.groups=A733_MEDIA_DE};
        memcpy(f->media_config.fabric,app.session.board.fabric,sizeof(app.session.board.fabric));
        memcpy(f->media_config.rate_hz,app.session.board.fabric_hz,sizeof(app.session.board.fabric_hz));
        {struct a733_media_clock_ops ops={now,media_authorize,enter,leave,retain,raw_read,media_write};
        if(!a733_media_clock_init(&f->media,&f->media_config,&ops,f,&providers.shared.clocks,&providers.shared.gpu,
            app.session.pck_client,&app.native.pck_lease))goto fail;}++f->stage;return 0;
    case 1:{
        enum a733_clock_result c=a733_clock_acquire(&providers.shared.clocks,app.session.owner,
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
        f->de_config=(struct a733_de_config){.ops={de_admit,read32,write32,barrier,now},
            .context=f,.lease=f->media.token.id,.surface={IOVA+4096,FRAME_SPAN-4096,app.session.owner,2560},
            .output=A733_DE_TV1_RGB565};
        if(!a733_de_init(&f->de,&f->de_config,app.deadline))goto fail;++f->stage;return 0;
    case 8:
        r=a733_de_poll(&f->de);if(r==A733_DE_PENDING)return 0;
        if(r!=A733_DE_READY||!a733_de_request(&f->de,A733_DE_START,app.deadline))goto fail;
        ++f->stage;return 0;
    case 9:
        r=a733_de_poll(&f->de);if(r==A733_DE_PENDING)return 0;
        if(r!=A733_DE_ARMED)goto fail;++f->stage;return 1;
    case 10:return dp_memory_source(f,1);
    default:goto fail;
    }
fail:dp_memory_retain();return -1;
}
int dp_memory_stop(void)
{
    if(!current()||app.native.controller.result.state!=A733_DP_STOPPED_RETAINED)return -1;
    if(framebuffer.iommu.state==A733_IOMMU_SCAN){
        int r=a733_iommu_poll(&framebuffer.iommu);if(r<=0)return r;
    }
    if(!framebuffer.de.initialized||framebuffer.de.state==A733_DE_READY)return 1;
    if(framebuffer.de.state==A733_DE_PENDING&&framebuffer.de.action!=A733_DE_STOP){
        dp_memory_retain();return -1; /* Never finish START after output stop. */
    }
    if(framebuffer.de.state==A733_DE_ARMED&&
       !a733_de_request(&framebuffer.de,A733_DE_STOP,app.deadline))return -1;
    enum a733_de_state s=a733_de_poll(&framebuffer.de);
    if(s==A733_DE_PENDING)return 0;
    if(s==A733_DE_DISABLED_RETAINED)return 1;
    dp_memory_retain();return -1;
}

int dp_memory_service(void)
{
    if(!current()||!owns()||!memory())return -1;
    if(framebuffer.iommu.state==A733_IOMMU_ACTIVE&&a733_iommu_scan(&framebuffer.iommu,app.deadline)!=1)return -1;
    return a733_iommu_poll(&framebuffer.iommu);
}

/* Acknowledge only the source-defined frame-end W1C bit while preserving
 * non-W1C status controls; then require an observed clear before output start. */
int dp_memory_frame(unsigned clear)
{
 uint32_t status;
 if(!current()||!source_ready()||!read32(&framebuffer,A733_DE_BASE+0x8144,&status))return -1;
 if(clear){
  if(!write32(&framebuffer,A733_DE_BASE+0x8144,(status&~13u)|1u)||
     !read32(&framebuffer,A733_DE_BASE+0x8144,&status)||(status&1))return -1;
  return 1;
 }
 if(status&2)return -1;return !!(status&1);
}
int dp_memory_guards(void)
{
 if(!current()||!memory()||a733_iommu_validate(&framebuffer.tables)!=1)return 0;
 const uint8_t *p=(const void *)framebuffer.extent.cpu;
 for(unsigned i=32768;i<FRAME_OFFSET;i++)if(p[i]!=0xa5)return 0;
 for(unsigned i=FRAME_OFFSET+PIXEL_BYTES;i<2097152;i++)if(p[i]!=0xa5)return 0;
 uint8_t hash[32];storage_digest_bytes(p+FRAME_OFFSET,PIXEL_BYTES,hash);
 return !memcmp(hash,framebuffer.pixel_digest,32);
}
