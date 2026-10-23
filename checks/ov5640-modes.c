/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Program every listed OV5640 mode through the native driver, with single
 * and block writes, and write each 64 KiB register image at READY in order.
 * A stream start and stop then follow with the selected mode.
 * Arguments: output file, then size:format:vtotal triples. */
#include "ov5640.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct model {
    unsigned char reg[65536], ready[65536];
    uint64_t now;
    unsigned writes, transactions, logged;
    const struct ov5640_mode *expected;
    struct { char kind; uint16_t reg; uint8_t value; } log[1024];
};
static int now(void *p,uint64_t *n) { *n=((struct model *)p)->now; return 1; }
static int acquire(void *p,uint64_t d,uint64_t *t) { (void)p;(void)d; *t=71; return 1; }
static int line(void *p,uint64_t t,enum ov5640_line l,int a,uint64_t d) {
    (void)p;(void)l;(void)d; assert(t==71 && !a); return 1;
}
static int same(const struct ov5640_mode *a,const struct ov5640_mode *b) {
    return a->xclk_hz==b->xclk_hz && a->link_hz==b->link_hz && a->pixel_hz==b->pixel_hz &&
        a->width==b->width && a->height==b->height && a->htotal==b->htotal &&
        a->vtotal==b->vtotal && a->lanes==b->lanes && a->virtual_channel==b->virtual_channel &&
        a->csi_data_type==b->csi_data_type && a->bytes_per_pixel==b->bytes_per_pixel &&
        a->size==b->size && a->format==b->format;
}
static void note(struct model *m,char kind,uint16_t r,uint8_t v) {
    assert(m->logged<1024); m->log[m->logged].kind=kind; m->log[m->logged].reg=r;
    m->log[m->logged++].value=v;
}
static int rd(void *p,uint64_t t,uint8_t a,uint16_t r,uint8_t *v,uint64_t d) {
    struct model *m=p; (void)d; assert(t==71 && a==0x3c); *v=m->reg[r]; note(m,'r',r,0); return 1;
}
static int wr(void *p,uint64_t t,uint8_t a,uint16_t r,uint8_t v,uint64_t d) {
    struct model *m=p; (void)d; assert(t==71 && a==0x3c);
    m->reg[r]=v; ++m->writes; ++m->transactions; note(m,'w',r,v); return 1;
}
static int block(void *p,uint64_t t,uint8_t a,uint16_t r,const uint8_t *v,unsigned n,uint64_t d) {
    struct model *m=p; (void)d; assert(t==71 && a==0x3c && n>=2 && n<=OV5640_BLOCK_BYTES);
    ++m->transactions;
    for(unsigned i=0;i<n;i++) { m->reg[r+i]=v[i]; ++m->writes; note(m,'w',(uint16_t)(r+i),v[i]); }
    return 1;
}
/* The receiver sees exactly the selected mode. */
static int receiver(void *p,uint64_t t,uint64_t rt,const struct ov5640_mode *mode,uint64_t d) {
    struct model *m=p; (void)d; assert(t==71 && rt==97);
    assert(same(mode,m->expected)); return 1;
}
static int release(void *p,uint64_t t,uint64_t d) { (void)p;(void)t;(void)d; return 1; }
static void run(struct model *m,const struct ov5640_mode *mode,int blocks) {
    struct ov5640_ops o={now,acquire,line,rd,wr,receiver,release,blocks?block:NULL};
    struct ov5640 s; memset(&s,0,sizeof(s));
    memset(m,0,sizeof(*m)); memset(m->reg,0x80,sizeof(m->reg));
    m->reg[0x300a]=0x56; m->reg[0x300b]=0x40; m->now=1; m->expected=mode;
    assert(ov5640_init(&s,&o,m,1)==1 && ov5640_select(&s,mode)==1);
    assert(ov5640_open(&s,100000000)==1);
    assert(ov5640_select(&s,mode)==-1);
    for(int r=0;!r;) {
        r=ov5640_poll(&s); assert(r>=0);
        if(s.wait_until>m->now) m->now=s.wait_until; else ++m->now;
    }
    assert(s.state==OV5640_READY && same(&s.mode,mode));
    memcpy(m->ready,m->reg,sizeof(m->ready));
    assert(ov5640_start(&s,97,m->now+100000)==1);
    for(int r=0;!r;) { r=ov5640_poll(&s); assert(r>=0); ++m->now; }
    assert(s.state==OV5640_STREAMING);
    assert(ov5640_stop(&s,m->now+100000)==1);
    for(int r=0;!r;) { r=ov5640_poll(&s); assert(r>=0); ++m->now; }
}
static void refusals(void) {
    struct ov5640_mode mode,copy; struct ov5640 s;
    struct ov5640_ops o={now,acquire,line,rd,wr,receiver,release,NULL};
    struct model *m=calloc(1,sizeof(*m)); assert(m);
    memset(&mode,0x5a,sizeof(mode)); copy=mode;
    assert(ov5640_mode_get(&mode,OV5640_SIZES,OV5640_UYVY,0)==-1);
    assert(ov5640_mode_get(&mode,OV5640_VGA,OV5640_FORMATS,0)==-1);
    assert(ov5640_mode_get(&mode,OV5640_VGA,OV5640_UYVY,480+23)==-1);
    assert(ov5640_mode_get(&mode,OV5640_VGA,OV5640_UYVY,3376)==-1);
    assert(ov5640_mode_get(&mode,OV5640_QSXGA,OV5640_UYVY,1944+23)==-1);
    assert(ov5640_mode_get(NULL,OV5640_VGA,OV5640_UYVY,0)==-1);
    assert(!memcmp(&mode,&copy,sizeof(mode)));
    /* The default mode is the existing VGA profile. */
    assert(ov5640_mode_get(&mode,OV5640_VGA,OV5640_UYVY,0)==1);
    assert(same(&mode,&ov5640_vga_mode));
    memset(&s,0,sizeof(s)); assert(ov5640_select(&s,&mode)==-1);
    assert(ov5640_init(&s,&o,m,0)==1);
    assert(same(&s.mode,&ov5640_vga_mode));
    /* A mode whose derived values were changed is refused. */
    assert(ov5640_mode_get(&mode,OV5640_720P,OV5640_UYVY,0)==1);
    copy=mode; copy.pixel_hz+=1; assert(ov5640_select(&s,&copy)==-1);
    copy=mode; copy.width=1290; assert(ov5640_select(&s,&copy)==-1);
    copy=mode; copy.csi_data_type=0x2a; assert(ov5640_select(&s,&copy)==-1);
    copy=mode; copy.vtotal=100; assert(ov5640_select(&s,&copy)==-1);
    assert(same(&s.mode,&ov5640_vga_mode));
    assert(ov5640_select(&s,&mode)==1 && s.mode.width==1280);
    free(m);
}
int main(int argc,char **argv) {
    assert(argc>=3); refusals();
    FILE *f=fopen(argv[1],"wb"); assert(f);
    struct model *a=malloc(sizeof(*a)),*b=malloc(sizeof(*b)); assert(a && b);
    for(int i=2;i<argc;i++) {
        unsigned size,format,vtotal; struct ov5640_mode mode;
        assert(sscanf(argv[i],"%u:%u:%u",&size,&format,&vtotal)==3);
        assert(ov5640_mode_get(&mode,(enum ov5640_size)size,(enum ov5640_format)format,vtotal)==1);
        run(a,&mode,0); run(b,&mode,1);
        /* Block writes reproduce the single-write access sequence. */
        assert(!memcmp(a->ready,b->ready,65536) && a->logged==b->logged && b->transactions<a->transactions);
        for(unsigned k=0;k<a->logged;k++)
            assert(a->log[k].kind==b->log[k].kind && a->log[k].reg==b->log[k].reg &&
                   a->log[k].value==b->log[k].value);
        printf("%u:%u:%u link=%u pixel=%u %ux%u htotal=%u vtotal=%u dt=0x%02x bpp=%u writes=%u/%u\n",
               size,format,vtotal,mode.link_hz,mode.pixel_hz,mode.width,mode.height,mode.htotal,
               mode.vtotal,mode.csi_data_type,mode.bytes_per_pixel,a->transactions,b->transactions);
        assert(fwrite(a->ready,1,65536,f)==65536);
    }
    assert(!fclose(f)); free(a); free(b);
    puts("PASS OV5640 modes: refusals, selected-mode receiver handoff and block-write equivalence");
    return 0;
}
