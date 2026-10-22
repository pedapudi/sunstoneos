/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "a733_video_encode_platform.h"
#define CCU UINT32_C(0x02002000)
#define PCK UINT32_C(0x07063000)
#define R_CCU UINT32_C(0x07010000)
#define VE UINT32_C(0x01c10000)
#define GIC UINT32_C(0x03400000)
#define fail a733_video_unit_fail
#define timely a733_video_unit_timely
#define held a733_video_unit_held
#define lock a733_video_unit_lock
#define unlock a733_video_unit_unlock
#define rd a733_video_unit_read
#define wr a733_video_unit_write
static struct a733_video_unit_config unit_config(const struct a733_encode_platform_config *c)
{
    struct a733_video_unit_config u={.clocks=c->clocks,.clock=c->clock,.power=c->power,.pck=c->pck,
        .power_client=c->power_client,.owner=c->owner,.handoff=c->handoff,.module=c->module,.module_hz=c->module_hz};
    for(unsigned i=0;i<3;++i){u.fabric[i]=c->fabric[i];u.fabric_hz[i]=c->fabric_hz[i];}
    return u;
}
int a733_encode_platform_scopes(const struct a733_encode_platform_config *c,a733_clock_mask_t *out)
{
    if(!c)return 0;
    const struct a733_video_unit_config u=unit_config(c);
    return a733_video_unit_scopes(&a733_video_encoder_unit,&u,out);
}
static int encoder_init(struct a733_video_encoder *p,const struct a733_encode_platform_config *c,
    const struct a733_video_unit_ops *o,void *cookie,
    int (*admit)(void *,const struct a733_encode_platform_config *,const void *,uint64_t,uint64_t *),
    int (*memory)(void *,uint64_t,const void *,uint64_t),void *context)
{
    if(!c||p->unit.initialized)return 0;
    const struct a733_encode_platform_config config=*c;
    const struct a733_video_unit_config u=unit_config(&config);
    if(!a733_video_unit_init(&p->unit,&a733_video_encoder_unit,&u,o,cookie))return 0;
    p->config=config;p->admit=admit;p->memory=memory;p->context=context;
    p->bound=p->checked=p->prepared=0;
    return 1;
}
/* The codec binding binds its job on the first call and reports whether a
 * later job equals the bound one. Inherited clock/reset/PCK checks precede the
 * first VE access, then identity is admitted. */
static int acquire(struct a733_video_encoder *p,const void *j,int same,uint64_t d,uint64_t *t)
{
    struct a733_video_unit *u=&p->unit;
    if(!t||!u->initialized||u->failed)return -1;
    if(p->bound&&(!same||d!=u->deadline||*t!=u->token))return fail(u);
    if(!p->bound){u->deadline=d;p->bound=1;}
    int r=p->admit(p->context,&p->config,j,d,t);
    if(!*t||(u->token&&u->token!=*t))return fail(u);
    u->token=*t;
    if(!timely(u)||r<0||r>1)return fail(u);
    if(!r)return 0;
    if(p->checked)return held(u);
    r=lock(u);if(r!=1)return r;
    uint64_t version;
    if(!a733_video_unit_baseline(u)||!a733_video_unit_identity(u,&version))return fail(u);
    if(unlock(u)!=1)return -1;
    p->checked=1;return 1;
}
static int prepare(struct a733_video_encoder *p,uint64_t t,const void *j,int same,uint64_t d)
{
    struct a733_video_unit *u=&p->unit;
    if(u->failed||!p->checked||t!=u->token||d!=u->deadline||!same||!timely(u)||held(u)!=1)return fail(u);
    if(p->prepared)return 1;
    int r=p->memory(p->context,t,j,d);
    if(!timely(u)||held(u)!=1||r<0||r>1)return fail(u);
    if(r==1)p->prepared=1;
    return r;
}
static int access_read(struct a733_video_encoder *p,uint32_t o,uint32_t *x)
{
    struct a733_video_unit *u=&p->unit;
    if(lock(u)!=1)return fail(u);
    if(rd(u,VE+o,x)!=1)return -1;
    return unlock(u);
}
static int access_write(struct a733_video_encoder *p,uint32_t o,uint32_t x)
{
    struct a733_video_unit *u=&p->unit;
    if(lock(u)!=1)return fail(u);
    if(wr(u,VE+o,4,x)!=1)return -1;
    return unlock(u);
}
static int read_allowed(uint32_t o)
{return o==0x800||o==0x804||o==0xb1c||o==0xb90;}

/* JPEG binding. */
static int write_allowed(uint32_t o)
{
    static const uint16_t offsets[]={0x800,0x804,0xa00,0xa04,0xa08,0xa14,0xa2c,0xa30,0xa34,0xa38,
      0xa70,0xa74,0xa78,0xa7c,0xa80,0xa84,0xa88,0xb04,0xb08,0xb14,0xb18,0xb1c,
      0xb80,0xb84,0xb88,0xb8c,0xbe0,0xbe4};
    for(unsigned i=0;i<sizeof(offsets)/sizeof(*offsets);++i)if(o==offsets[i])return 1;return 0;
}
static int jpeg_admit(void *v,const struct a733_encode_platform_config *c,const void *j,uint64_t d,uint64_t *t)
{struct a733_encode_platform *p=v;return p->ops.admit(p->cookie,c,j,d,t);}
static int jpeg_memory(void *v,uint64_t t,const void *j,uint64_t d)
{struct a733_encode_platform *p=v;return p->ops.memory(p->cookie,t,j,d);}
int a733_encode_platform_init(struct a733_encode_platform *p,const struct a733_encode_platform_config *c,
    const struct a733_encode_platform_ops *o,void *cookie)
{
    if(!p||!o||!o->admit||!o->memory||!o->copied)return 0;
    const struct a733_video_unit_ops unit={o->now_us,o->enter,o->leave,o->read,o->write};
    const struct a733_encode_platform_ops ops=*o;
    if(!encoder_init(&p->encoder,c,&unit,cookie,jpeg_admit,jpeg_memory,p))return 0;
    p->ops=ops;p->cookie=cookie;
    return 1;
}
static int same(const struct a733_jpeg_encode_job *a,const struct a733_jpeg_encode_job *b)
{return a->input==b->input&&a->input_bytes==b->input_bytes&&a->input_iova==b->input_iova&&
 a->output_iova==b->output_iova&&a->input_capacity==b->input_capacity&&a->output_capacity==b->output_capacity;}
static int jpeg_now(void *v,uint64_t *out)
{struct a733_encode_platform *p=v;return p?a733_video_unit_now(&p->encoder.unit,out):-1;}
static int jpeg_acquire(void *v,const struct a733_jpeg_encode_job *j,uint64_t d,uint64_t *t)
{
    struct a733_encode_platform *p=v;
    if(!p||!j)return -1;
    if(!p->encoder.bound)p->job=*j;
    return acquire(&p->encoder,j,same(j,&p->job),d,t);
}
static int jpeg_prepare(void *v,uint64_t t,const struct a733_jpeg_encode_job *j,uint64_t d)
{
    struct a733_encode_platform *p=v;
    return p?prepare(&p->encoder,t,j,same(j,&p->job),d):-1;
}
static int jpeg_read(void *v,uint64_t t,uint32_t o,uint32_t *x,uint64_t d)
{
    struct a733_encode_platform *p=v;if(!p)return -1;struct a733_video_unit *u=&p->encoder.unit;
    if(!x||!p->encoder.prepared||t!=u->token||d!=u->deadline||!read_allowed(o))return fail(u);
    return access_read(&p->encoder,o,x);
}
static int jpeg_write(void *v,uint64_t t,uint32_t o,uint32_t x,uint64_t d)
{
    struct a733_encode_platform *p=v;if(!p)return -1;struct a733_video_unit *u=&p->encoder.unit;
    if(!p->encoder.prepared||t!=u->token||d!=u->deadline||!write_allowed(o))return fail(u);
    return access_write(&p->encoder,o,x);
}
static int jpeg_copied(void *v,uint64_t t,size_t n,uint64_t d)
{
    struct a733_encode_platform *p=v;if(!p)return -1;struct a733_video_unit *u=&p->encoder.unit;
    if(!p->encoder.prepared||t!=u->token||d!=u->deadline||n<625||n>A733_JPEG_ENCODE_CAPACITY||
       !timely(u)||held(u)!=1)return fail(u);
    int r=p->ops.copied(p->cookie,t,n,d);
    if(!timely(u)||held(u)!=1||r<0||r>1)return fail(u);
    return r;
}
const struct a733_jpeg_encode_ops *a733_encode_platform_jpeg_ops(void)
{static const struct a733_jpeg_encode_ops ops={jpeg_now,jpeg_acquire,jpeg_prepare,jpeg_read,jpeg_write,jpeg_copied};return &ops;}
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
int a733_encode_native_read(void *unused,uintptr_t a,uint32_t *x)
{
    (void)unused;if(!x||!physical_read_allowed(a))return -1;
    return a733_video_unit_native_read(a,x);
}
int a733_encode_native_write(void *unused,uintptr_t a,unsigned width,uint32_t x)
{
    (void)unused;if(width!=4||a<VE||a>=VE+0x1000||!write_allowed((uint32_t)(a-VE)))return -1;
    return a733_video_unit_native_write(a,4,0,x);
}

/* H264 binding: a distinct immutable span/reference contract on the same
 * encoder sequencing and leases. */
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
int a733_encode_h264_native_write(void *unused,uintptr_t a,unsigned width,uint32_t x)
{
 (void)unused;if(width!=4||a<VE||a>=VE+0x1000||!h264_write_allowed((uint32_t)(a-VE)))return -1;
 return a733_video_unit_native_write(a,4,0,x);
}
static int h264_admit(void *v,const struct a733_encode_platform_config *c,const void *j,uint64_t d,uint64_t *t)
{struct a733_h264_encode_platform *p=v;return p->ops.admit(p->cookie,c,j,d,t);}
static int h264_memory(void *v,uint64_t t,const void *j,uint64_t d)
{struct a733_h264_encode_platform *p=v;return p->ops.memory(p->cookie,t,j,d);}
int a733_h264_encode_platform_init(struct a733_h264_encode_platform *p,const struct a733_encode_platform_config *c,
 const struct a733_h264_encode_platform_ops *o,void *cookie)
{
 if(!p||!o||!o->admit||!o->memory||!o->prime||!o->copied)return 0;
 const struct a733_video_unit_ops unit={o->now_us,o->enter,o->leave,o->read,o->write};
 const struct a733_h264_encode_platform_ops ops=*o;
 if(!encoder_init(&p->encoder,c,&unit,cookie,h264_admit,h264_memory,p))return 0;
 p->ops=ops;p->cookie=cookie;return 1;
}
static int h264_job_same(const struct a733_h264_encode_job *a,const struct a733_h264_encode_job *b)
{
 if(a->frames!=b->frames)return 0;
 for(unsigned i=0;i<2;++i)if(a->input[i]!=b->input[i])return 0;
 for(unsigned i=0;i<A733_H264_ENCODE_BUFFERS;++i)if(a->buffers[i].iova!=b->buffers[i].iova||a->buffers[i].bytes!=b->buffers[i].bytes)return 0;
 return 1;
}
static int h264_now(void *v,uint64_t *out)
{struct a733_h264_encode_platform *p=v;return p?a733_video_unit_now(&p->encoder.unit,out):-1;}
static int h264_acquire(void *v,const struct a733_h264_encode_job *j,uint64_t d,uint64_t *t)
{
 struct a733_h264_encode_platform *p=v;if(!p||!j)return -1;
 if(!p->encoder.bound)p->job=*j;
 return acquire(&p->encoder,j,h264_job_same(j,&p->job),d,t);
}
static int h264_prepare(void *v,uint64_t t,const struct a733_h264_encode_job *j,uint64_t d)
{
 struct a733_h264_encode_platform *p=v;if(!p||!j)return -1;
 return prepare(&p->encoder,t,j,h264_job_same(j,&p->job),d);
}
static int h264_authorized(struct a733_h264_encode_platform *p,uint64_t t,uint64_t d)
{
 if(!p)return 0;struct a733_video_unit *u=&p->encoder.unit;
 return p->encoder.prepared&&!u->failed&&t==u->token&&d==u->deadline&&timely(u)&&held(u)==1;
}
static int h264_prime(void *v,uint64_t t,unsigned ordinal,size_t cursor,uint64_t d)
{
 struct a733_h264_encode_platform *p=v;if(!h264_authorized(p,t,d)||ordinal>=A733_H264_ENCODE_GOP_FRAMES||cursor>2097152)return p?fail(&p->encoder.unit):-1;
 int r=p->ops.prime(p->cookie,t,ordinal,cursor,d);
 if(!h264_authorized(p,t,d)||r<0||r>1)return fail(&p->encoder.unit);return r;
}
static int h264_read(void *v,uint64_t t,uint32_t off,uint32_t *x,uint64_t d)
{
 struct a733_h264_encode_platform *p=v;if(!x||!h264_authorized(p,t,d)||!read_allowed(off))return p?fail(&p->encoder.unit):-1;
 return access_read(&p->encoder,off,x);
}
static int h264_write(void *v,uint64_t t,uint32_t off,uint32_t x,uint64_t d)
{
 struct a733_h264_encode_platform *p=v;if(!h264_authorized(p,t,d)||!h264_write_allowed(off))return p?fail(&p->encoder.unit):-1;
 return access_write(&p->encoder,off,x);
}
static int h264_copied(void *v,uint64_t t,unsigned ordinal,size_t first,size_t end,uint64_t d)
{
 struct a733_h264_encode_platform *p=v;
 if(!h264_authorized(p,t,d)||ordinal>=A733_H264_ENCODE_GOP_FRAMES||first>=end||end>4194304||end-first>1048576)return p?fail(&p->encoder.unit):-1;
 int r=p->ops.copied(p->cookie,t,ordinal,first,end,d);
 if(!h264_authorized(p,t,d)||r<0||r>1)return fail(&p->encoder.unit);return r;
}
const struct a733_h264_encode_ops *a733_encode_platform_h264_ops(void)
{static const struct a733_h264_encode_ops ops={h264_now,h264_acquire,h264_prepare,h264_prime,h264_read,h264_write,h264_copied};return &ops;}
