/* SPDX-License-Identifier: GPL-2.0-only */
#include "a733_camera_dma.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct model {
    uint32_t reg[1024];uint64_t now;
    unsigned calls,writes,starts,stops,drains,syncs,releases;
    unsigned pending[6];int fail_cb,fail_read,fail_write,drop_write,late,clock_fail;
    int stopped,drained,token_change;
};
static int now(void *p,uint64_t *v) {
    struct model *m=p;*v=m->now;return m->clock_fail?-1:1;
}
static int admission(void *p,const struct a733_camera_dma_config *c,uint64_t d,uint64_t *t) {
    struct model *m=p;assert(c->frame_id==19 && m->now<d);++m->calls;
    *t=m->token_change?74:73;
    if(m->pending[0]) {--m->pending[0];return 0;}return m->fail_cb==1?-1:1;
}
static int rd(void *p,uint64_t t,uint16_t off,uint32_t *v,uint64_t d) {
    struct model *m=p;assert(t==73 && off<4096 && !(off&3) && m->now<d);
    ++m->calls;*v=m->reg[off/4];return m->fail_read?-1:1;
}
static int wr(void *p,uint64_t t,uint16_t off,uint32_t v,uint64_t d) {
    struct model *m=p;assert(t==73 && off<4096 && !(off&3) && m->now<d);
    ++m->calls;++m->writes;
    if(m->starts && (off==0 || off==0x200))assert(m->stopped);
    if(!m->drop_write) {
        if(off==0x254)m->reg[off/4]&=~v;else m->reg[off/4]=v;
    }
    if(m->late)m->now=d;
    return m->fail_write?-1:1;
}
static int cb(struct model *m,unsigned id,uint64_t t,uint64_t d) {
    assert(t==73 && m->now<d);++m->calls;
    if(m->pending[id]){--m->pending[id];return 0;}
    return m->fail_cb==(int)id+1?-1:1;
}
static int start(void *p,uint64_t t,uint64_t d) {
    struct model *m=p;assert((m->reg[0]&1) && (m->reg[0x200/4]&1));++m->starts;
    return cb(m,1,t,d);
}
static int stop(void *p,uint64_t t,uint64_t d) {
    struct model *m=p;++m->stops;int r=cb(m,2,t,d);if(r==1)m->stopped=1;return r;
}
static int drain(void *p,uint64_t t,uint64_t d) {
    struct model *m=p;assert(m->stopped && !(m->reg[0]&1) && !(m->reg[0x200/4]&1));
    ++m->drains;int r=cb(m,3,t,d);if(r==1)m->drained=1;return r;
}
static int sync_cpu(void *p,uint64_t t,uint64_t d) {
    struct model *m=p;assert(m->drained && !(m->reg[0x254/4]));++m->syncs;return cb(m,4,t,d);
}
static int release(void *p,uint64_t t,uint64_t d) {
    struct model *m=p;assert(m->syncs && m->drained);++m->releases;
    int r=cb(m,5,t,d);if(m->late)m->now=d;return r;
}
static int slot_sync(void *p,uint64_t t,unsigned slot,uint64_t d) {(void)slot;return cb(p,4,t,d);}
static const struct a733_camera_dma_ops ops={now,admission,rd,wr,start,stop,drain,sync_cpu,release,NULL,NULL};
static struct a733_camera_dma_config cfg(unsigned block) {
    return (struct a733_camera_dma_config){.y={0x140000000,307264},
        .uv={0x140100000,307264},.frame_id=19,.block=(uint8_t)block};
}
static void init(struct a733_camera_dma *s,struct model *m,unsigned block) {
    memset(s,0,sizeof(*s));memset(m,0,sizeof(*m));m->now=1;
    m->reg[0]=0x80000000;m->reg[0x204/4]=0x40000000;
    m->reg[0x254/4]=0x2fe0e7;
    struct a733_camera_dma_config c=cfg(block);
    assert(a733_camera_dma_init(s,&ops,m,&c)==1);
    assert(a733_camera_dma_open(s,10000)==1 && s->owner==A733_CAMERA_RESERVED);
}
static int step(struct a733_camera_dma *s,struct model *m) {
    unsigned n=m->calls;int r=a733_camera_dma_poll(s);assert(m->calls-n<=1);++m->now;return r;
}
static int complete(struct a733_camera_dma *s,struct model *m) {
    for(unsigned n=0;n<1000;n++){int r=step(s,m);if(r)return r;}assert(!"unbounded");return -1;
}
static void arm(struct a733_camera_dma *s,struct model *m) {
    assert(complete(s,m)==1 && s->state==A733_CAM_DMA_ARMED && s->owner==A733_CAMERA_DEVICE);
}
static void wait_frame(struct a733_camera_dma *s,struct model *m) {
    assert(a733_camera_dma_capture(s,m->now+1000)==1);
    while(s->state!=A733_CAM_DMA_WAIT)assert(step(s,m)==0);
}
static void done(struct a733_camera_dma *s,struct model *m) {
    wait_frame(s,m);m->reg[0x254/4]=0x83;
    assert(complete(s,m)==1 && s->state==A733_CAM_DMA_READY && s->owner==A733_CAMERA_CALLER);
}
static void faulted(struct a733_camera_dma *s,struct model *m) {
    assert(s->state==A733_CAM_DMA_FAULT && s->token==73);
    assert(s->owner!=A733_CAMERA_CALLER);unsigned n=m->calls;
    assert(a733_camera_dma_poll(s)==-1 && a733_camera_dma_capture(s,m->now+10)==-1);
    assert(a733_camera_dma_close(s,m->now+10)==-1 && n==m->calls);
}
int main(int argc,char **argv) {
    struct a733_camera_dma s;struct model m;FILE *f=NULL;
    if(argc==2){f=fopen(argv[1],"wb");assert(f);}
    for(unsigned block=0;block<6;block++) {
        init(&s,&m,block);for(unsigned i=0;i<6;i++)m.pending[i]=2;arm(&s,&m);
        if(f)assert(fwrite(m.reg,1,sizeof(m.reg),f)==sizeof(m.reg));
        done(&s,&m);assert(m.drains>=3 && m.syncs>=3 && s.events==0x83);
        assert(a733_camera_dma_close(&s,m.now+1000)==1 && complete(&s,&m)==1);
        assert(s.state==A733_CAM_DMA_CLOSED && !s.token);
        assert(a733_camera_dma_open(&s,m.now+1000)==-1);
    }
    /* Further sizes on block 0; each plane span covers exactly its bytes. */
    static const struct a733_camera_format sizes[]={
        {320,240,A733_CAMERA_UYVY},{1280,720,A733_CAMERA_YUYV},
        {2592,1944,A733_CAMERA_UYVY},{176,144,A733_CAMERA_YUYV}
    };
    for(unsigned i=0;i<sizeof(sizes)/sizeof(sizes[0]);i++) {
        uint64_t bytes=a733_camera_plane_bytes(&sizes[i]);
        struct a733_camera_dma_config c={.y={0x140000000,bytes},.uv={0x141000000,bytes},
            .frame_id=19,.block=0};
        memset(&s,0,sizeof(s));memset(&m,0,sizeof(m));m.now=1;
        m.reg[0]=0x80000000;m.reg[0x204/4]=0x40000000;m.reg[0x254/4]=0x2fe0e7;
        assert(a733_camera_dma_init_format(&s,&ops,&m,&c,&sizes[i])==1);
        assert(a733_camera_dma_open(&s,10000)==1);arm(&s,&m);
        if(f)assert(fwrite(m.reg,1,sizeof(m.reg),f)==sizeof(m.reg));
        memset(&s,0,sizeof(s));c.uv.mapped_bytes=bytes-1;
        assert(a733_camera_dma_init_format(&s,&ops,&m,&c,&sizes[i])==-1);
        struct a733_camera_format bad=sizes[i];bad.width+=8;c.uv.mapped_bytes=UINT64_C(1)<<24;
        assert(a733_camera_dma_init_format(&s,&ops,&m,&c,&bad)==-1 && !s.initialized);
    }
    if(f)assert(!fclose(f));
    /* Full mapping spans, not only first addresses, must fit and be disjoint. */
    for(unsigned i=0;i<7;i++) {
        memset(&s,0,sizeof(s));struct a733_camera_dma_config c=cfg(0);
        switch(i) {
        case 0:c.uv.bus_address=c.y.bus_address+307200;break;
        case 1:c.y.bus_address=(UINT64_C(1)<<34)-64;break;
        case 2:c.y.mapped_bytes=307199;break;
        case 3:c.y.bus_address++;break;
        case 4:c.y.mapped_bytes=UINT64_MAX;break;
        case 5:c.block=6;break;
        default:c.frame_id=0;break;
        }
        assert(a733_camera_dma_init(&s,&ops,&m,&c)==-1);
    }
    init(&s,&m,0);m.reg[0x600/4]=1;assert(complete(&s,&m)==-1 && !m.writes);faulted(&s,&m);
    init(&s,&m,0);m.drop_write=1;assert(complete(&s,&m)==-1);faulted(&s,&m);
    init(&s,&m,0);m.fail_write=1;assert(complete(&s,&m)==-1);faulted(&s,&m);
    init(&s,&m,0);m.late=1;assert(complete(&s,&m)==-1);faulted(&s,&m);
    init(&s,&m,0);assert(step(&s,&m)==0);m.fail_read=1;assert(step(&s,&m)==-1);faulted(&s,&m);
    init(&s,&m,0);assert(step(&s,&m)==0);m.now=0;assert(step(&s,&m)==-1);faulted(&s,&m);
    init(&s,&m,0);assert(step(&s,&m)==0);m.clock_fail=1;assert(step(&s,&m)==-1);faulted(&s,&m);
    init(&s,&m,0);m.pending[0]=2;assert(step(&s,&m)==0);m.token_change=1;
    assert(step(&s,&m)==-1);faulted(&s,&m);
    /* Video FRAME_DONE alone cannot complete an image capture. */
    init(&s,&m,0);arm(&s,&m);wait_frame(&s,&m);m.reg[0x254/4]=2;
    assert(step(&s,&m)==0 && s.state==A733_CAM_DMA_WAIT && !m.stops);
    /* Capture completion with an error never transfers ownership. */
    init(&s,&m,0);arm(&s,&m);wait_frame(&s,&m);m.reg[0x254/4]=5;
    assert(step(&s,&m)==-1 && !m.stops && !m.syncs);faulted(&s,&m);
    /* A late DMA fault is caught after verified drain, before CPU sync. */
    init(&s,&m,0);arm(&s,&m);wait_frame(&s,&m);m.reg[0x254/4]=1;
    while(s.state!=A733_CAM_DMA_FINAL_STATUS)assert(step(&s,&m)==0);
    m.reg[0x254/4]|=0x200000;assert(step(&s,&m)==-1 && !m.syncs);faulted(&s,&m);
    init(&s,&m,0);arm(&s,&m);wait_frame(&s,&m);m.now=s.deadline;
    assert(step(&s,&m)==-1);faulted(&s,&m);
    for(int id=2;id<=5;id++) {
        init(&s,&m,0);arm(&s,&m);m.fail_cb=id;
        assert(a733_camera_dma_capture(&s,m.now+1000)==1);m.reg[0x254/4]=1;
        assert(complete(&s,&m)==-1);faulted(&s,&m);
    }
    init(&s,&m,0);arm(&s,&m);done(&s,&m);m.late=1;
    assert(a733_camera_dma_close(&s,m.now+1000)==1 && complete(&s,&m)==-1);
    assert(s.state==A733_CAM_DMA_FAULT && !s.token && s.owner==A733_CAMERA_CALLER);
    /* Continuous capture admission: ARMED only, both stream operations, a
     * ring of 2..4 page-multiple slots that each cover one plane and fit
     * both spans. Refusals change nothing. */
    {
        static const struct a733_camera_dma_ops partial={now,admission,rd,wr,start,stop,drain,sync_cpu,release,start,NULL};
        struct a733_camera_dma_ops full=partial;full.sync_slot=slot_sync;
        struct a733_camera_dma_config c={.y={0x140000000,4*311296},.uv={0x141000000,4*311296},.frame_id=19};
        static const struct { unsigned slots;uint64_t bytes; } bad[]={
            {1,311296},{5,311296},{4,303104},{4,311296+64},{4,311296+4096}};
        memset(&s,0,sizeof(s));memset(&m,0,sizeof(m));m.now=1;
        assert(a733_camera_dma_init(&s,&partial,&m,&c)==1 && a733_camera_dma_open(&s,10000)==1);
        assert(a733_camera_dma_stream(&s,2,311296,10000)==-1);arm(&s,&m);
        assert(a733_camera_dma_stream(&s,2,311296,10000)==-1);
        memset(&s,0,sizeof(s));memset(&m,0,sizeof(m));m.now=1;
        assert(a733_camera_dma_init(&s,&full,&m,&c)==1 && a733_camera_dma_open(&s,10000)==1);
        assert(a733_camera_dma_stream(&s,2,311296,10000)==-1);arm(&s,&m);
        struct a733_camera_dma saved=s;
        for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++){
            assert(a733_camera_dma_stream(&s,bad[i].slots,bad[i].bytes,10000)==-1);
            assert(!memcmp(&saved,&s,sizeof(s)));
        }
        unsigned slot;uint32_t sequence;
        assert(a733_camera_dma_take(&s,&slot,&sequence)==-1 && a733_camera_dma_give(&s,0)==-1);
        assert(a733_camera_dma_stop_stream(&s,10000)==-1);
        assert(a733_camera_dma_stream(&s,4,311296,10000)==1 && s.stream && s.ring.state[0]==A733_SLOT_NEXT);
        assert(a733_camera_dma_stream(&s,4,311296,10000)==-1 && a733_camera_dma_capture(&s,10000)==-1);
        assert(a733_camera_dma_take(&s,&slot,&sequence)==-1);
    }
    puts("PASS camera DMA mapping bounds, ownership, retained faults and bounded polling");return 0;
}
