/* Reuse the existing wire/register fixture helpers, not its test entry point. */
#define s (*bound_scanout)
#define main unused_scanout_suite
#include "display-scanout.c"
#undef main
#undef s
#include "a733_display_handoff.h"

static struct a733_display_handoff handoff;
static struct native_model {
    struct a733_clock_leases registry;
    struct a733_gpu_resource_manager manager;
    struct a733_media_clock media;
    struct a733_display_pll pll;
    uint32_t ccu[0x2000/4],domains[10][3],osc,pck;
    unsigned setup,held,retained,reads,writes,late,busy_after_publish,auth_pending;
} n;
static struct native_model source_checkpoint;
static struct model *register_checkpoint;
static struct display_request request_image;
static int source_now(void *v,uint64_t *out)
{assert(v==&n);*out=m.now++;return 1;}
static uint32_t *source_word(uintptr_t a)
{
    if(a>=0x02002000 && a<0x02004000)return &n.ccu[(a-0x02002000)/4];
    if(a==0x07090160)return &n.osc;
    if(a==0x070101ac)return &n.pck;
    if(a>=0x07060000 && a<=0x07069008)return &n.domains[(a-0x07060000)/0x1000][((a-0x07060000)%0x1000)/4];
    return reg(a);
}
static int source_read(void *v,uintptr_t a,uint32_t *out)
{assert(v==&n && (n.setup || n.held) && !n.retained);++n.reads;*out=*source_word(a);return 1;}
static int source_write(void *v,uintptr_t a,uint32_t value)
{assert(v==&n && n.held && !n.retained);++n.writes;*source_word(a)=value;return 1;}
static int source_enter(void *v,uint64_t deadline)
{assert(v==&n && !n.held && m.now<deadline);n.held=1;return 1;}
static void source_leave(void *v)
{
    assert(v==&n && n.held);n.held=0;
    assert(!a733_media_clock_prepared(&n.media) || n.pll.initialized);
    assert(!a733_display_pll_ready(&n.pll));
}
static void source_retain(void *v){assert(v==&n && n.held);n.retained=1;}
static int source_barrier(void *v){assert(v==&n && n.held);return 1;}
static int media_auth(void *v,const struct a733_media_clock_config *c,
                      const struct a733_clock_token *t,const struct a733_gpu_resource_lease *p)
{
    assert(v==&n && c->handoff==99 && t->owner==4 && p->token);
    return a733_clock_token_status(&n.registry,t)==A733_CLOCK_OK;
}
static int pll_auth(void *v,const struct a733_display_pll_config *c)
{
    assert(v==&n && n.held && c->reservation.handoff==99 && c->initial.id==201);
    assert(a733_media_clock_prepared(&n.media));
    return a733_clock_token_status(&n.registry,&c->reservation.token)==A733_CLOCK_OK;
}
static int gpu_update(void *v,uintptr_t a,uint32_t required,uint32_t set,uint64_t d)
{(void)v;(void)a;(void)required;(void)set;(void)d;assert(0);return 0;}
static int gpu_rail(void *v,struct a733_gpu_resource_rail *r,uint64_t d)
{(void)v;(void)r;(void)d;assert(0);return 0;}
static int consumer_now(void *v,uint64_t *out)
{
    assert(v==&m);
    if(n.late && handoff.transferred) {
        *out=handoff.scanout.deadline;
        if(n.busy_after_publish)atomic_store(&n.registry.busy,1);
        return 1;
    }
    return now(v,out);
}
static int consumer_auth(void *v,const struct display_request *r,const struct display_ownership *o,
                         enum display_phase phase,uint64_t deadline)
{
    assert(v==&m && m.now<deadline && o->resources.id==101 && o->resources.generation==101);
    assert(o->frame.id==102 && o->frame.mapping_generation==6 && o->frame.visibility_generation==7);
    assert(o->frame.device_address==r->frame.device_address && o->frame.cpu_address==r->frame.cpu_address);
    assert(a733_clock_token_status(&n.registry,&handoff.clock)==A733_CLOCK_OK);
    ++m.calls;++m.authenticated[phase];return 1;
}
static int consumer_read(void *v,const struct display_resource_token *t,uintptr_t a,uint32_t *out)
{assert(!n.held);n.held=1;int ok=read32(v,t,a,out);n.held=0;return ok;}
static int consumer_write(void *v,const struct display_resource_token *t,uintptr_t a,uint32_t value)
{assert(!n.held);n.held=1;int ok=write32(v,t,a,value);n.held=0;return ok;}
static int admission(void *v,const struct a733_display_handoff_config *c,
                     const struct a733_clock_token *t,uint64_t deadline)
{
    assert(v==&m && m.now<deadline && c->recovery_generation==3);
    assert(c->media==&n.media && c->pll==&n.pll && t->id==100);
    assert(c->request.owner==4 && c->request.board_id==8 && c->request.panel_id==9);
    assert(n.pll.config.initial.id==201 && n.pll.config.initial.generation==202);
    assert(n.pll.config.reservation.inactive_epoch==102);
    return n.auth_pending ? (n.auth_pending=0) : 1;
}
static struct a733_display_handoff_config config(void)
{
    return (struct a733_display_handoff_config){.media=&n.media,.pll=&n.pll,
        .request=request_image,.recovery_generation=3,.context=&m,.authorize_handoff=admission,
        .platform={consumer_now,NULL,frame,consumer_auth,consumer_read,consumer_write,barrier,panel,drain,quiet,NULL}};
}
static void prepare_sources(void)
{
    memset(&n,0,sizeof(n));memset(&m,0,sizeof(m));m.now=1;n.setup=1;n.pck=1;n.osc=0x10000002;
    n.ccu[0]=0xf8000000u|(99u<<8)|(99u<<16);
    n.ccu[0xa0/4]=0xfe000000u|(99u<<8)|(1u<<20)|(2u<<16)|(4u<<2);
    n.ccu[0x2e0/4]=0xf4000000u|(99u<<8)|(3u<<16);
    n.ccu[0x1908/4]=0x0fff0fff;n.ccu[0x1988/4]=0x0fff0000;
    n.ccu[0x1928/4]=0x00030003;n.ccu[0x19a8/4]=0x00030000;
    n.ccu[0x500/4]=0x03000003;n.ccu[0x580/4]=0x83000001;n.ccu[0x588/4]=0x81000001;
    n.ccu[0x5c0/4]=0xff;n.ccu[0x5e0/4]=0x41055803;n.ccu[0x5e4/4]=0x40302;
    for(unsigned i=0;i<10;++i)n.domains[i][0]=n.domains[i][2]=8;
    assert(a733_clock_leases_init(&n.registry,source_now,&n));
    n.registry.next_id=100; /* Fixture identity; acquisition/publication remain real. */
    const struct a733_gpu_resource_ops go={source_now,source_read,gpu_update,gpu_rail};
    assert(a733_gpu_resources_init(&n.manager,&go,&n));
    uint64_t token;struct a733_gpu_power_resources ready;
    assert(a733_gpu_resources_acquire(&n.manager,2,A733_GPU_RESOURCE_PCK,1000000,&token,&ready)==A733_GPU_RESOURCE_OK);
    struct a733_gpu_resource_lease pck=n.manager.leases[2];
    struct a733_media_clock_config mc={.owner=4,.handoff=99,.deadline_us=1000000,
        .groups=A733_MEDIA_DE|A733_MEDIA_VO0,.fabric={0x03000003,0x83000001,0x81000001},
        .rate_hz={150000000,300000000,300000000}};
    const struct a733_media_clock_ops mo={source_now,media_auth,source_enter,source_leave,source_retain,source_read,source_write};
    assert(a733_media_clock_init(&n.media,&mc,&mo,&n,&n.registry,&n.manager,2,&pck));n.setup=0;
    enum a733_media_clock_result mr=A733_MEDIA_PENDING;
    for(unsigned i=0;mr==A733_MEDIA_PENDING && i<100;++i)mr=a733_media_clock_poll(&n.media);
    assert(mr==A733_MEDIA_PREPARED && a733_media_clock_prepared(&n.media));
    m.ccu[0]=n.ccu[0x1588/4];m.ccu[1]=n.ccu[0x158c/4];m.top[3]=0x20;
    struct a733_dsi_pll_plan factors;assert(a733_dsi_pll_plan(480000000,80000000,&factors));
    struct a733_display_pll_config pc={.ops={source_now,source_enter,source_leave,source_retain,pll_auth,source_read,source_write,source_barrier},
        .context=&n,.registry=&n.registry,.reservation={n.media.token,99,102,103,104},
        .initial={.id=201,.generation=202,.hs_hz=480000000,.ls_hz=80000000,
            .words={factors.factors|0x00800080,0x00120381,0x01372818,0x80000000,0x1f123456,0x01004000,0x172}},
        .hs_hz=480000000,.ls_hz=80000000,.deadline=1000000};
    const unsigned offsets[]={0x104,0x108,0x10c,0x124,0x130,0x134,0x138};
    for(unsigned i=0;i<7;++i)m.phy[offsets[i]/4]=pc.initial.words[i];m.phy[0xe0/4]=1u<<26;
    assert(a733_display_pll_init(&n.pll,&pc));
    for(unsigned i=0;n.pll.state==A733_DISPLAY_PLL_PENDING && i<4000;++i)a733_display_pll_poll(&n.pll);
    assert(a733_display_pll_ready(&n.pll) && !n.held);
    m.de[0x8000/4]=m.de[0x8004/4]=0x10010;m.de[0x8008/4]=0x11;
    request_image=(struct display_request){.owner=4,.board_id=8,.panel_id=9,
        .timing={.dsi={.pixel_hz=80000000,.hs_hz=480000000,.ls_hz=80000000,.width=1280,.height=720,
            .hsync=40,.hback=100,.hfront=80,.vsync=5,.vback=20,.vfront=5,.lanes=4},.actual_dclk_hz=80000000},
        .frame={.cpu_address=0x40000000,.device_address=0x50000000,.mapped_bytes=5120*720,.frame_id=11,.pitch=5120}};
    memcpy(&source_checkpoint,&n,sizeof(n));register_checkpoint=malloc(sizeof(m));assert(register_checkpoint);
    memcpy(register_checkpoint,&m,sizeof(m));
}
static void restore_sources(void)
{
    /* Restore the model at the SAME addresses; production objects cannot move. */
    memcpy(&n,&source_checkpoint,sizeof(n));memcpy(&m,register_checkpoint,sizeof(m));
    memset(&handoff,0,sizeof(handoff));bound_scanout=&handoff.scanout;
}
static void bind(void)
{
    struct a733_display_handoff_config c=config();assert(a733_display_handoff_init(&handoff,&c));
    assert(a733_display_handoff_prepare(&handoff,m.now+100000));
}
static enum display_scanout_state run_handoff(void)
{
    enum display_scanout_state r=DISPLAY_SCANOUT_PENDING;
    for(unsigned i=0;r==DISPLAY_SCANOUT_PENDING && i<5000;++i)r=a733_display_handoff_poll(&handoff);
    return r;
}
#ifndef DISPLAY_NATIVE_REUSE
int main(void)
{
    prepare_sources();restore_sources();
    struct a733_media_clock copied_media=n.media;struct a733_display_pll copied_pll=n.pll;
    assert(!a733_media_clock_prepared(&copied_media) && !a733_display_pll_ready(&copied_pll));
    struct a733_display_handoff_config c=config();c.media=&copied_media;
    assert(!a733_display_handoff_init(&handoff,&c));c=config();c.pll=&copied_pll;
    assert(!a733_display_handoff_init(&handoff,&c));c=config();handoff.config=c;
    assert(!a733_display_handoff_init(&handoff,&handoff.config));memset(&handoff,0,sizeof(handoff));
    bind();struct a733_display_handoff duplicate={0};c=config();assert(!a733_display_handoff_init(&duplicate,&c));
    struct a733_display_handoff copied=handoff;assert(a733_display_handoff_poll(&copied)==DISPLAY_SCANOUT_FAULT_RETAINED);
    atomic_store(&n.registry.busy,1);unsigned calls=m.calls;uint64_t before=m.now;
    assert(a733_display_handoff_poll(&handoff)==DISPLAY_SCANOUT_PENDING && calls==m.calls && before==m.now);
    atomic_store(&n.registry.busy,0);n.auth_pending=1;
    assert(a733_display_handoff_poll(&handoff)==DISPLAY_SCANOUT_PENDING && !handoff.resources.id && !handoff.transferred);
    assert(run_handoff()==DISPLAY_SCANOUT_READY);
    assert(handoff.transferred && handoff.clock.id==101 && n.media.token.id==101 && n.media.transferred && n.pll.transferred);
    assert(n.pll.config.reservation.token.id==100 && handoff.scanout.ownership.resources.generation==101);
    assert(a733_clock_token_status(&n.registry,&handoff.original)==A733_CLOCK_INVALID);
    struct a733_clock_token conflict;
    assert(a733_clock_acquire(&n.registry,55,0,A733_CLOCK_DISPLL1,1000000,&conflict)==A733_CLOCK_BUSY);
    assert(a733_clock_acquire(&n.registry,55,0,A733_CLOCK_VO0,1000000,&conflict)==A733_CLOCK_BUSY);
    assert(a733_display_handoff_request(&handoff,DISPLAY_SCANOUT_START,m.now+10000));
    assert(run_handoff()==DISPLAY_SCANOUT_ACTIVE && handoff.scanout.de.state==A733_DE_ARMED);
    calls=m.calls;n.media.cookie=&m;
    assert(!a733_display_handoff_request(&handoff,DISPLAY_SCANOUT_OBSERVE,m.now+10000));
    assert(a733_display_handoff_poll(&handoff)==DISPLAY_SCANOUT_FAULT_RETAINED && calls==m.calls);
    assert(n.registry.failed && handoff.clock.id==101 && n.media.token.id==101);
    puts("actual media/PLL publication and scanout start; copied/duplicate source, BUSY and post-adoption mutation passed");

    restore_sources();bind();n.pll.config.context=&m;calls=m.calls;
    assert(a733_display_handoff_poll(&handoff)==DISPLAY_SCANOUT_FAULT_RETAINED);
    assert(n.registry.failed && !handoff.transferred && handoff.clock.id==100 && !handoff.resources.id && calls==m.calls);
    restore_sources();bind();n.late=n.busy_after_publish=1;
    assert(a733_display_handoff_poll(&handoff)==DISPLAY_SCANOUT_PENDING && handoff.failed && !handoff.retained);
    assert(handoff.clock.id==101 && n.media.token.id==101 && n.media.transferred && n.pll.transferred);
    assert(handoff.scanout.ownership.resources.id==101 && !handoff.scanout.phy.initialized);
    calls=m.calls;before=m.now;
    assert(a733_display_handoff_poll(&handoff)==DISPLAY_SCANOUT_PENDING && m.calls==calls && m.now==before);
    atomic_store(&n.registry.busy,0);
    assert(a733_display_handoff_poll(&handoff)==DISPLAY_SCANOUT_FAULT_RETAINED && n.registry.failed);
    assert(a733_display_handoff_poll(&handoff)==DISPLAY_SCANOUT_FAULT_RETAINED && m.calls==calls && m.now==before);
    puts("prepublication refusal and postpublication deadline with BUSY retention preserve the only live token");
    free(register_checkpoint);return 0;
}

#endif
