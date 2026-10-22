/* SPDX-License-Identifier: GPL-2.0-only */
#include "a733_camera_route.h"
#include "a733_csi_receiver.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct model {
    uint32_t regs[2][256];uint8_t parameters[1024];uint64_t now;
    unsigned calls,writes,publishes,stops,drains,releases;
    unsigned pending[5],disable_delay;
    int fail_read,fail_write,drop,late,fail_cb,changed_token,stopped;
};
static int now(void *p,uint64_t *n){*n=((struct model *)p)->now;return 1;}
static int cb(struct model *m,unsigned i,uint64_t t,uint64_t d){
    assert(t==73 && m->now<d);++m->calls;
    if(m->pending[i]){--m->pending[i];return 0;}return m->fail_cb==(int)i+1?-1:1;
}
static int admit(void *p,const struct a733_route_config *c,uint64_t d,uint64_t *t){
    struct model *m=p;assert(c->parser<=1 && c->parameter_address==0x48000000);
    *t=m->changed_token?74:73;return cb(m,0,73,d);
}
static int publish(void *p,uint64_t t,const uint8_t bytes[1024],uint64_t d){
    struct model *m=p;++m->publishes;int r=cb(m,1,t,d);
    if(r==1)memcpy(m->parameters,bytes,1024);return r;
}
static int rd(void *p,uint64_t t,enum a733_route_region r,uint16_t off,uint32_t *v,uint64_t d){
    struct model *m=p;assert(t==73 && r<=1 && off<1024 && !(off&3) && m->now<d);++m->calls;
    if(r==A733_ROUTE_VIPP && off==0x40 && m->stopped && m->disable_delay){
        if(--m->disable_delay==0)m->regs[r][off/4]&=~5u;
    }
    *v=m->regs[r][off/4];return m->fail_read?-1:1;
}
static int wr(void *p,uint64_t t,enum a733_route_region r,uint16_t off,uint32_t v,uint64_t d){
    struct model *m=p;assert(t==73 && r<=1 && off<1024 && !(off&3) && m->now<d);
    ++m->calls;++m->writes;
    if(!m->drop){
        if(r==A733_ROUTE_VIPP && off==0x30)m->regs[r][off/4]&=~v;
        else if(r==A733_ROUTE_VIPP && off==0x40 && m->stopped && m->disable_delay){}
        else m->regs[r][off/4]=v;
    }
    if(m->late)m->now=d;
    return m->fail_write?-1:1;
}
static int stop(void *p,uint64_t t,uint64_t d){
    struct model *m=p;++m->stops;int r=cb(m,2,t,d);if(r==1)m->stopped=1;return r;
}
static int drain(void *p,uint64_t t,uint64_t d){
    struct model *m=p;assert(m->stopped && !(m->regs[1][0]&3) && !(m->regs[1][0x40/4]&5));
    ++m->drains;return cb(m,3,t,d);
}
static int release(void *p,uint64_t t,uint64_t d){
    struct model *m=p;assert(m->drains);++m->releases;return cb(m,4,t,d);
}
static const struct a733_route_ops ops={now,admit,publish,rd,wr,stop,drain,release};
/* Further sizes and byte orders after the VGA images. */
static const struct a733_camera_format formats[]={
    {320,240,A733_CAMERA_UYVY},{1280,720,A733_CAMERA_YUYV},
    {2592,1944,A733_CAMERA_UYVY},{176,144,A733_CAMERA_YUYV}
};
#define FORMATS (sizeof(formats)/sizeof(formats[0]))
static void setup_format(struct a733_camera_route *s,struct model *m,unsigned parser,
                         const struct a733_camera_format *f){
    memset(s,0,sizeof(*s));memset(m,0,sizeof(*m));m->now=1;
    m->regs[0][0x40/4]=4;m->regs[0][0xa0/4]=0;
    m->regs[1][0]=0x80;m->regs[1][0x30/4]=0xffffff03;
    struct a733_route_config c={0x48000000,1088,(uint8_t)parser};
    assert(f?a733_camera_route_init_format(s,&ops,m,&c,f)==1:a733_camera_route_init(s,&ops,m,&c)==1);
    assert(a733_camera_route_open(s,10000)==1);
}
static void setup(struct a733_camera_route *s,struct model *m,unsigned parser){
    setup_format(s,m,parser,NULL);
}
static int step(struct a733_camera_route *s,struct model *m){
    unsigned n=m->calls;int r=a733_camera_route_poll(s);assert(m->calls-n<=1);++m->now;return r;
}
static int complete(struct a733_camera_route *s,struct model *m){
    for(unsigned i=0;i<1000;i++){int r=step(s,m);if(r)return r;}assert(!"unbounded");return -1;
}
static void faulted(struct a733_camera_route *s,struct model *m){
    assert(s->state==A733_ROUTE_FAULT && s->token==73);unsigned n=m->calls;
    assert(a733_camera_route_poll(s)==-1 && a733_camera_route_finish(s,m->now+100)==-1 && n==m->calls);
}
/* Only the receiver's capture-mode behavior is exercised here. Its existing
 * fault/lifecycle evidence remains separate from this changed branch.
 */
struct receiver_model{uint32_t regs[4][256];uint64_t now;unsigned calls,writes;};
static int rnow(void *p,uint64_t *n){*n=((struct receiver_model *)p)->now;return 1;}
static int radmit(void *p,const struct a733_csi_config *c,uint64_t d,uint64_t *t){
    struct receiver_model *m=p;assert(c->port==c->parser && m->now<d);++m->calls;*t=41;return 1;
}
static int rread(void *p,uint64_t t,enum a733_csi_region r,uint16_t off,uint32_t *v,uint64_t d){
    struct receiver_model *m=p;assert(t==41 && m->now<d && off<1024);++m->calls;*v=m->regs[r][off/4];return 1;
}
static int rwrite(void *p,uint64_t t,enum a733_csi_region r,uint16_t off,uint32_t v,uint64_t d){
    struct receiver_model *m=p;assert(t==41 && m->now<d && off<1024);++m->calls;++m->writes;
    if((r==2 && off==0x118)||(r==3 && off==0x44))m->regs[r][off/4]&=~v;
    else m->regs[r][off/4]=v;return 1;
}
static int rcb(void *p,uint64_t t,uint64_t d){
    struct receiver_model *m=p;assert(t==41 && m->now<d);++m->calls;return 1;
}
static void receiver_format(FILE *f,unsigned port,unsigned image,const struct a733_camera_format *format){
    struct receiver_model m={.now=1};struct a733_csi_receiver s={0};
    struct a733_csi_ops o={rnow,radmit,rread,rwrite,rcb,rcb,rcb};
    struct a733_csi_config c={(uint8_t)port,(uint8_t)port,42,2,324000000};
    m.regs[0][0]=0xb0f;
    assert(format?a733_csi_init_format(&s,&o,&m,&c,format)==1:a733_csi_init(&s,&o,&m,&c)==1);
    assert((image?a733_csi_open_image(&s,10000):a733_csi_open(&s,10000))==1);
    while(s.state!=A733_CSI_ARMED){unsigned n=m.calls;assert(a733_csi_poll(&s)>=0);assert(m.calls-n<=1);++m.now;}
    assert(m.regs[3][3]==(image?1u:2u));
    if(f)assert(fwrite(m.regs,1,sizeof(m.regs),f)==sizeof(m.regs));
    if(image){
        m.regs[3][3]=0;m.regs[3][0x44/4]=1; /* SCAP self-clears after one frame. */
        assert(a733_csi_sample(&s,10000)==1);
        while(s.state!=A733_CSI_ARMED){assert(a733_csi_poll(&s)>=0);++m.now;}
        assert(!m.regs[3][3] && a733_csi_open_image(&s,10000)==-1);
    }
}
static void receiver(FILE *f,unsigned port,unsigned image){receiver_format(f,port,image,NULL);}
int main(int argc,char **argv){
    FILE *f=NULL;struct model m;struct a733_camera_route s;
    if(argc==2){f=fopen(argv[1],"wb");assert(f);}
    for(unsigned parser=0;parser<2;parser++){
        setup(&s,&m,parser);for(unsigned i=0;i<5;i++)m.pending[i]=2;
        assert(complete(&s,&m)==1 && s.state==A733_ROUTE_ARMED && s.token==73);
        if(f){assert(fwrite(m.parameters,1,1024,f)==1024);assert(fwrite(m.regs,1,sizeof(m.regs),f)==sizeof(m.regs));}
        assert(m.publishes==3);m.regs[1][0x30/4]=0x100;m.regs[1][0x40/4]&=~4u;
        m.disable_delay=5;assert(a733_camera_route_finish(&s,10000)==1);
        assert(complete(&s,&m)==1 && s.state==A733_ROUTE_CLOSED && !s.token);
        assert(m.releases==3 && s.events==0x100 && a733_camera_route_open(&s,10000)==-1);
    }
    /* Route images for every format precede all receiver images. */
    for(unsigned i=0;i<FORMATS;i++){
        setup_format(&s,&m,i&1,&formats[i]);assert(complete(&s,&m)==1);
        if(f){assert(fwrite(m.parameters,1,1024,f)==1024);assert(fwrite(m.regs,1,sizeof(m.regs),f)==sizeof(m.regs));}
    }
    for(unsigned port=0;port<2;port++)for(unsigned image=0;image<2;image++)receiver(f,port,image);
    for(unsigned i=0;i<FORMATS;i++)for(unsigned port=0;port<2;port++)receiver_format(f,port,1,&formats[i]);
    /* Invalid sizes and orders are refused before any callback. */
    static const struct a733_camera_format bad[]={
        {0,480,0},{648,480,0},{2608,480,0},{640,481,0},{640,1946,0},{640,480,2}
    };
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++){
        struct a733_camera_route r={0};struct a733_csi_receiver v={0};
        struct a733_route_config c={0x48000000,1088,0};
        struct a733_csi_ops o={rnow,radmit,rread,rwrite,rcb,rcb,rcb};
        struct a733_csi_config k={0,0,42,2,324000000};
        assert(a733_camera_route_init_format(&r,&ops,&m,&c,&bad[i])==-1 && !r.initialized);
        assert(a733_csi_init_format(&v,&o,&m,&k,&bad[i])==-1 && !v.initialized);
    }
    if(f)assert(!fclose(f));
    for(unsigned i=0;i<4;i++){
        memset(&s,0,sizeof(s));struct a733_route_config c={0x48000000,1088,0};
        if(i==0)c.parameter_address=UINT64_C(0xffffffc0);
        if(i==1)c.mapped_bytes=1023;
        if(i==2)c.parser=2;
        if(i==3)c.parameter_address++;
        assert(a733_camera_route_init(&s,&ops,&m,&c)==-1);
    }
    setup(&s,&m,0);m.regs[1][0xc0/4]=1;assert(complete(&s,&m)==-1 && !m.writes && !m.publishes);faulted(&s,&m);
    setup(&s,&m,0);m.fail_cb=2;assert(complete(&s,&m)==-1 && !m.writes);faulted(&s,&m);
    setup(&s,&m,0);m.drop=1;assert(complete(&s,&m)==-1);faulted(&s,&m);
    setup(&s,&m,0);m.late=1;assert(complete(&s,&m)==-1);faulted(&s,&m);
    setup(&s,&m,0);m.fail_write=1;assert(complete(&s,&m)==-1);faulted(&s,&m);
    setup(&s,&m,0);assert(step(&s,&m)==0);m.fail_read=1;assert(step(&s,&m)==-1);faulted(&s,&m);
    setup(&s,&m,0);m.pending[0]=2;assert(step(&s,&m)==0);m.changed_token=1;assert(step(&s,&m)==-1);faulted(&s,&m);
    for(unsigned status=0;status<2;status++){
        setup(&s,&m,0);assert(complete(&s,&m)==1);m.regs[1][0x30/4]=status?0x300:0;
        assert(a733_camera_route_finish(&s,10000)==1 && complete(&s,&m)==-1 && !m.releases);faulted(&s,&m);
    }
    for(int id=3;id<=5;id++){
        setup(&s,&m,0);assert(complete(&s,&m)==1);m.regs[1][0x30/4]=0x100;m.fail_cb=id;
        assert(a733_camera_route_finish(&s,10000)==1 && complete(&s,&m)==-1);faulted(&s,&m);
    }
    setup(&s,&m,0);assert(step(&s,&m)==0);m.now=0;assert(step(&s,&m)==-1);faulted(&s,&m);
    setup(&s,&m,0);assert(complete(&s,&m)==1);m.regs[1][0x30/4]=0x100;m.disable_delay=100000;
    assert(a733_camera_route_finish(&s,m.now+30)==1 && complete(&s,&m)==-1);faulted(&s,&m);
    puts("PASS camera route ownership, bounded disable and receiver SCAP mode");return 0;
}
