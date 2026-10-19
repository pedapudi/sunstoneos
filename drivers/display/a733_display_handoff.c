/* SPDX-License-Identifier: Apache-2.0 */
#include "a733_display_handoff.h"
#include <stddef.h>
#include <string.h>

static int separate(const void *a,size_t an,const void *b,size_t bn)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return a && b && an<=UINTPTR_MAX-x && bn<=UINTPTR_MAX-y && (x+an<=y || y+bn<=x);
}
static int same_token(const struct a733_clock_token *a,const struct a733_clock_token *b)
{return a->id==b->id && a->owner==b->owner && a->shared==b->shared && a->exclusive==b->exclusive;}
static int fault(struct a733_display_handoff *b) {b->failed=1;return -1;}
static int integrity(struct a733_display_handoff *b)
{
    if (!b || b->self!=b || !b->initialized || b->failed) return 0;
    const struct a733_media_clock *m=b->config.media;
    const struct a733_display_pll *p=b->config.pll;
    if (!a733_media_clock_prepared(m) || !a733_display_pll_ready(p) ||
        m->successor!=b || p->successor!=b || m->transferred!=b->transferred || p->transferred!=b->transferred ||
        m->registry!=b->registry || m->pck_manager!=b->pck_manager || m->pck_client!=b->pck_client ||
        m->cookie!=b->media_cookie || memcmp(&m->config,&b->media_config,sizeof(m->config)) ||
        memcmp(&m->ops,&b->media_ops,sizeof(m->ops)) || memcmp(&m->pck,&b->pck,sizeof(m->pck)) ||
        !same_token(&m->token,&b->clock) ||
        m->shared!=(b->transferred ? b->clock.shared : b->media_shared) ||
        m->exclusive!=(b->transferred ? b->clock.exclusive : b->media_exclusive) ||
        memcmp(&p->config,&b->pll_config,sizeof(p->config)) ||
        memcmp(&p->plan,&b->pll_plan,sizeof(p->plan)) ||
        memcmp(p->expected,b->pll_expected,sizeof(p->expected))) {fault(b);return 0;}
    return 1;
}
/* BUSY is yielded by the outer poll before any provider/device operation. */
static int health(struct a733_display_handoff *b)
{
    if (!integrity(b)) return -1;
    enum a733_clock_result c=a733_clock_token_status(b->registry,&b->clock);
    enum a733_gpu_resource_result p=a733_gpu_resources_status(b->pck_manager,b->pck_client,&b->pck);
    if ((c!=A733_CLOCK_OK && c!=A733_CLOCK_BUSY) ||
        (p!=A733_GPU_RESOURCE_OK && p!=A733_GPU_RESOURCE_BUSY)) return fault(b);
    return c==A733_CLOCK_BUSY || p==A733_GPU_RESOURCE_BUSY ? 0 : 1;
}
static int timestamp(void *v,uint64_t *out)
{
    struct a733_display_handoff *b=v;
    if (!integrity(b)) return -1;
    uint64_t n=0;
    int r=b->config.platform.now_us(b->config.context,&n);
    if (!integrity(b) || r!=1 || n<b->last_now) return fault(b);
    b->last_now=n;*out=n;return 1;
}
static int matching(struct a733_display_handoff *b,const struct display_request *r,
                    const struct display_resource_token *t)
{
    return integrity(b) && b->transferred &&
        (!r || !memcmp(r,&b->config.request,sizeof(*r))) &&
        t && !memcmp(t,&b->resources,sizeof(*t));
}
static int resources(void *v,const struct display_request *r,uint64_t deadline,struct display_resource_token *out)
{
    struct a733_display_handoff *b=v;
    if (!integrity(b) || memcmp(r,&b->config.request,sizeof(*r)) ||
        (out->id && memcmp(out,&b->resources,sizeof(*out)))) return fault(b);
    if (b->transferred) {*out=b->resources;return 1;}
    int ok=b->config.authorize_handoff(b->config.context,&b->config,&b->clock,deadline);
    uint64_t n=0;
    if (!integrity(b) || timestamp(b,&n)!=1 || n>=deadline || (ok!=0 && ok!=1)) return fault(b);
    if (!ok) return 0;
    ok=health(b);if (ok!=1) return ok;
    enum a733_clock_result c=a733_clock_publish_stable(b->registry,&b->original,A733_CLOCK_DISPLL1,deadline,&b->clock);
    if (c==A733_CLOCK_BUSY) return 0;
    if (c!=A733_CLOCK_OK) return fault(b);
    /* Only plain metadata stores occur after publication until all live copies
     * and the outward resource identity refer to the persistent fresh token. */
    b->transferred=1;b->config.pll->transferred=1;b->config.media->transferred=1;
    b->config.media->token=b->clock;b->config.media->shared=b->clock.shared;
    b->config.media->exclusive=b->clock.exclusive;
    b->resources=(struct display_resource_token){.id=b->clock.id,.generation=b->clock.id,
        .owner=r->owner,.board_id=r->board_id,.panel_id=r->panel_id,
        .recovery_generation=b->config.recovery_generation,.scopes=DISPLAY_REQUIRED_SCOPES,
        .reference_hz=24000000,.host_hz=150000000,
        .hs_hz=b->pll_config.hs_hz,.ls_hz=b->pll_config.ls_hz};
    *out=b->resources;return 1;
}
static int frame(void *v,const struct display_request *r,const struct display_resource_token *t,
                 uint64_t d,struct display_frame_token *out)
{
    struct a733_display_handoff *b=v;
    if (!matching(b,r,t)) return fault(b);
    int ok=b->config.platform.acquire_frame(b->config.context,r,t,d,out);
    if (!integrity(b) || (ok!=0 && ok!=1)) return fault(b);
    return ok;
}
static int authenticate(void *v,const struct display_request *r,const struct display_ownership *o,
                        enum display_phase p,uint64_t d)
{
    struct a733_display_handoff *b=v;
    if (!matching(b,r,&o->resources)) return fault(b);
    int ok=b->config.platform.authenticate(b->config.context,r,o,p,d);
    return integrity(b) && ok==1 ? 1 : fault(b);
}
static int read32(void *v,const struct display_resource_token *t,uintptr_t a,uint32_t *out)
{
    struct a733_display_handoff *b=v;
    if (!matching(b,0,t)) return fault(b);
    int ok=b->config.platform.read32(b->config.context,t,a,out);
    return integrity(b) && ok==1 ? 1 : fault(b);
}
static int write32(void *v,const struct display_resource_token *t,uintptr_t a,uint32_t value)
{
    struct a733_display_handoff *b=v;
    if (!matching(b,0,t)) return fault(b);
    int ok=b->config.platform.write32(b->config.context,t,a,value);
    return integrity(b) && ok==1 ? 1 : fault(b);
}
static int rcq_submit(void *v,const struct display_resource_token *t,uint64_t deadline)
{struct a733_display_handoff *b=v;if(!matching(b,0,t)||!b->config.platform.rcq_submit)return fault(b);
 int r=b->config.platform.rcq_submit(b->config.context,t,deadline);return integrity(b)&&(r==0||r==1)?r:fault(b);}
static int barrier(void *v,const struct display_resource_token *t)
{
    struct a733_display_handoff *b=v;
    if (!matching(b,0,t)) return fault(b);
    int ok=b->config.platform.barrier(b->config.context,t);
    return integrity(b) && ok==1 ? 1 : fault(b);
}
static int panel(void *v,const struct display_request *r,const struct display_ownership *o,
                 const struct a733_dsi_phy *phy,uint64_t d)
{
    struct a733_display_handoff *b=v;
    if (!matching(b,r,&o->resources)) return fault(b);
    int ok=b->config.platform.prepare_panel(b->config.context,r,o,phy,d);
    return integrity(b) && (ok==0 || ok==1) ? ok : fault(b);
}
static int completion(void *v,const struct display_request *r,const struct display_ownership *o,
                      uint64_t d,struct display_proof *out,unsigned quiet)
{
    struct a733_display_handoff *b=v;
    if (!matching(b,r,&o->resources)) return fault(b);
    int ok=quiet ? b->config.platform.quiet_output(b->config.context,r,o,d,out) :
        b->config.platform.drain_de(b->config.context,r,o,d,out);
    return integrity(b) && (ok==0 || ok==1) ? ok : fault(b);
}
static int drain(void *v,const struct display_request *r,const struct display_ownership *o,uint64_t d,struct display_proof *out)
{return completion(v,r,o,d,out,0);}
static int quiet(void *v,const struct display_request *r,const struct display_ownership *o,uint64_t d,struct display_proof *out)
{return completion(v,r,o,d,out,1);}

int a733_display_handoff_init(struct a733_display_handoff *b,const struct a733_display_handoff_config *c)
{
    if (!b || !c || b->initialized || !a733_media_clock_prepared(c->media) ||
        !a733_display_pll_ready(c->pll) || !c->recovery_generation || !c->authorize_handoff ||
        c->platform.acquire_resources || c->media->successor || c->pll->successor ||
        c->media->transferred || c->pll->transferred) return 0;
    struct a733_media_clock *m=c->media;struct a733_display_pll *p=c->pll;
    const void *objects[]={b,c,m,p,m->registry,m->pck_manager};
    const size_t sizes[]={sizeof(*b),sizeof(*c),sizeof(*m),sizeof(*p),sizeof(*m->registry),sizeof(*m->pck_manager)};
    for (unsigned i=0;i<6;++i) for (unsigned j=i+1;j<6;++j)
        if (!separate(objects[i],sizes[i],objects[j],sizes[j])) return 0;
    const struct display_provider *v=&c->platform;
    if (!v->now_us || !v->acquire_frame || !v->authenticate || !v->read32 || !v->write32 ||
        !v->barrier || !v->prepare_panel || !v->drain_de || !v->quiet_output ||
        m->registry!=p->config.registry || !same_token(&m->token,&p->config.reservation.token) ||
        m->config.owner!=c->request.owner || m->config.handoff!=p->config.reservation.handoff ||
        (m->config.groups&(A733_MEDIA_DE|A733_MEDIA_VO0))!=(A733_MEDIA_DE|A733_MEDIA_VO0) ||
        (m->token.exclusive&(A733_CLOCK_DE_SYS|A733_CLOCK_VO0|A733_CLOCK_DISPLL1))!=
            (A733_CLOCK_DE_SYS|A733_CLOCK_VO0|A733_CLOCK_DISPLL1) ||
        p->config.hs_hz!=c->request.timing.dsi.hs_hz || p->config.ls_hz!=c->request.timing.dsi.ls_hz) return 0;
    static const struct display_provider provider={timestamp,resources,frame,authenticate,read32,write32,barrier,panel,drain,quiet,rcq_submit};
    /* Validate scanout before claiming either producer. */
    if (!display_scanout_init(&b->scanout,&c->request,&provider,b)) return 0;
    b->config=*c;b->self=b;b->media_config=m->config;b->media_ops=m->ops;b->media_cookie=m->cookie;
    b->registry=m->registry;b->pck_manager=m->pck_manager;b->pck=m->pck;b->pck_client=m->pck_client;
    b->pll_config=p->config;b->pll_plan=p->plan;memcpy(b->pll_expected,p->expected,sizeof(b->pll_expected));
    b->media_shared=m->shared;b->media_exclusive=m->exclusive;b->original=b->clock=m->token;
    b->last_now=m->last_now>p->last_now ? m->last_now : p->last_now;
    b->initialized=1;m->successor=b;p->successor=b;return 1;
}
int a733_display_handoff_prepare(struct a733_display_handoff *b,uint64_t deadline)
{return b && !b->entered && integrity(b) && display_scanout_prepare(&b->scanout,deadline);}
int a733_display_handoff_request(struct a733_display_handoff *b,enum display_scanout_action a,uint64_t deadline)
{return b && !b->entered && integrity(b) && display_scanout_request(&b->scanout,a,deadline);}
static enum display_scanout_state retained(struct a733_display_handoff *b)
{
    b->failed=1;
    if (!b->retained) {
        enum a733_clock_result r=a733_clock_retain(b->registry,&b->clock);
        if (r==A733_CLOCK_BUSY) return DISPLAY_SCANOUT_PENDING;
        b->retained=1;
    }
    b->scanout.state=DISPLAY_SCANOUT_FAULT_RETAINED;return DISPLAY_SCANOUT_FAULT_RETAINED;
}
enum display_scanout_state a733_display_handoff_poll(struct a733_display_handoff *b)
{
    if (!b || !b->initialized || b->self!=b) return DISPLAY_SCANOUT_FAULT_RETAINED;
    if (b->entered) {fault(b);return DISPLAY_SCANOUT_PENDING;}
    b->entered=1;
    enum display_scanout_state result=DISPLAY_SCANOUT_PENDING;
    if (b->failed) goto failure;
    int ok=health(b);
    if (ok<0) goto failure;
    if (!ok) goto out;
    result=display_scanout_poll(&b->scanout);
    if (b->failed || result==DISPLAY_SCANOUT_FAULT_RETAINED) goto failure;
    goto out;
failure:result=retained(b);
out:b->entered=0;return result;
}

int a733_display_handoff_replay(struct a733_display_handoff *b,unsigned slot,uint64_t bytes,uint64_t deadline)
{return b&&!b->entered&&integrity(b)&&display_scanout_replay(&b->scanout,slot,bytes,deadline);}
