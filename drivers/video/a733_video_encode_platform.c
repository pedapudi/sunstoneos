/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "a733_video_encode_platform.h"
#include "a733_clock_reference.h"
#define CCU UINT32_C(0x02002000)
#define PCK UINT32_C(0x07063000)
#define R_CCU UINT32_C(0x07010000)
#define VE UINT32_C(0x01c10000)
#define GIC UINT32_C(0x03400000)
#define BIT(n) (UINT32_C(1)<<(n))
static const uint32_t fabric_off[]={0x500,0x580,0x588};
static const uint32_t fabric_mask[]={0x0300001f,0x8700001f,0x8700001f};
static const uint32_t mhz[3][8]={
    {24,0,0,600,0,0,0,0},{24,0,800,600,480,0,0,0},{24,600,0,480,400,0,0,0}
};
int a733_encode_platform_scopes(const struct a733_encode_platform_config *c,a733_clock_mask_t *out)
{
    if(!c||!out||!c->clocks||!c->power||!c->owner||!c->handoff||!c->module_hz||
       c->module_hz>720000000u||(c->module&~0x8700001fu)||!(c->module&BIT(31))||
       ((c->module>>24)&7)>1)return 0;
    a733_clock_mask_t shared=A733_CLOCK_FABRIC|A733_CLOCK_REF|A733_CLOCK_OSC|
        ((c->module&BIT(24))?A733_CLOCK_PLL_VE1:A733_CLOCK_PLL_VE0);
    for(unsigned i=0;i<3;++i) {
        unsigned rate=mhz[i][(c->fabric[i]>>24)&7],div=(c->fabric[i]&31)+1;
        if((c->fabric[i]&~fabric_mask[i])||(i&&!(c->fabric[i]&BIT(31)))||!rate||
           (rate*1000000u)%div||c->fabric_hz[i]!=rate*1000000u/div)return 0;
        if(rate!=24)shared|=A733_CLOCK_PERI0;
    }
    *out=shared;return 1;
}
static int fail(struct a733_encode_platform *p) {p->failed=1;return -1;}
static int clock_now(void *v,uint64_t *out)
{
    struct a733_encode_platform *p=v;uint64_t n;
    if(!p||!p->initialized||p->failed||!out||p->ops.now_us(p->cookie,&n)!=1||n<p->last_now)
        return p?fail(p):-1;
    p->last_now=n;*out=n;return 1;
}
static int timely(struct a733_encode_platform *p)
{uint64_t n;return clock_now(p,&n)==1&&n<p->deadline;}
/* A busy registry is effect-free: another task is inside its short critical
 * section. Retry for at most two scheduler ticks, so an equal-priority holder
 * on this core can finish after a time slice; then report 0 (not held). */
#define BUSY_RETRY_US 2000u
static int held(struct a733_encode_platform *p)
{
    uint64_t first=0,n;
    for(unsigned tries=0;;++tries) {
        enum a733_clock_result c=a733_clock_token_status(p->config.clocks,&p->config.clock);
        if(c!=A733_CLOCK_OK&&c!=A733_CLOCK_BUSY)return fail(p);
        if(c==A733_CLOCK_OK) {
            enum a733_gpu_resource_result g=a733_gpu_resources_status(p->config.power,p->config.power_client,&p->config.pck);
            if(g==A733_GPU_RESOURCE_OK)return 1;
            if(g!=A733_GPU_RESOURCE_BUSY)return fail(p);
        }
        if(clock_now(p,&n)!=1)return -1;
        if(!tries)first=n;
        if(n>=p->deadline||n-first>=BUSY_RETRY_US||tries==65535)return 0;
    }
}
static int lock(struct a733_encode_platform *p)
{
    if(p->failed||!timely(p))return fail(p);
    int h=held(p);if(h!=1)return h;
    if(p->locked)return fail(p);
    int r=p->ops.enter(p->cookie,p->deadline);
    if(r==1)p->locked=1;
    if((r!=0&&r!=1)||!timely(p))return fail(p);
    return r;
}
static int unlock(struct a733_encode_platform *p)
{
    if(p->failed||!p->locked||!timely(p))return fail(p);
    if(p->ops.leave(p->cookie)!=1)return fail(p);
    p->locked=0;
    return timely(p)?1:fail(p);
}
static int rd(struct a733_encode_platform *p,uintptr_t a,uint32_t *v)
{
    if(!p->locked||!timely(p)||p->ops.read(p->cookie,a,v)!=1||!timely(p))return fail(p);
    return 1;
}
static int wr(struct a733_encode_platform *p,uintptr_t a,unsigned width,uint32_t v)
{
    if(!p->locked||!timely(p)||(width==4?p->ops.write(p->cookie,a,v):-1)!=1||!timely(p))return fail(p);
    return 1;
}
static int check(struct a733_encode_platform *p,uintptr_t a,uint32_t mask,uint32_t expected)
{uint32_t v;return rd(p,a,&v)==1&&(v&mask)==expected;}
static int update(struct a733_encode_platform *p,uintptr_t a,uint32_t mask,uint32_t value)
{
    uint32_t old,actual;
    if(rd(p,a,&old)!=1)return 0;
    value=(old&~mask)|(value&mask);
    return wr(p,a,4,value)==1&&rd(p,a,&actual)==1&&actual==value;
}
static int parents(struct a733_encode_platform *p)
{
    uint32_t osc,ref,pll,pat,gate,status;
    if(rd(p,0x07090160,&osc)!=1||rd(p,CCU,&ref)!=1||!a733_clock_ref24_valid(osc,ref))return 0;
    unsigned n=((ref>>8)&255)+1,m=((ref>>16)&127)+1;
    if(n<80||n>116)return 0;
    unsigned v0=!(p->config.module&BIT(24)),off=v0?0x220:0x240;
    if(rd(p,CCU+off,&pll)!=1||(pll&0xf8000000u)!=0xf8000000u||
       rd(p,CCU+off+8,&pat)!=1||(pat&BIT(31))||
       rd(p,CCU+off+12,&pat)!=1||(pat&BIT(27))||
       rd(p,CCU+0x1918,&gate)!=1)return 0;
    unsigned required=v0?(BIT(0)|BIT(16)):(BIT(1)|BIT(17));
    if((gate&required)!=required||rd(p,CCU+0x1998,&status)!=1||
       (status&(required&0xffff0000u))!=(required&0xffff0000u))return 0;
    /* No-Auto, requested enable and effective output status are independent. */
    n=((pll>>8)&255)+1;m=(((pll>>20)&7)+1)*((pll&2)?2:1)*(1+(p->config.module&31));
    if(n<53||n>105||24000000u*n%m||24000000u*n/m!=p->config.module_hz)return 0;
    if(!(p->shared&A733_CLOCK_PERI0))return 1;
    if(rd(p,CCU+0xa0,&pll)!=1||(pll&0xfe000000u)!=0xfe000000u||
       (pll&(BIT(24)|BIT(1)))||rd(p,CCU+0xa8,&pat)!=1||(pat&BIT(31))||
       rd(p,CCU+0xac,&pat)!=1||(pat&BIT(27))||rd(p,CCU+0x1908,&gate)!=1||rd(p,CCU+0x1988,&status)!=1)return 0;
    n=((pll>>8)&255)+1;if(n<53||n>105)return 0;
    for(unsigned i=0;i<3;++i) {
        unsigned div,mask,rate=p->selected_mhz[i];
        switch(rate) {
        case 24:continue;
        case 600:div=(((pll>>20)&7)+1)*2;mask=BIT(11)|BIT(9);break;
        case 400:div=(((pll>>20)&7)+1)*3;mask=BIT(11)|BIT(2)|BIT(1);break;
        case 800:div=((pll>>16)&7)+1;mask=BIT(10);break;
        case 480:div=((pll>>2)&7)+1;mask=BIT(8)|BIT(7);break;
        default:return 0;
        }
        mask|=mask<<16;
        if((gate&mask)!=mask||(status&(mask&0xffff0000u))!=(mask&0xffff0000u)||
           24u*n!=rate*div)return 0;
    }
    return 1;
}
static int baseline(struct a733_encode_platform *p)
{
    /* These inherited clock/reset/PCK checks precede every first VE access.
     * Source: sun60iw2 platform encoder path and domain3 pck600 data. */
    for(unsigned i=0;i<3;++i)
        if(!check(p,CCU+fabric_off[i],fabric_mask[i],p->config.fabric[i]))return 0;
    return parents(p)&&check(p,R_CCU+0x1ac,1,1)&&check(p,PCK,15,8)&&check(p,PCK+8,15,8)&&
      check(p,CCU+0xa80,0x8700001f,p->config.module)&&check(p,CCU+0xa8c,BIT(0)|BIT(16),BIT(0)|BIT(16))&&
      check(p,CCU+0x5c0,BIT(1),BIT(1))&&check(p,CCU+0x5e0,BIT(12),BIT(12))&&
      check(p,CCU+0x5e4,BIT(1),BIT(1))&&check(p,GIC,BIT(31),0)&&
      check(p,GIC+0x108,BIT(16),0)&&check(p,GIC+0x208,BIT(16),0)&&
      check(p,GIC+0x308,BIT(16),0)&&check(p,GIC+0xc14,3,0);
}
int a733_encode_platform_init(struct a733_encode_platform *p,const struct a733_encode_platform_config *c,
    const struct a733_encode_platform_ops *o,void *cookie)
{
    a733_clock_mask_t scopes;
    if(!p||p->initialized||!o||!o->now_us||!o->admit||!o->memory||!o->copied||
       !o->enter||!o->leave||!o->read||!o->write||!a733_encode_platform_scopes(c,&scopes)||
       !c->clock.id||c->clock.owner!=c->owner||c->clock.shared!=scopes||
       c->clock.exclusive!=A733_CLOCK_VE_ENC||c->power_client>=A733_GPU_RESOURCE_CLIENTS||
       !c->pck.token||c->pck.mask!=(A733_GPU_RESOURCE_OSC|A733_GPU_RESOURCE_PCK)||
       c->pck.mode!=A733_GPU_RESOURCE_STABLE)return 0;
    *p=(struct a733_encode_platform){.config=*c,.ops=*o,.cookie=cookie,.shared=scopes,.initialized=1};
    for(unsigned i=0;i<3;++i)p->selected_mhz[i]=mhz[i][(c->fabric[i]>>24)&7];
    return 1;
}
static int same(const struct a733_jpeg_encode_job *a,const struct a733_jpeg_encode_job *b)
{return a->input==b->input&&a->input_bytes==b->input_bytes&&a->input_iova==b->input_iova&&
 a->output_iova==b->output_iova&&a->input_capacity==b->input_capacity&&a->output_capacity==b->output_capacity;}
static int acquire(void *v,const struct a733_jpeg_encode_job *j,uint64_t d,uint64_t *t)
{
    struct a733_encode_platform *p=v;
    if(!p||!j||!t||!p->initialized||p->failed)return -1;
    if(p->bound&&(!same(j,&p->job)||d!=p->deadline||*t!=p->token))return fail(p);
    if(!p->bound){p->job=*j;p->deadline=d;p->bound=1;}
    int r=p->ops.admit(p->cookie,&p->config,j,d,t);
    if(!*t||(p->token&&p->token!=*t))return fail(p);p->token=*t;
    if(!timely(p)||r<0||(r!=0&&r!=1))return fail(p);if(!r)return 0;
    if(p->checked)return held(p);
    r=lock(p);if(r!=1)return r;
    if(!baseline(p))return fail(p);
    uint32_t f,h,l;
    if(rd(p,VE+0x8f0,&f)!=1||f||rd(p,VE+0x8e0,&h)!=1||rd(p,VE+0x8e4,&l)!=1||
       (((uint64_t)(h&0xfffff)<<32)|(l&0xfffff))!=UINT64_C(0x0003331000021320))return fail(p);
    if(unlock(p)!=1)return -1;p->checked=1;return 1;
}
static int prepare(void *v,uint64_t t,const struct a733_jpeg_encode_job *j,uint64_t d)
{
    struct a733_encode_platform *p=v;
    if(!p||p->failed||!p->checked||t!=p->token||d!=p->deadline||!same(j,&p->job)||
       !timely(p)||held(p)!=1)return p?fail(p):-1;
    if(p->prepared)return 1;
    int r=p->ops.memory(p->cookie,t,j,d);
    if(!timely(p)||held(p)!=1||r<0||(r!=0&&r!=1))return fail(p);
    if(r==1)p->prepared=1;return r;
}
static int read_allowed(uint32_t o)
{return o==0x800||o==0x804||o==0xb1c||o==0xb90;}
static int write_allowed(uint32_t o)
{
    static const uint16_t offsets[]={0x800,0x804,0xa00,0xa04,0xa08,0xa14,0xa2c,0xa30,0xa34,0xa38,
      0xa70,0xa74,0xa78,0xa7c,0xa80,0xa84,0xa88,0xb04,0xb08,0xb14,0xb18,0xb1c,
      0xb80,0xb84,0xb88,0xb8c,0xbe0,0xbe4};
    for(unsigned i=0;i<sizeof(offsets)/sizeof(*offsets);++i)if(o==offsets[i])return 1;return 0;
}
static int read_codec(void *v,uint64_t t,uint32_t o,uint32_t *x,uint64_t d)
{
    struct a733_encode_platform *p=v;
    if(!p||!x||!p->prepared||t!=p->token||d!=p->deadline||!read_allowed(o))return p?fail(p):-1;
    if(lock(p)!=1)return fail(p);if(rd(p,VE+o,x)!=1)return -1;return unlock(p);
}
static int write_codec(void *v,uint64_t t,uint32_t o,uint32_t x,uint64_t d)
{
    struct a733_encode_platform *p=v;
    if(!p||!p->prepared||t!=p->token||d!=p->deadline||!write_allowed(o))return p?fail(p):-1;
    if(lock(p)!=1)return fail(p);if(wr(p,VE+o,4,x)!=1)return -1;return unlock(p);
}
static int copied(void *v,uint64_t t,size_t n,uint64_t d)
{
    struct a733_encode_platform *p=v;
    if(!p||!p->prepared||t!=p->token||d!=p->deadline||n<625||n>A733_JPEG_ENCODE_CAPACITY||
       !timely(p)||held(p)!=1)return p?fail(p):-1;
    int r=p->ops.copied(p->cookie,t,n,d);
    if(!timely(p)||held(p)!=1||r<0||(r!=0&&r!=1))return fail(p);return r;
}
const struct a733_jpeg_encode_ops *a733_encode_platform_jpeg_ops(void)
{static const struct a733_jpeg_encode_ops ops={clock_now,acquire,prepare,read_codec,write_codec,copied};return &ops;}
static int physical_read_allowed(uintptr_t a)
{
    static const uint32_t fixed[]={R_CCU+0x1ac,PCK,PCK+8,0x07090160,
      CCU,CCU+0x220,CCU+0x228,CCU+0x22c,CCU+0x240,CCU+0x248,CCU+0x24c,
      CCU+0x1918,CCU+0x1998,CCU+0xa0,CCU+0xa8,CCU+0xac,CCU+0x1908,CCU+0x1988,
      CCU+0x500,CCU+0x580,CCU+0x588,CCU+0xa80,CCU+0xa8c,CCU+0x5c0,CCU+0x5e0,CCU+0x5e4,
      GIC,GIC+0x108,GIC+0x208,GIC+0x308,GIC+0xc14,VE+0x8f0,VE+0x8e0,VE+0x8e4};
    for(unsigned i=0;i<sizeof(fixed)/sizeof(*fixed);++i)if(a==fixed[i])return 1;
    return a>=VE&&a<VE+0x1000&&read_allowed((uint32_t)(a-VE));
}
#ifdef __aarch64__
#include "aarch64_device_memory.h"
static int device(uintptr_t a)
{struct aarch64_translation t;return aarch64_device_translate(a,4,&t)&&t.physical==a&&t.attributes==0&&t.shareability==2;}
#endif
int a733_encode_native_read(void *unused,uintptr_t a,uint32_t *x)
{
    (void)unused;if(!x||!physical_read_allowed(a))return -1;
#ifdef __aarch64__
    if(!device(a))return -1;__asm__ volatile("dsb sy":::"memory");*x=*(volatile uint32_t *)a;
    __asm__ volatile("dsb sy":::"memory");return 1;
#else
    return -1;
#endif
}
int a733_encode_native_write(void *unused,uintptr_t a,uint32_t x)
{
    (void)unused;if(a<VE||a>=VE+0x1000||!write_allowed((uint32_t)(a-VE)))return -1;
#ifdef __aarch64__
    if(!device(a))return -1;__asm__ volatile("dsb sy":::"memory");*(volatile uint32_t *)a=x;
    __asm__ volatile("dsb sy":::"memory");return 1;
#else
    return -1;
#endif
}

/* H264 has a distinct immutable span/reference contract. It shares the real
 * lease/PCK/parent/mutex coordinator and never fabricates a JPEG job. */
static int h264_write_allowed(uint32_t o)
{
 static const uint16_t offsets[]={0x800,0x804,0xa00,0xa04,0xa08,0xa14,0xa2c,0xa30,0xa34,0xa38,
  0xa70,0xa74,0xa78,0xa7c,0xa80,0xa84,0xa88,0xb04,0xb08,0xb0c,0xb10,0xb14,0xb18,0xb1c,0xb20,
  0xb28,0xb2c,0xb30,0xb34,0xb38,0xb3c,0xb40,0xb48,0xb4c,0xb60,0xb70,0xb74,0xb80,0xb84,
  0xb88,0xb8c,0xb94,0xb98,0xb9c,0xba0,0xba4,0xba8,0xbb0,0xbb4,0xbb8,0xbbc,0xbc0,0xbc4,
  0xbcc,0xbd0,0xbd4,0xbd8,0xbdc,0xbe8,0xbec,0xbf0,0xbf4,0xbf8,0xbfc,0xc10,0xc14,0xc18,
  0xc1c,0xc20,0xcc4,0xcf8,0xcfc};
 for(unsigned i=0;i<sizeof(offsets)/sizeof(*offsets);++i)if(o==offsets[i])return 1;return 0;
}
int a733_encode_h264_native_write(void *unused,uintptr_t a,uint32_t x)
{
 (void)unused;if(a<VE||a>=VE+0x1000||!h264_write_allowed((uint32_t)(a-VE)))return -1;
#ifdef __aarch64__
 if(!device(a))return -1;__asm__ volatile("dsb sy":::"memory");*(volatile uint32_t *)a=x;
 __asm__ volatile("dsb sy":::"memory");return 1;
#else
 return -1;
#endif
}
static int no_jpeg_admission(void *v,const struct a733_encode_platform_config *c,
 const struct a733_jpeg_encode_job *j,uint64_t d,uint64_t *t)
{(void)v;(void)c;(void)j;(void)d;(void)t;return -1;}
static int no_jpeg_memory(void *v,uint64_t t,const struct a733_jpeg_encode_job *j,uint64_t d)
{(void)v;(void)t;(void)j;(void)d;return -1;}
static int no_jpeg_copy(void *v,uint64_t t,size_t n,uint64_t d)
{(void)v;(void)t;(void)n;(void)d;return -1;}
int a733_h264_encode_platform_init(struct a733_h264_encode_platform *p,const struct a733_encode_platform_config *c,
 const struct a733_h264_encode_platform_ops *o,void *cookie)
{
 if(!p||p->common.initialized||!o||!o->now_us||!o->admit||!o->memory||!o->prime||!o->copied||
    !o->enter||!o->leave||!o->read||!o->write)return 0;
 struct a733_encode_platform_ops base={o->now_us,no_jpeg_admission,no_jpeg_memory,no_jpeg_copy,
   o->enter,o->leave,o->read,o->write};
 if(!a733_encode_platform_init(&p->common,c,&base,cookie))return 0;
 p->ops=*o;p->cookie=cookie;return 1;
}
static int h264_job_same(const struct a733_h264_encode_job *a,const struct a733_h264_encode_job *b)
{
 for(unsigned i=0;i<2;++i)if(a->input[i]!=b->input[i])return 0;
 for(unsigned i=0;i<A733_H264_ENCODE_BUFFERS;++i)if(a->buffers[i].iova!=b->buffers[i].iova||a->buffers[i].bytes!=b->buffers[i].bytes)return 0;
 return 1;
}
static int h264_now(void *v,uint64_t *out)
{struct a733_h264_encode_platform *p=v;return p?clock_now(&p->common,out):-1;}
static int h264_acquire(void *v,const struct a733_h264_encode_job *j,uint64_t d,uint64_t *t)
{
 struct a733_h264_encode_platform *p=v;if(!p||!j||!t)return -1;
 struct a733_encode_platform *c=&p->common;
 if(!c->initialized||c->failed)return -1;
 if(p->bound&&(!h264_job_same(j,&p->job)||d!=c->deadline||*t!=c->token))return fail(c);
 if(!p->bound){p->job=*j;p->bound=1;c->deadline=d;}
 int r=p->ops.admit(p->cookie,&c->config,j,d,t);
 if(!*t||(c->token&&c->token!=*t))return fail(c);c->token=*t;
 if(!timely(c)||r<0||r>1)return fail(c);if(!r)return 0;
 if(c->checked)return held(c);
 r=lock(c);if(r!=1)return r;
 if(!baseline(c))return fail(c);
 uint32_t f,h,l;
 if(rd(c,VE+0x8f0,&f)!=1||f||rd(c,VE+0x8e0,&h)!=1||rd(c,VE+0x8e4,&l)!=1||
   (((uint64_t)(h&0xfffff)<<32)|(l&0xfffff))!=UINT64_C(0x0003331000021320))return fail(c);
 if(unlock(c)!=1)return -1;c->checked=1;return 1;
}
static int h264_prepare(void *v,uint64_t t,const struct a733_h264_encode_job *j,uint64_t d)
{
 struct a733_h264_encode_platform *p=v;if(!p||!j)return -1;struct a733_encode_platform *c=&p->common;
 if(c->failed||!c->checked||t!=c->token||d!=c->deadline||!h264_job_same(j,&p->job)||!timely(c)||held(c)!=1)return fail(c);
 if(c->prepared)return 1;int r=p->ops.memory(p->cookie,t,j,d);
 if(!timely(c)||held(c)!=1||r<0||r>1)return fail(c);if(r==1)c->prepared=1;return r;
}
static int h264_authorized(struct a733_h264_encode_platform *p,uint64_t t,uint64_t d)
{
 if(!p)return 0;struct a733_encode_platform *c=&p->common;
 return c->prepared&&!c->failed&&t==c->token&&d==c->deadline&&timely(c)&&held(c)==1;
}
static int h264_prime(void *v,uint64_t t,unsigned ordinal,size_t cursor,uint64_t d)
{
 struct a733_h264_encode_platform *p=v;if(!h264_authorized(p,t,d)||ordinal>1||cursor>2097152)return p?fail(&p->common):-1;
 int r=p->ops.prime(p->cookie,t,ordinal,cursor,d);
 if(!h264_authorized(p,t,d)||r<0||r>1)return fail(&p->common);return r;
}
static int h264_read(void *v,uint64_t t,uint32_t off,uint32_t *x,uint64_t d)
{
 struct a733_h264_encode_platform *p=v;if(!x||!h264_authorized(p,t,d)||!read_allowed(off))return p?fail(&p->common):-1;
 if(lock(&p->common)!=1)return fail(&p->common);if(rd(&p->common,VE+off,x)!=1)return -1;return unlock(&p->common);
}
static int h264_write(void *v,uint64_t t,uint32_t off,uint32_t x,uint64_t d)
{
 struct a733_h264_encode_platform *p=v;if(!h264_authorized(p,t,d)||!h264_write_allowed(off))return p?fail(&p->common):-1;
 if(lock(&p->common)!=1)return fail(&p->common);if(wr(&p->common,VE+off,4,x)!=1)return -1;return unlock(&p->common);
}
static int h264_copied(void *v,uint64_t t,unsigned ordinal,size_t first,size_t end,uint64_t d)
{
 struct a733_h264_encode_platform *p=v;
 if(!h264_authorized(p,t,d)||ordinal>1||first>=end||end>4194304||end-first>1048576)return p?fail(&p->common):-1;
 int r=p->ops.copied(p->cookie,t,ordinal,first,end,d);
 if(!h264_authorized(p,t,d)||r<0||r>1)return fail(&p->common);return r;
}
const struct a733_h264_encode_ops *a733_encode_platform_h264_ops(void)
{static const struct a733_h264_encode_ops ops={h264_now,h264_acquire,h264_prepare,h264_prime,h264_read,h264_write,h264_copied};return &ops;}
