/* SPDX-License-Identifier: GPL-2.0-or-later */
/* DE clock/reset sequence adapted from de_top.c and the A733 domain driver.
 * Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved. */
#include "a733_display_framebuffer.h"
#include "a733_memory_uncached.h"
#include "a733_media_clock_native.h"
#include "a733_mapping_check.h"
#include "a733_mmio.h"
#include "a733_range.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>
#define CCU UINT32_C(0x02002000)
#define IOVA UINT64_C(0x10000000)
#define TABLE_BYTES 32768u
#define FRAME_OFFSET (TABLE_BYTES+4096u)
#define EXCLUSION (A733_CLOCK_IOMMU_SYS|A733_CLOCK_VI|A733_CLOCK_VE_DEC|A733_CLOCK_VE_ENC)
typedef struct a733_display_framebuffer fb_t;
static fb_t *self(void *v){fb_t *f=v;return f&&f->self==f?f:NULL;}
static int current(fb_t *f)
{return f->self==f&&!f->failed&&f->config.current(f->config.context);}
static int now(void *v,uint64_t *out)
{
    fb_t *f=self(v);uint64_t t;
    if(!f||!current(f)||a733_counter_clock_now(f->config.counter,&t)!=1||
       !a733_deadline_admit(t,&f->last_now,f->config.deadline))return 0;
    *out=t;return 1;
}
static int timely(fb_t *f)
{uint64_t t;return now(f,&t)&&!atomic_load_explicit(&f->config.clocks->failed,memory_order_acquire);}
static int token_live(fb_t *f,const struct a733_clock_token *t)
{
    const struct a733_display_framebuffer_config *c=&f->config;
    return c->token_live?c->token_live(c->context,t):a733_clock_token_status(c->clocks,t)==A733_CLOCK_OK;
}
static int owns(fb_t *f)
{
    const struct a733_display_framebuffer_config *c=&f->config;
    if(!current(f))return 0;
    if(f->exclusion.id&&(f->exclusion.owner!=c->owner||f->exclusion.exclusive!=EXCLUSION||
       f->exclusion.shared!=(A733_CLOCK_FABRIC|A733_CLOCK_REF|A733_CLOCK_OSC)||
       !token_live(f,&f->exclusion)))return 0;
    if(f->media.token.id&&(f->media.self!=&f->media||f->media.registry!=c->clocks||
       f->media.pck_manager!=c->pck||f->media.cookie!=f||f->media.token.owner!=c->owner||
       f->media.token.exclusive!=A733_CLOCK_DE_SYS||
       memcmp(&f->media.config,&f->media_config,sizeof(f->media_config))||
       !token_live(f,&f->media.token)))return 0;
    return c->output_owned(c->context);
}
static int mapped(uintptr_t a)
{
    static const struct a733_mapping_rule rule={A733_MAPPING_READ_WRITE,1,
        {{A733_MAIR_DEVICE_NGNRNE,A733_SH_ANY}},0,1};
    return a733_mapping_check(aarch64_device_translate,a,a,&rule);
}
static int address(uintptr_t a)
{
    return !(a&3)&&((a>=A733_DE_BASE&&a<A733_DE_BASE+0x400000)||
        (a>=A733_IOMMU_BASE&&a<A733_IOMMU_BASE+0x20000)||
        (a>=CCU&&a<CCU+0x2000)||
        a==0x07090160||a==0x070101ac);
}
static int lock(fb_t *f,uint64_t d)
{
    if(!owns(f)||!timely(f)||f->held)return -1;
    int r=a733_platform_lock_enter(f->config.locks,A733_PLATFORM_REGISTER_LOCK,d);
    if(r!=1)return r;
    f->held=1;return timely(f)?1:-1;
}
static int enter(void *v,uint64_t d)
{fb_t *f=self(v);return f&&d==f->config.deadline?lock(f,d):-1;}
static void leave(void *v)
{
    fb_t *f=self(v);if(!f)return;
    if(!f->held||!timely(f)){a733_display_framebuffer_retain(f);return;}
    a733_platform_lock_leave(f->config.locks,A733_PLATFORM_REGISTER_LOCK);f->held=0;
}
static int raw_read(void *v,uintptr_t a,uint32_t *out)
{
    fb_t *f=self(v);
    if(!f||!out||!f->held||!address(a)||!mapped(a)||!timely(f))return 0;
    *out=a733_mmio_read32_dsb_sy_both(a);return timely(f);
}
static int raw_write(void *v,uintptr_t a,uint32_t value)
{
    fb_t *f=self(v);
    if(!f||!f->held||!address(a)||!mapped(a)||!timely(f))return 0;
    a733_mmio_write32_dsb_sy_both(a,value);return timely(f);
}
void a733_display_framebuffer_retain(fb_t *f)
{
    if(!f||f->self!=f)return;
    f->failed=1;
    if(f->held&&a733_platform_lock_retain(f->config.locks,A733_PLATFORM_REGISTER_LOCK)==1)f->held=0;
    if(f->exclusion.id)(void)a733_clock_retain(f->config.clocks,&f->exclusion);
    if(f->media.token.id)(void)a733_clock_retain(f->config.clocks,&f->media.token);
    /* Even partial construction keeps its complete pool parent. There is no
     * drain inference or transition back to CPU/FREE on any path. */
    if(f->pool&&f->lease.generation){
        struct device_memory_allocation *a=device_memory_lookup(f->pool,f->lease);
        if(a)a->state=DEVICE_MEMORY_RETAINED;
    }
}
static void retain(void *v){a733_display_framebuffer_retain(self(v));}
static int read32(void *v,uintptr_t a,uint32_t *out)
{
    fb_t *f=self(v);
    if(!f||enter(f,f->config.deadline)!=1)return 0;
    if(!raw_read(f,a,out)){retain(f);return 0;}leave(f);return timely(f);
}
static int write32(void *v,uintptr_t a,uint32_t value)
{
    fb_t *f=self(v);
    if(!f||enter(f,f->config.deadline)!=1)return 0;
    if(!raw_write(f,a,value)){retain(f);return 0;}leave(f);return timely(f);
}
int a733_display_framebuffer_read(fb_t *f,uintptr_t a,uint32_t *out){return read32(f,a,out);}
int a733_display_framebuffer_write(fb_t *f,uintptr_t a,uint32_t value){return write32(f,a,value);}
static int update(fb_t *f,uintptr_t a,uint32_t mask,uint32_t value)
{
    int r=enter(f,f->config.deadline);if(r!=1)return r;
    uint32_t old,actual;
    if(!raw_read(f,a,&old)||!raw_write(f,a,(old&~mask)|value)||
       !raw_read(f,a,&actual)||actual!=((old&~mask)|value)){retain(f);return -1;}
    leave(f);return timely(f)?1:-1;
}
int a733_display_framebuffer_memory(const fb_t *f)
{
    struct device_memory_extent e;enum device_memory_state state;
    return f&&f->self==f&&f->pool&&f->pool==a733_memory_uncached_init()&&
        device_memory_describe(f->pool,f->lease,&e,&state)&&state==DEVICE_MEMORY_DEVICE&&
        !memcmp(&e,&f->extent,sizeof(e))&&e.physical==e.cpu&&e.bytes==f->config.parent_bytes&&
        e.cpu>=f->pool->backing.cpu&&e.bytes<=f->pool->backing.bytes&&
        e.cpu-f->pool->backing.cpu<=f->pool->backing.bytes-e.bytes;
}
static int memory(fb_t *f){return a733_display_framebuffer_memory(f);}
int a733_display_framebuffer_ready(const fb_t *f)
{
    uint64_t pa;
    if(!a733_display_framebuffer_memory(f))return 0;
    const struct a733_iommu_tables *t=&f->tables;
    const uint32_t span=f->config.frame_span;
    return a733_media_clock_prepared(&f->media)&&
        f->iommu.state==A733_IOMMU_ACTIVE&&f->iommu.translated_masters==0x100&&
        f->iommu.accessible_masters==0x100&&!memcmp(&f->iommu.tables,t,sizeof(*t))&&
        t->words==(void *)f->extent.cpu&&t->physical==f->extent.physical&&
        t->count==1&&t->spans[0].iova==IOVA&&t->spans[0].physical==f->extent.physical+TABLE_BYTES&&
        t->spans[0].bytes==span&&t->spans[0].access==A733_IOMMU_READ&&
        f->de_config.lease==f->media.token.id&&f->de_config.output==f->config.output&&
        f->de_config.surface.device_address==IOVA+4096&&
        f->de_config.surface.mapped_bytes==span-4096&&
        f->de_config.surface.owner==f->config.owner&&f->de_config.surface.pitch==2560&&
        a733_iommu_validate(t)==1&&
        a733_iommu_translate(t,IOVA+4096,A733_IOMMU_READ,&pa)==1&&
        pa==f->extent.physical+FRAME_OFFSET&&
        !memcmp(&f->de.config,&f->de_config,sizeof(f->de_config));
}
int a733_display_framebuffer_source(void *v,unsigned starting)
{
    fb_t *f=self(v);
    if(!f||starting>1||!current(f))return -1;
    /* The output may prepare its independent PHY/TV while this source is still
     * NEW. Only the start boundary can authorize fetching the framebuffer. */
    if(!f->stage)return starting?-1:1;
    if(!owns(f)||!memory(f))return -1;
    if(!starting)return 1;
    return a733_display_framebuffer_ready(f)&&f->de.state==A733_DE_ARMED?1:-1;
}
static int media_authorize(void *v,const struct a733_media_clock_config *c,
    const struct a733_clock_token *token,const struct a733_gpu_resource_lease *pck)
{
    fb_t *f=self(v);
    return f&&owns(f)&&memory(f)&&c==&f->media.config&&
        !memcmp(c,&f->media_config,sizeof(*c))&&token==&f->media.token&&
        token->exclusive==A733_CLOCK_DE_SYS&&pck==&f->media.pck&&
        !memcmp(pck,f->config.pck_lease,sizeof(*pck));
}
static int media_write(void *v,uintptr_t a,uint32_t value)
{
    fb_t *f=self(v);
    if(!f||!f->held||!mapped(a)||!timely(f))return 0;
    int r=a733_media_clock_native_write(v,a,value);return r==1&&timely(f);
}
static int barrier(void *v)
{
    fb_t *f=self(v);
    if(!f||!owns(f)||!timely(f))return 0;
    __asm__ volatile("dsb sy":::"memory");return timely(f);
}
static int de_admit(void *v,const struct a733_de_config *c,enum a733_de_action action)
{
    fb_t *f=self(v);
    return f&&owns(f)&&a733_display_framebuffer_ready(f)&&c==&f->de.config&&
        (action!=A733_DE_REPLAY||f->config.replay)&&f->config.admit(f->config.context,action);
}
/* One RCQ submission under the register mutex. The DE leaf retains a failed
 * replay; an expired request that submitted nothing returns a healthy mutex. */
static int rcq_submit(void *v)
{
    fb_t *f=self(v);uint64_t time;
    if(!f)return -1;
    uint64_t deadline=f->de.deadline;
    if(!owns(f)||!a733_display_framebuffer_ready(f)||f->de.action!=A733_DE_REPLAY||
       !f->config.replay(f->config.context,deadline)||f->held||
       !mapped(A733_DE_BASE+0x8144)||!mapped(A733_DE_BASE+0x8150)||
       !a733_counter_clock_now(f->config.counter,&time)||time>=deadline||
       a733_platform_lock_enter(f->config.locks,A733_PLATFORM_REGISTER_LOCK,deadline)!=1)return -1;
    f->held=1;
    if(!a733_counter_clock_now(f->config.counter,&time)||time>=deadline){leave(f);return -1;}
    int submitted=f->config.rcq_submit();
    if(!a733_counter_clock_now(f->config.counter,&time)||time>=deadline||!timely(f)){
        retain(f);return -1;
    }
    leave(f);return timely(f)?submitted:-1;
}
static int iommu_admit(void *v,const struct a733_iommu_tables *t,uint64_t d,struct a733_iommu_admission *a)
{
    fb_t *f=self(v);
    if(!f||d!=f->config.deadline||!owns(f)||!memory(f)||!a733_media_clock_prepared(&f->media)||
       memcmp(t,&f->tables,sizeof(*t)))return -1;
    /* The immutable isolation record authenticates prior drain and exclusion
     * of every master. This state is not inferred from ENABLE or status bits. */
    *a=(struct a733_iommu_admission){.token=f->config.authority,
        .excluded_masters=A733_IOMMU_REAL_MASTERS,.powered_masters=0x100,
        .inherited_resolved=1,.transactions_drained=1,.clocks_held=1,.retained_power_removal=1};
    return 1;
}
static int iommu_read(void *v,uint64_t token,uint32_t off,uint32_t *out,uint64_t d)
{fb_t *f=self(v);return f&&token==f->config.authority&&off<0x20000&&d==f->config.deadline&&read32(f,A733_IOMMU_BASE+off,out)?1:-1;}
static int iommu_write(void *v,uint64_t token,uint32_t off,uint32_t value,uint64_t d)
{fb_t *f=self(v);return f&&token==f->config.authority&&off<0x20000&&d==f->config.deadline&&write32(f,A733_IOMMU_BASE+off,value)?1:-1;}
static int publish(void *v,uint64_t token,const void *p,size_t bytes,uint64_t d)
{fb_t *f=self(v);return f&&token==f->config.authority&&p==f->tables.words&&bytes==TABLE_BYTES&&d==f->config.deadline&&barrier(f)?1:-1;}
/* PCK domain 1 belongs to the power-domain owner. Before the row request the
 * exclusive DE_SYS token, borrowed output PCK lease and the media token's
 * DCDC2 share are authenticated by owns(); the isolation record supplies
 * initial drain. */
static int domain_authorize(fb_t *f)
{return f->media.token.id&&f->exclusion.id&&owns(f)&&memory(f)?1:-1;}
static int allocate(fb_t *f,unsigned colorbars)
{
    const struct a733_display_framebuffer_config *c=&f->config;
    f->pool=a733_memory_uncached_init();
    if(!f->pool||!device_memory_acquire_aligned(f->pool,c->owner,c->parent_bytes,16384,&f->lease,&f->extent))return 0;
    struct device_memory_extent *e=&f->extent;
    if(e->physical!=e->cpu||e->bytes!=c->parent_bytes||e->cpu<f->pool->backing.cpu||
       e->bytes>f->pool->backing.bytes||e->cpu-f->pool->backing.cpu>f->pool->backing.bytes-e->bytes)return 0;
    memset((void *)e->cpu,0xa5,e->bytes);
    if(colorbars){
        uint16_t *pixels=(void *)(e->cpu+FRAME_OFFSET);
        static const uint16_t colors[]={0xffff,0xffe0,0x07ff,0x07e0,0xf81f,0xf800,0x001f,0};
        for(unsigned y=0;y<720;++y)for(unsigned x=0;x<1280;++x)
            pixels[y*1280+x]=(x<8||x>=1272||y<8||y>=712||x==y||x==1279-y)?0:colors[x/160];
    }
    struct a733_iommu_span span={IOVA,e->physical+TABLE_BYTES,c->frame_span,A733_IOMMU_READ};
    return a733_iommu_build(&f->tables,(void *)e->cpu,e->physical,&span,1)==1;
}
int a733_display_framebuffer_publish(fb_t *f)
{
    if(!f||f->self!=f||f->failed||!f->boot_ready)return 0;
    __asm__ volatile("dsb sy":::"memory");
    struct device_memory_allocation *a=device_memory_lookup(f->pool,f->lease);
    if(!a||a->state!=DEVICE_MEMORY_CPU)return 0;a->state=DEVICE_MEMORY_DEVICE;
    return memory(f);
}
int a733_display_framebuffer_boot(fb_t *f,const struct a733_display_framebuffer_config *c,unsigned colorbars)
{
    if(xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED||!f||f->self||!c||
       !c->counter||!c->clocks||!c->locks||!c->pck||!c->pck_lease||!c->owner||!c->authority||
       !c->deadline||!c->current||!c->output_owned||!c->admit||(c->replay==NULL)!=(c->rcq_submit==NULL)||
       (c->output!=A733_DE_TV0_RGB565&&c->output!=A733_DE_TV1_RGB565)||
       c->frame_span<4096u+1280u*720u*2u||c->parent_bytes<TABLE_BYTES||
       c->frame_span>c->parent_bytes-TABLE_BYTES||!c->domains||!c->rail)return 0;
    f->self=f;f->config=*c;
    if(!a733_power_domain_row_init(&f->row,c->domains,1,c->owner,c->rail)){a733_display_framebuffer_retain(f);return 0;}
    if(!allocate(f,colorbars)){a733_display_framebuffer_retain(f);return 0;}
    f->boot_ready=1;
    if(colorbars&&!a733_display_framebuffer_publish(f)){a733_display_framebuffer_retain(f);return 0;}
    return 1;
}
/* Every selected register is owned by the real DE token. Existing PCK/rail
 * ownership is borrowed from the output, never fabricated or released. */
static int cold_step(fb_t *f)
{
    uint32_t value;
    switch(f->index){
    case 0:
        /* Refuse an incoherent domain, closed MBUS gates or a stopped PPU
         * clock before the resets change; the owner reads, never writes. */
        if(a733_power_domain_check(f->config.domains,1)!=1)return -1;
        if(!read32(f,CCU+0xa00,&value)||(value&0x80000000)||
           !read32(f,CCU+0xa04,&value)||(value&1))return -1;
        for(unsigned i=0;i<2;++i)if(!read32(f,CCU+(i?0x5b4:0x58c),&value)||
            (value&0x10007)!=f->config.iommu_gates[i])return -1;
        break;
    case 1:case 2:{
        int r=update(f,CCU+(f->index==1?0xa74:0xa04),0x10000,0);if(r!=1)return r;
        if(f->index==2)f->until=f->last_now+10;break;}
    case 3:if(!timely(f))return -1;if(f->last_now<f->until)return 0;break;
    case 4:{
        if(!f->domain_requested){
            if(domain_authorize(f)!=1||!a733_power_domain_request(&f->row,1))return -1;
            f->domain_requested=1;
        }
        enum a733_power_row_state r=a733_power_domain_state(&f->row);
        if(r==A733_POWER_ROW_PENDING)return timely(f)?0:-1;
        if(r!=A733_POWER_ROW_ON)return -1;
        break;}
    default:return 1;
    }
    ++f->index;return 0;
}
int a733_display_framebuffer_prepare(fb_t *f)
{
    if(!f||f->self!=f||!current(f))return -1;
    const struct a733_display_framebuffer_config *c=&f->config;
    int r;
    switch(f->stage){
    case 0:
        if(!f->boot_ready||!memory(f))goto fail;
        f->media_config=(struct a733_media_clock_config){.owner=c->owner,
            .handoff=c->authority,.deadline_us=c->deadline,.groups=A733_MEDIA_DE};
        memcpy(f->media_config.fabric,c->fabric,sizeof(c->fabric));
        memcpy(f->media_config.rate_hz,c->fabric_hz,sizeof(c->fabric_hz));
        {struct a733_media_clock_ops ops={now,media_authorize,enter,leave,retain,raw_read,media_write};
        if(!a733_media_clock_init(&f->media,&f->media_config,&ops,f,c->clocks,c->pck,
            c->pck_client,c->pck_lease))goto fail;}++f->stage;return 0;
    case 1:{
        enum a733_clock_result cr=a733_clock_acquire(c->clocks,c->owner,
            A733_CLOCK_FABRIC|A733_CLOCK_REF|A733_CLOCK_OSC,EXCLUSION,c->deadline,&f->exclusion);
        if(cr==A733_CLOCK_BUSY)return 0;if(cr!=A733_CLOCK_OK)goto fail;++f->stage;return 0;}
    case 2:
        r=a733_media_clock_poll(&f->media);
        if(r==A733_MEDIA_BUSY)return 0;
        if(r!=A733_MEDIA_PENDING||!f->media.token.id)goto fail;
        ++f->stage;return 0;
    case 3:
        r=cold_step(f);if(r<0)goto fail;if(r){f->index=0;++f->stage;}return 0;
    case 4:
        r=a733_media_clock_poll(&f->media);
        if(r==A733_MEDIA_BUSY||r==A733_MEDIA_PENDING)return 0;
        if(r!=A733_MEDIA_PREPARED)goto fail;++f->stage;return 0;
    case 5:{
        /* Exact de_top_set_clk_enable() display1+MBUS setup; unrelated fields
         * survive. No other output/RCQ owner exists under the whole-DE token. */
        static const unsigned offsets[]={0x8000,0x8004,0x8008};
        uint32_t mask=f->index==2?0x11:0x10010;
        r=update(f,A733_DE_BASE+offsets[f->index],mask,mask);if(r<0)goto fail;if(!r)return 0;
        if(++f->index==3){f->index=0;++f->stage;}return 0;}
    case 6:{
        struct a733_iommu_ops ops={now,iommu_admit,iommu_read,iommu_write,publish};
        if(a733_iommu_init_display(&f->iommu,&ops,f,&f->tables)!=1||a733_iommu_open(&f->iommu,c->deadline)!=1)goto fail;
        ++f->stage;return 0;}
    case 7:
        r=a733_iommu_poll(&f->iommu);if(r<0)goto fail;if(!r)return 0;
        f->de_config=(struct a733_de_config){.ops={de_admit,read32,write32,barrier,now,
            c->replay?rcq_submit:NULL},.context=f,.lease=f->media.token.id,
            .surface={IOVA+4096,c->frame_span-4096,c->owner,2560},.output=c->output};
        if(!a733_de_init(&f->de,&f->de_config,c->deadline))goto fail;++f->stage;return 0;
    case 8:
        r=a733_de_poll(&f->de);if(r==A733_DE_PENDING)return 0;
        if(r!=A733_DE_READY||!a733_de_request(&f->de,A733_DE_START,c->deadline))goto fail;
        ++f->stage;return 0;
    case 9:
        r=a733_de_poll(&f->de);if(r==A733_DE_PENDING)return 0;
        if(r!=A733_DE_ARMED)goto fail;++f->stage;return 1;
    case 10:return a733_display_framebuffer_source(f,1);
    default:goto fail;
    }
fail:a733_display_framebuffer_retain(f);return -1;
}
int a733_display_framebuffer_stop(fb_t *f)
{
    if(!f||f->self!=f||!current(f))return -1;
    if(f->iommu.state==A733_IOMMU_SCAN){
        int r=a733_iommu_poll(&f->iommu);if(r<=0)return r;
    }
    if(!f->de.initialized||f->de.state==A733_DE_READY)return 1;
    if(f->de.state==A733_DE_PENDING&&f->de.action!=A733_DE_STOP){
        a733_display_framebuffer_retain(f);return -1; /* Never finish START after output stop. */
    }
    if(f->de.state==A733_DE_ARMED&&!a733_de_request(&f->de,A733_DE_STOP,f->config.deadline))return -1;
    enum a733_de_state s=a733_de_poll(&f->de);
    if(s==A733_DE_PENDING)return 0;
    if(s==A733_DE_DISABLED_RETAINED)return 1;
    a733_display_framebuffer_retain(f);return -1;
}
int a733_display_framebuffer_service(fb_t *f)
{
    if(!f||f->self!=f||!current(f)||!owns(f)||!memory(f))return -1;
    if(f->iommu.state==A733_IOMMU_ACTIVE&&a733_iommu_scan(&f->iommu,f->config.deadline)!=1)return -1;
    return a733_iommu_poll(&f->iommu);
}
