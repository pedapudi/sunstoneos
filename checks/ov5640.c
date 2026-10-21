/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ov5640.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct model {
    unsigned char reg[65536];
    uint64_t now, acquired, pwdn, reset, delay_start;
    unsigned calls, writes, reads, line_calls, receiver_calls, release_calls;
    int acquire_pending, receiver_pending, release_pending;
    int fail_write, fail_line, late_write, wrong_read, fail_release, change_token;
    int receiver_ready, fault_time, pwdn_only, fail_block, late_block;
    unsigned transactions, blocks, logged;
    /* Every register access in order; a block write logs each register. */
    struct { char kind; uint16_t reg; uint8_t value, masked, delayed; } log[512];
};
static int now(void *p, uint64_t *n) {
    struct model *m=p; if(m->fault_time) return -1; *n=m->now; return 1;
}
static int acquire(void *p,uint64_t d,uint64_t *token) {
    struct model *m=p; assert(m->now<d); ++m->calls;
    if(!*token) { *token=71; m->acquired=m->now; }
    if(m->change_token) *token=72;
    if(m->acquire_pending) { --m->acquire_pending; return 0; } return 1;
}
static int line(void *p,uint64_t token,enum ov5640_line l,int asserted,uint64_t d) {
    struct model *m=p; assert(token==71 && !asserted && m->now<d);
    ++m->calls; ++m->line_calls;
    if(l==OV5640_PWDN_LINE) { assert(m->now>=m->acquired+5000); m->pwdn=m->now; }
    else { assert(!m->pwdn_only && m->pwdn && m->now>=m->pwdn+1000); m->reset=m->now; }
    return m->fail_line ? -1 : 1;
}
/* Without a RESET line, PWDN release still precedes the bus by 1 ms + 20 ms. */
static int bus_ready(const struct model *m) {
    return m->pwdn_only ? m->pwdn && !m->reset && m->now>=m->pwdn+21000 :
        m->reset && m->now>=m->reset+20000;
}
static int read_reg(void *p,uint64_t token,uint8_t a,uint16_t r,uint8_t *v,uint64_t d) {
    struct model *m=p; assert(token==71 && a==0x3c && m->now<d);
    assert(bus_ready(m)); ++m->calls; ++m->reads;
    *v=m->reg[r]; if(m->wrong_read && r==0x3036) *v^=1;
    if(m->logged<512) { m->log[m->logged].kind='r'; m->log[m->logged++].reg=r; }
    return 1;
}
static void store(struct model *m,char kind,uint16_t r,uint8_t v) {
    if(m->delay_start) { assert(m->now>=m->delay_start+300000); m->delay_start=0; }
    if(r==0x3c00 && m->writes<300) m->delay_start=m->now;
    if(r==0x4202 && v==0) assert(m->receiver_ready);
    ++m->writes; m->reg[r]=v;
    if(m->logged<512) {
        int masked=m->logged && m->log[m->logged-1].kind=='r' && m->log[m->logged-1].reg==r;
        m->log[m->logged].kind=kind; m->log[m->logged].reg=r;
        m->log[m->logged].value=v; m->log[m->logged++].masked=(uint8_t)masked;
    }
}
static int write_reg(void *p,uint64_t token,uint8_t a,uint16_t r,uint8_t v,uint64_t d) {
    struct model *m=p; assert(token==71 && a==0x3c && m->now<d);
    assert(bus_ready(m));
    ++m->calls; ++m->transactions; store(m,'w',r,v);
    if(m->late_write) m->now=d;
    return m->fail_write ? -1 : 1;
}
static int write_block(void *p,uint64_t token,uint8_t a,uint16_t r,const uint8_t *v,
                       unsigned n,uint64_t d) {
    struct model *m=p; assert(token==71 && a==0x3c && m->now<d && v);
    assert(bus_ready(m) && n>=2 && n<=OV5640_BLOCK_BYTES && r+n<=65536);
    ++m->calls; ++m->transactions; ++m->blocks;
    for(unsigned i=0;i<n;i++) store(m,'b',(uint16_t)(r+i),v[i]);
    if(m->late_write || m->late_block) m->now=d;
    return m->fail_block ? -1 : 1;
}
static int receiver(void *p,uint64_t token,uint64_t receiver_token,
                    const struct ov5640_mode *mode,uint64_t d) {
    struct model *m=p; assert(token==71 && receiver_token==97 && m->now<d);
    assert(mode->width==640 && mode->height==480 && mode->lanes==2);
    assert(mode->link_hz==192000000 && mode->xclk_hz==24000000);
    assert(mode->pixel_hz==48000000 && mode->htotal==1600 && mode->vtotal==1000);
    assert(mode->csi_data_type==0x1e && mode->virtual_channel==0);
    ++m->calls; ++m->receiver_calls;
    if(m->receiver_pending) { --m->receiver_pending; return 0; }
    m->receiver_ready=1; return 1;
}
static int release(void *p,uint64_t token,uint64_t d) {
    struct model *m=p; assert(token==71 && m->now<d && m->reg[0x4202]==15);
    ++m->calls; ++m->release_calls;
    if(m->release_pending) { --m->release_pending; return 0; }
    return m->fail_release ? -1 : 1;
}
static const struct ov5640_ops ops={now,acquire,line,read_reg,write_reg,receiver,release,write_block};
static const struct ov5640_ops single={now,acquire,line,read_reg,write_reg,receiver,release,NULL};
static const struct ov5640_ops *selected=&ops;
static void setup_profile(struct ov5640 *s,struct model *m,int pattern,int pwdn_only) {
    memset(s,0,sizeof(*s)); memset(m,0,sizeof(*m)); memset(m->reg,0x80,sizeof(m->reg));
    m->reg[0x300a]=0x56; m->reg[0x300b]=0x40; m->now=1; m->pwdn_only=pwdn_only;
    assert(ov5640_pwdn_only(s)==-1);
    assert(ov5640_init(s,selected,m,pattern)==1);
    if(pwdn_only) assert(ov5640_pwdn_only(s)==1);
    assert(ov5640_open(s,2000000)==1);
    if(!pwdn_only) assert(ov5640_pwdn_only(s)==-1 && !s->pwdn_only);
}
static void setup(struct ov5640 *s,struct model *m,int pattern) { setup_profile(s,m,pattern,0); }
static int step(struct ov5640 *s,struct model *m) {
    unsigned n=m->calls,w=m->writes; int r=ov5640_poll(s); assert(m->calls-n<=1);
    /* Record which register write started a mandatory configuration delay. */
    if(m->writes!=w && s->wait_until>m->now && m->logged) m->log[m->logged-1].delayed=1;
    if(s->wait_until>m->now) m->now=s->wait_until; else ++m->now;
    return r;
}
static int complete(struct ov5640 *s,struct model *m) {
    for(unsigned i=0;i<2000;i++) { int r=step(s,m); if(r) return r; }
    assert(!"unbounded state machine"); return -1;
}
static void quiet_fault(struct ov5640 *s,struct model *m) {
    assert(s->state==OV5640_FAULT); unsigned n=m->calls;
    assert(ov5640_poll(s)==-1); assert(ov5640_close(s,m->now+100)==-1);
    assert(ov5640_init(s,&ops,m,0)==-1); assert(m->calls==n);
}
static void ready(struct ov5640 *s,struct model *m) {
    assert(complete(s,m)==1 && s->state==OV5640_READY && s->token==71);
    assert(m->reg[0x4202]==15 && m->reg[0x300e]==0x44);
    assert(m->reg[0x3808]==2 && m->reg[0x3809]==128);
    assert(m->reg[0x380e]==3 && m->reg[0x380f]==232);
}
int main(int argc,char **argv) {
    struct ov5640 s; struct model *m=malloc(sizeof(*m)); assert(m);
    setup(&s,m,1); m->acquire_pending=2; ready(&s,m);
    assert(m->reg[0x503d]==0x84);
    if(argc==2) { FILE *f=fopen(argv[1],"wb"); assert(f);
        assert(fwrite(m->reg,1,65536,f)==65536); assert(!fclose(f)); }
    struct ov5640 saved=s; unsigned n=m->calls;
    assert(ov5640_start(&s,0,m->now+100)==-1);
    assert(!memcmp(&saved,&s,sizeof(s)) && n==m->calls);
    assert(ov5640_start(&s,97,m->now+10000)==1); m->receiver_pending=2;
    assert(complete(&s,m)==1 && s.state==OV5640_STREAMING && m->receiver_calls==3);
    assert(ov5640_close(&s,m->now+100)==-1);
    assert(ov5640_stop(&s,m->now+10000)==1 && complete(&s,m)==1);
    assert(s.receiver_token==97 && ov5640_start(&s,98,m->now+100)==-1);
    assert(ov5640_start(&s,97,m->now+10000)==1 && complete(&s,m)==1);
    assert(ov5640_stop(&s,m->now+10000)==1 && complete(&s,m)==1);
    assert(ov5640_close(&s,m->now+10000)==1); m->release_pending=2;
    assert(complete(&s,m)==1 && s.state==OV5640_CLOSED && !s.token);
    n=m->calls; assert(ov5640_poll(&s)==1 && n==m->calls);
    /* PWDN-only modules make no RESET call and program the same register image. */
    unsigned char *image=malloc(65536); assert(image);
    setup(&s,m,1); ready(&s,m); memcpy(image,m->reg,65536);
    setup_profile(&s,m,1,1); ready(&s,m);
    assert(m->line_calls==1 && !m->reset && !memcmp(image,m->reg,65536));
    /* Block writes replay the single-write sequence exactly. Only unmasked,
     * undelayed entries other than the test pattern join a block. */
    struct model *base=malloc(sizeof(*base)); assert(base);
    selected=&single; setup(&s,m,1); ready(&s,m); memcpy(base,m,sizeof(*m)); selected=&ops;
    setup(&s,m,1); ready(&s,m);
    assert(!memcmp(base->reg,m->reg,65536) && !base->blocks && base->logged==m->logged);
    for(unsigned i=0;i<m->logged;i++) {
        assert(base->log[i].reg==m->log[i].reg && (base->log[i].kind=='r')==(m->log[i].kind=='r'));
        if(m->log[i].kind!='r') assert(base->log[i].value==m->log[i].value);
        if(m->log[i].kind=='b')
            assert(!base->log[i].masked && !base->log[i].delayed && m->log[i].reg!=0x503d);
    }
    assert(base->transactions==308 && m->transactions==125 && m->blocks==36);
    printf("configuration writes: %u single transactions, %u with %u blocks\n",
           base->transactions,m->transactions,m->blocks);
    free(base); free(image);
    /* A failed or overdue block write retains the lease and stops I/O. */
    setup(&s,m,0); m->fail_block=1;
    assert(complete(&s,m)==-1 && m->blocks==1 && s.token==71); quiet_fault(&s,m);
    setup(&s,m,0); m->late_block=1;
    assert(complete(&s,m)==-1 && m->blocks==1 && s.token==71); quiet_fault(&s,m);
    setup_profile(&s,m,0,1); m->fail_line=1;
    assert(complete(&s,m)==-1 && m->line_calls==1 && !m->reads); quiet_fault(&s,m);
    /* Identify a different sensor before writing any registers. */
    setup(&s,m,0); m->reg[0x300a]=0x55;
    assert(complete(&s,m)==-1 && !m->writes && s.token==71); quiet_fault(&s,m);
    /* Uncertain GPIO and register writes retain the lease and stop I/O. */
    setup(&s,m,0); m->fail_line=1;
    assert(complete(&s,m)==-1 && s.token==71); quiet_fault(&s,m);
    setup(&s,m,0); m->fail_write=1;
    assert(complete(&s,m)==-1 && m->writes==1 && s.token==71); quiet_fault(&s,m);
    setup(&s,m,0); m->late_write=1;
    assert(complete(&s,m)==-1 && m->writes==1 && s.token==71); quiet_fault(&s,m);
    /* A sensor accepting writes but not retaining the PLL is refused. */
    setup(&s,m,0); m->wrong_read=1;
    assert(complete(&s,m)==-1 && s.token==71); quiet_fault(&s,m);
    /* Pending admission never replaces a previously retained token. */
    setup(&s,m,0); m->acquire_pending=2; assert(step(&s,m)==0);
    m->change_token=1; assert(step(&s,m)==-1 && s.token==71); quiet_fault(&s,m);
    /* Time failure and reversal prevent subsequent device operations. */
    setup(&s,m,0); assert(step(&s,m)==0); m->now=0; n=m->calls;
    assert(ov5640_poll(&s)==-1 && n==m->calls); quiet_fault(&s,m);
    setup(&s,m,0); m->fault_time=1; assert(ov5640_poll(&s)==-1 && !m->calls);
    quiet_fault(&s,m);
    /* A deadline during mandatory settling does not skip the delay. */
    setup(&s,m,0); s.deadline=100; assert(step(&s,m)==0);
    assert(step(&s,m)==-1 && !m->line_calls); quiet_fault(&s,m);
    /* Partial stream start, stop, and failed release keep receiver ownership. */
    setup(&s,m,0); ready(&s,m);
    assert(ov5640_start(&s,97,m->now+10000)==1); assert(step(&s,m)==0);
    m->fail_write=1; assert(step(&s,m)==-1 && s.receiver_token==97); quiet_fault(&s,m);
    setup(&s,m,0); ready(&s,m);
    assert(ov5640_start(&s,97,m->now+10000)==1 && complete(&s,m)==1);
    assert(ov5640_stop(&s,m->now+10000)==1); m->fail_write=1;
    assert(step(&s,m)==-1 && s.token==71 && s.receiver_token==97); quiet_fault(&s,m);
    setup(&s,m,0); ready(&s,m); m->fail_release=1;
    assert(ov5640_close(&s,m->now+10000)==1 && complete(&s,m)==-1 && s.token==71);
    quiet_fault(&s,m);
    free(m);
    puts("PASS OV5640: bounded power/configuration/stream lifecycle, delays, admission, readback and retained faults");
    return 0;
}
