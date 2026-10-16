/* SPDX-License-Identifier: GPL-2.0-only */
#include "a733_csi_receiver.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct model {
    uint32_t regs[4][256]; uint64_t now;
    unsigned calls,writes,reads,stop_calls,drain_calls,release_calls;
    int pending_admit,pending_stop,pending_drain,pending_release;
    int fail_write,fail_read,drop_write,late_write,fail_stop,fail_drain,fail_release;
    int sensor_quiet,drained;
};
static int now(void *p,uint64_t *n) { *n=((struct model *)p)->now;return 1; }
static int admit(void *p,const struct a733_csi_config *c,uint64_t d,uint64_t *t) {
    struct model *m=p;assert(c->port<=1 && c->parser==c->port && c->top_hz==324000000);
    assert(m->now<d);++m->calls;*t=73;
    if(m->pending_admit){--m->pending_admit;return 0;}return 1;
}
static int read_reg(void *p,uint64_t t,enum a733_csi_region r,uint16_t off,uint32_t *v,uint64_t d) {
    struct model *m=p;assert(t==73 && r<=A733_CSI_PARSER && off<1024 && !(off&3));
    assert(m->now<d);++m->calls;++m->reads;*v=m->regs[r][off/4];return m->fail_read?-1:1;
}
static int write_reg(void *p,uint64_t t,enum a733_csi_region r,uint16_t off,uint32_t v,uint64_t d) {
    struct model *m=p;assert(t==73 && r>=A733_CSI_PHY && r<=A733_CSI_PARSER);
    assert(off<1024 && !(off&3) && m->now<d);++m->calls;++m->writes;
    if(m->stop_calls && !m->sensor_quiet)assert(!"disable before producer stop");
    if(!m->drop_write){
        if((r==A733_CSI_PORT && off==0x118)||(r==A733_CSI_PARSER && off==0x44))m->regs[r][off/4]&=~v;
        else m->regs[r][off/4]=v;
    }
    if(m->late_write)m->now=d;
    return m->fail_write?-1:1;
}
static int stop(void *p,uint64_t t,uint64_t d) {
    struct model *m=p;assert(t==73 && m->now<d);++m->calls;++m->stop_calls;
    if(m->pending_stop){--m->pending_stop;return 0;}
    if(m->fail_stop)return -1;m->sensor_quiet=1;return 1;
}
static int drain(void *p,uint64_t t,uint64_t d) {
    struct model *m=p;assert(t==73 && m->now<d && m->sensor_quiet);
    assert(!(m->regs[A733_CSI_PORT][0]&1) && !(m->regs[A733_CSI_PHY][0]&1));
    assert(!(m->regs[A733_CSI_PARSER][0]&0x80008001));
    assert(!(m->regs[A733_CSI_PARSER][3]&0x03030303));
    ++m->calls;++m->drain_calls;
    if(m->pending_drain){--m->pending_drain;return 0;}
    if(m->fail_drain)return -1;m->drained=1;return 1;
}
static int release(void *p,uint64_t t,uint64_t d) {
    struct model *m=p;assert(t==73 && m->now<d && m->drained);++m->calls;++m->release_calls;
    if(m->pending_release){--m->pending_release;return 0;}return m->fail_release?-1:1;
}
static const struct a733_csi_ops ops={now,admit,read_reg,write_reg,stop,drain,release};
static void setup(struct a733_csi_receiver *s,struct model *m,unsigned port) {
    memset(s,0,sizeof(*s));memset(m,0,sizeof(*m));m->now=1;
    m->regs[A733_CSI_TOP][0]=0xb0f;m->regs[A733_CSI_TOP][1]=0xdeadbeef;
    m->regs[A733_CSI_PHY][0]=0x2000;m->regs[A733_CSI_PORT][0]=0x100000;
    m->regs[A733_CSI_PARSER][0]=0x20;
    struct a733_csi_config cfg={(uint8_t)port,(uint8_t)port,port?255:42,port?7:2,324000000};
    assert(a733_csi_init(s,&ops,m,&cfg)==1);assert(a733_csi_open(s,100000)==1);
}
static int step(struct a733_csi_receiver *s,struct model *m) {
    unsigned n=m->calls;int r=a733_csi_poll(s);assert(m->calls-n<=1);++m->now;return r;
}
static int complete(struct a733_csi_receiver *s,struct model *m) {
    for(unsigned i=0;i<1000;i++){int r=step(s,m);if(r)return r;}assert(!"unbounded");return -1;
}
static void armed(struct a733_csi_receiver *s,struct model *m) {
    assert(complete(s,m)==1 && s->state==A733_CSI_ARMED && s->token==73);
    assert(m->regs[A733_CSI_PORT][0]&1);assert(m->regs[A733_CSI_PARSER][3]==2);
    assert(m->regs[A733_CSI_PARSER][0x28/4]==(640u<<16));
    assert(m->regs[A733_CSI_PARSER][0x2c/4]==(480u<<16));
    assert(m->regs[A733_CSI_TOP][0]==0xb0f && m->regs[A733_CSI_TOP][1]==0xdeadbeef);
}
static void faulted(struct a733_csi_receiver *s,struct model *m) {
    assert(s->state==A733_CSI_FAULT && s->token==73);unsigned n=m->calls;
    assert(a733_csi_poll(s)==-1);assert(a733_csi_close(s,m->now+1000)==-1);assert(m->calls==n);
}
int main(int argc,char **argv) {
    struct a733_csi_receiver s;struct model m;FILE *f=NULL;
    if(argc==2){f=fopen(argv[1],"wb");assert(f);}
    for(unsigned port=0;port<2;port++) {
        setup(&s,&m,port);m.pending_admit=2;armed(&s,&m);
        if(f)assert(fwrite(m.regs,1,sizeof(m.regs),f)==sizeof(m.regs));
        m.regs[A733_CSI_PORT][0x118/4]=4;
        assert(a733_csi_sample(&s,m.now+1000)==1);assert(step(&s,&m)==0);
        m.regs[A733_CSI_PORT][0x118/4]|=8;assert(complete(&s,&m)==1);
        assert(s.mipi_events==4 && m.regs[A733_CSI_PORT][0x118/4]==8);
        m.regs[A733_CSI_PARSER][0x44/4]=3;
        assert(a733_csi_sample(&s,m.now+1000)==1 && complete(&s,&m)==1);
        assert(s.mipi_events==12 && s.parser_events==3);
        unsigned n=m.writes;
        assert(a733_csi_close(&s,m.now+10000)==1);m.pending_stop=2;
        assert(step(&s,&m)==0 && step(&s,&m)==0 && m.writes==n);
        m.pending_drain=2;m.pending_release=2;
        assert(complete(&s,&m)==1 && s.state==A733_CSI_CLOSED && !s.token);
        assert(m.drain_calls==3 && m.release_calls==3);
        assert(m.regs[A733_CSI_TOP][0]==0xb0f && m.regs[A733_CSI_TOP][1]==0xdeadbeef);
    }
    if(f)assert(!fclose(f));
    setup(&s,&m,0);m.regs[A733_CSI_PORT][0]|=1;
    assert(complete(&s,&m)==-1 && !m.writes);faulted(&s,&m);
    setup(&s,&m,0);m.regs[A733_CSI_TOP][0]^=1;
    assert(complete(&s,&m)==-1 && !m.writes);faulted(&s,&m);
    setup(&s,&m,0);m.fail_read=1;assert(complete(&s,&m)==-1 && !m.writes);faulted(&s,&m);
    setup(&s,&m,0);m.drop_write=1;assert(complete(&s,&m)==-1);faulted(&s,&m);
    setup(&s,&m,0);m.fail_write=1;assert(complete(&s,&m)==-1 && m.writes==1);faulted(&s,&m);
    setup(&s,&m,0);m.late_write=1;assert(complete(&s,&m)==-1 && m.writes==1);faulted(&s,&m);
    setup(&s,&m,0);assert(step(&s,&m)==0);m.now=0;assert(a733_csi_poll(&s)==-1);faulted(&s,&m);
    setup(&s,&m,0);m.pending_admit=2;assert(step(&s,&m)==0);m.now=s.deadline;
    assert(a733_csi_poll(&s)==-1 && !m.writes);faulted(&s,&m);
    setup(&s,&m,0);armed(&s,&m);m.regs[A733_CSI_PORT][0x118/4]=0x800;
    assert(a733_csi_sample(&s,m.now+1000)==1 && complete(&s,&m)==-1);
    assert(s.mipi_events==0x800 && m.regs[A733_CSI_PORT][0x118/4]==0x800);faulted(&s,&m);
    setup(&s,&m,0);armed(&s,&m);m.regs[A733_CSI_PARSER][0x44/4]=4;
    assert(a733_csi_sample(&s,m.now+1000)==1 && complete(&s,&m)==-1);
    assert(s.parser_events==4);faulted(&s,&m);
    for(unsigned which=0;which<3;which++) {
        setup(&s,&m,0);armed(&s,&m);unsigned n=m.writes;
        if(which==0)m.fail_stop=1;if(which==1)m.fail_drain=1;if(which==2)m.fail_release=1;
        assert(a733_csi_close(&s,m.now+10000)==1 && complete(&s,&m)==-1);
        if(which==0)assert(m.writes==n && !m.drain_calls && !m.release_calls);
        if(which==1)assert(!m.release_calls);faulted(&s,&m);
    }
    memset(&s,0,sizeof(s));struct a733_csi_receiver saved=s;
    struct a733_csi_config bad={0,0,0,2,324000000};
    assert(a733_csi_init(&s,&ops,&m,&bad)==-1 && !memcmp(&s,&saved,sizeof(s)));
    bad.settle=1;bad.parser=1;assert(a733_csi_init(&s,&ops,&m,&bad)==-1);
    puts("PASS CSI receiver: bounded setup, packet status, producer-before-disable, drain-before-release and retained faults");return 0;
}
