#include "a733_display_native.h"
#include <string.h>

/* The full walk reads all 8192 uncached table words and translates every
 * frame and table page, so it runs once per scanout phase and again after
 * each prepare, request or replay. Allocation, extent and IOMMU generation
 * checks still run on every access. One owner task serializes this record. */
static const struct a733_display_native *walked;
static enum display_phase walked_phase;
static int memory(struct a733_display_native *n)
{
    const struct a733_display_native_config *c=&n->config;
    struct device_memory_extent f,t;enum device_memory_state fs,ts;
    if(!device_memory_describe(c->pool,c->frame,&f,&fs)||
       !device_memory_describe(c->pool,c->tables,&t,&ts)||fs!=DEVICE_MEMORY_DEVICE||ts!=DEVICE_MEMORY_DEVICE||
       memcmp(&f,&n->frame_extent,sizeof(f))||memcmp(&t,&n->table_extent,sizeof(t))||
       c->iommu->state!=A733_IOMMU_ACTIVE||c->iommu->translated_masters!=0x100||
       c->iommu->accessible_masters!=0x100||memcmp(&c->iommu->tables,&n->tables,sizeof(n->tables)))return 0;
    if(walked==n&&walked_phase==n->binding.scanout.phase)return 1;
    walked=0;
    if(a733_iommu_validate(&n->tables)!=1)return 0;
    /* The one immutable READ span and every table word were validated above.
     * Checking each CPU page needs no repeated full-table translation walk. */
    for(uint64_t off=0;off<f.bytes;off+=DEVICE_MEMORY_PAGE)
        if(!a733_display_native_mapping(f.cpu+off,f.physical+off,0x44))return 0;
    for(uint64_t off=0;off<t.bytes;off+=DEVICE_MEMORY_PAGE)
        if(!a733_display_native_mapping(t.cpu+off,t.physical+off,0x44))return 0;
    walked=n;walked_phase=n->binding.scanout.phase;return 1;
}
static int live(struct a733_display_native *n)
{return n&&n->self==n&&n->initialized&&!n->failed&&a733_display_native_owner(n->config.task);}
static int now(void *v,uint64_t *out)
{
    struct a733_display_native *n=v;uint64_t t;
    if(!live(n)||n->config.now_us(n->config.context,&t)!=1||t<n->last_now)return 0;
    n->last_now=t;*out=t;return 1;
}
static int authority(struct a733_display_native *n,uint64_t d)
{
    uint64_t t;
    return now(n,&t)&&t<d&&memory(n)&&n->config.authorize(n->config.context,&n->config,d)==1&&
        now(n,&t)&&t<d;
}
static int handoff(void *v,const struct a733_display_handoff_config *c,
                   const struct a733_clock_token *token,uint64_t d)
{
    struct a733_display_native *n=v;
    return c==&n->binding.config&&token==&n->binding.clock&&authority(n,d)?1:-1;
}
static int resource(struct a733_display_native *n,const struct display_resource_token *t)
{return live(n)&&n->binding.transferred&&t&&!memcmp(t,&n->binding.resources,sizeof(*t));}
static int frame(void *v,const struct display_request *r,const struct display_resource_token *t,
                 uint64_t d,struct display_frame_token *out)
{
    struct a733_display_native *n=v;
    if(!resource(n,t)||memcmp(r,&n->config.request,sizeof(*r))||!authority(n,d))return -1;
    if(out->id&&memcmp(out,&n->frame_token,sizeof(*out)))return -1;
    *out=n->frame_token;return 1;
}
static int authenticate(void *v,const struct display_request *r,const struct display_ownership *o,
                        enum display_phase p,uint64_t d)
{
    struct a733_display_native *n=v;
    return p!=DISPLAY_STOP_LCD&&p!=DISPLAY_QUIET_OUTPUT&&resource(n,&o->resources)&&
        !memcmp(r,&n->config.request,sizeof(*r))&&!memcmp(&o->frame,&n->frame_token,sizeof(o->frame))&&
        authority(n,d)?1:-1;
}
static int address(uintptr_t a)
{
    return !(a&3)&&((a>=A733_DE_BASE&&a<A733_DE_BASE+0x400000)||
        (a>=A733_TCON_LCD1_BASE&&a<A733_TCON_LCD1_BASE+0x1000)||
        (a>=A733_TCON_TOP_BASE&&a<A733_TCON_TOP_BASE+0x1000)||
        (a>=0x05508000&&a<0x05509000)||(a>=0x05509000&&a<0x0550a000)||
        a==0x02003588||a==0x0200358c);
}
static void retain(struct a733_display_native *n)
{
    n->failed=1;walked=0;
    if(n->held){a733_display_native_retain(n->config.locks);n->held=0;}
    (void)device_memory_retain(n->config.pool,n->config.frame);
    (void)device_memory_retain(n->config.pool,n->config.tables);
}
static int access32(struct a733_display_native *n,const struct display_resource_token *t,
                    uintptr_t a,uint32_t *value,unsigned write)
{
    uint64_t d=n->binding.scanout.deadline;
    if(!resource(n,t)||!value||!address(a)||!authority(n,d)||n->held)return 0;
    if(a733_display_native_enter(n->config.locks,d)!=1)return 0;
    n->held=1;
    uint64_t time;
    if(!now(n,&time)||time>=d||!a733_display_native_mapping(a,a,0)||
       !(write?a733_display_native_write(a,*value):a733_display_native_read(a,value))||
       !now(n,&time)||time>=d){retain(n);return 0;}
    a733_display_native_leave(n->config.locks);n->held=0;
    if(!now(n,&time)||time>=d){retain(n);return 0;}return 1;
}
static int read32(void *v,const struct display_resource_token *t,uintptr_t a,uint32_t *out)
{return access32(v,t,a,out,0);}
static int write32(void *v,const struct display_resource_token *t,uintptr_t a,uint32_t value)
{return access32(v,t,a,&value,1);}
static int rcq_submit(void *v,const struct display_resource_token *t,uint64_t deadline)
{
 struct a733_display_native *n=v;uint64_t time;
 if(!resource(n,t)||!authority(n,deadline)||n->held||a733_display_native_enter(n->config.locks,deadline)!=1)return -1;
 n->held=1;
 if(!now(n,&time)||time>=deadline||!a733_display_native_mapping(A733_DE_BASE+0x8144,A733_DE_BASE+0x8144,0)||
 !a733_display_native_mapping(A733_DE_BASE+0x8150,A733_DE_BASE+0x8150,0)){retain(n);return -1;}
 int r=a733_display_native_rcq_submit();
 if((r!=0&&r!=1)||!now(n,&time)||time>=deadline){retain(n);return -1;}
 a733_display_native_leave(n->config.locks);n->held=0;
 if(!now(n,&time)||time>=deadline){retain(n);return -1;}return r;
}
static int barrier(void *v,const struct display_resource_token *t)
{
    struct a733_display_native *n=v;uint64_t time,d=n->binding.scanout.deadline;
    return resource(n,t)&&now(n,&time)&&time<d&&a733_display_native_barrier()&&now(n,&time)&&time<d;
}
static int panel(void *v,const struct display_request *r,const struct display_ownership *o,
                 const struct a733_dsi_phy *phy,uint64_t d)
{
    struct a733_display_native *n=v;
    if(authenticate(v,r,o,DISPLAY_PREPARE_PANEL,d)!=1)return -1;
    return n->config.panel(n->config.context,r,o,phy,d);
}
static int unsupported(void *v,const struct display_request *r,const struct display_ownership *o,
                       uint64_t d,struct display_proof *out)
{(void)v;(void)r;(void)o;(void)d;(void)out;return -1;}
static int separate(const void *a,size_t an,const void *b,size_t bn)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return a&&b&&an<=UINTPTR_MAX-x&&bn<=UINTPTR_MAX-y&&(x+an<=y||y+bn<=x);
}
int a733_display_native_init(struct a733_display_native *n,const struct a733_display_native_config *c)
{
    if(!n||!c||n->initialized||!c->pool||!c->iommu||!c->locks||!c->task||!c->now_us||!c->authorize||!c->panel||
       c->frame.owner!=c->request.owner||c->tables.owner!=c->request.owner||
       c->frame.slot==c->tables.slot||!c->frame.generation||!c->tables.generation)return 0;
    struct device_memory_extent f,t;enum device_memory_state fs,ts;
    if(!device_memory_describe(c->pool,c->frame,&f,&fs)||!device_memory_describe(c->pool,c->tables,&t,&ts)||
       fs!=DEVICE_MEMORY_DEVICE||ts!=DEVICE_MEMORY_DEVICE||t.bytes!=A733_IOMMU_TABLE_BYTES||
       c->iommu->tables.words!=(void *)t.cpu||c->iommu->tables.physical!=t.physical||
       c->iommu->tables.count!=1||c->iommu->tables.spans[0].iova!=c->request.frame.device_address||
       c->iommu->tables.spans[0].physical!=f.physical||c->iommu->tables.spans[0].bytes!=f.bytes||
       c->iommu->tables.spans[0].access!=A733_IOMMU_READ||c->request.frame.cpu_address!=f.cpu||
       c->request.frame.mapped_bytes!=f.bytes)return 0;
    if(c->replay_frame_bytes&&(c->replay_frame_bytes>UINT64_MAX/2-4096||
       f.bytes!=2*c->replay_frame_bytes+8192||
       !a733_de_replay_valid((void *)(f.cpu+2*c->replay_frame_bytes),c->request.frame.device_address,c->replay_frame_bytes)))return 0;
    const void *objects[]={n,c,c->media,c->pll,c->iommu,c->pool};
    const size_t sizes[]={sizeof(*n),sizeof(*c),sizeof(*c->media),sizeof(*c->pll),sizeof(*c->iommu),sizeof(*c->pool)};
    for(unsigned i=0;i<6;++i){
        for(unsigned j=i+1;j<6;++j)if(!separate(objects[i],sizes[i],objects[j],sizes[j]))return 0;
        if(!separate(objects[i],sizes[i],(void *)f.cpu,f.bytes)||
           !separate(objects[i],sizes[i],(void *)t.cpu,t.bytes))return 0;
    }
    n->config=*c;n->frame_extent=f;n->table_extent=t;n->tables=c->iommu->tables;
    n->frame_token=(struct display_frame_token){.id=c->frame.generation,.generation=c->frame.generation,
        .owner=c->request.owner,.frame_id=c->request.frame.frame_id,.mapping_generation=c->tables.generation,
        .visibility_generation=c->frame.generation,.cpu_address=f.cpu,.device_address=c->request.frame.device_address,
        .mapped_bytes=f.bytes,.pitch=c->request.frame.pitch,.cache_state=DISPLAY_CACHE_IMMUTABLE_DEVICE_READ};
    struct a733_display_handoff_config h={.media=c->media,.pll=c->pll,.request=c->request,
        .recovery_generation=c->recovery_generation,.context=n,.authorize_handoff=handoff,
        .platform={now,0,frame,authenticate,read32,write32,barrier,panel,unsupported,unsupported,rcq_submit}};
    if(!a733_display_handoff_init(&n->binding,&h))return 0;
    n->last_now=n->binding.last_now;n->self=n;n->initialized=1;return 1;
}
int a733_display_native_prepare(struct a733_display_native *n,uint64_t d)
{walked=0;return live(n)&&a733_display_handoff_prepare(&n->binding,d);}
int a733_display_native_request(struct a733_display_native *n,enum display_scanout_action a,uint64_t d)
{walked=0;return live(n)&&a733_display_handoff_request(&n->binding,a,d);}
enum display_scanout_state a733_display_native_poll(struct a733_display_native *n)
{
    if(!n||n->self!=n||!n->initialized)return DISPLAY_SCANOUT_FAULT_RETAINED;
    if(!a733_display_native_owner(n->config.task))return DISPLAY_SCANOUT_FAULT_RETAINED;
    enum display_scanout_state s=a733_display_handoff_poll(&n->binding);
    if(s==DISPLAY_SCANOUT_FAULT_RETAINED)retain(n);
    return s;
}

int a733_display_native_replay(struct a733_display_native *n,unsigned slot,uint64_t deadline)
{
 walked=0;
 if(!live(n)||!n->config.replay_frame_bytes||!authority(n,deadline))return 0;
 return a733_display_handoff_replay(&n->binding,slot,n->config.replay_frame_bytes,deadline);
}
