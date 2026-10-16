/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "a733_video_jpeg_run.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct write { uint32_t off, width, value; };
struct model {
    uint64_t now;
    unsigned fault, reads, writes, finish, prepared, triggered;
    struct write trace[700];
};
static int now(void *p,uint64_t *v) { struct model*m=p; *v=++m->now; return 1; }
static int acquire(void *p,const struct a733_video_jpeg_job*j,uint64_t d,uint64_t*t)
{ (void)p;(void)j;(void)d;*t=123;return 1; }
static int prepare(void*p,uint64_t t,const struct a733_video_jpeg_job*j,
 const struct a733_video_jpeg*f,uint32_t stride,uint32_t rows,uint64_t d)
{
    struct model*m=p; (void)j;(void)d;
    assert(t==123 && f->width==48 && f->height==32 && stride==64 && rows==32);
    ++m->prepared; return m->fault==1 ? -1 : 1;
}
static int read_reg(void*p,uint64_t t,uint32_t o,uint32_t*v,uint64_t d)
{
    struct model*m=p;(void)d;assert(t==123);
    assert(m->prepared || o==0xf0 || o==0xe0 || o==0xe4);++m->reads;*v=0;
    if(o==0xe0)*v=m->fault==2?0xdead:0x33321;
    if(o==0xe4)*v=0x21321;
    if(o==0x11c) {
        if(!m->triggered && m->fault==3)*v=1;
        else if(m->triggered)*v=m->fault==4?3:1;
    }
    if(m->fault==8)return 2;
    return 1;
}
static int write_reg(void*p,uint64_t t,uint32_t o,unsigned w,uint32_t v,uint64_t d)
{
    struct model*m=p;assert(t==123 && m->writes<700);
    m->trace[m->writes++]=(struct write){o,w,v};
    if(o==0x118) { assert(w==1 && v==14);m->triggered=1;if(m->fault==5)m->now=d; }
    if(m->fault==6 && o==0x1e4)return -1;
    return 1;
}
static int finish(void*p,uint64_t t,uint64_t d)
{
    struct model*m=p;(void)d;assert(t==123 && m->triggered);
    assert(m->trace[m->writes-1].off==0x11c);
    if(m->fault==7)return -1;
    return ++m->finish>1?1:0;
}
int main(int argc,char**argv)
{
    assert(argc==3);
    FILE*f=fopen(argv[1],"rb");assert(f);
    uint8_t *data=calloc(1,65536);assert(data);
    size_t size=fread(data,1,65536,f);assert(!ferror(f) && size && size<65536);fclose(f);
    struct a733_video_jpeg_ops ops={now,acquire,prepare,read_reg,write_reg,finish};
    struct a733_video_jpeg_job job={data,(uint32_t)size,0x81000000,65536,0x82000000,2048,0x83000000,1024};
    for(unsigned fail=0;fail<=8;++fail) {
        struct model m={.fault=fail};struct a733_video_jpeg_run r={0};
        assert(a733_video_jpeg_begin(&r,&ops,&m,&job,10000));
        int result=0;unsigned polls=0;
        while(result==0 && ++polls<1000)result=a733_video_jpeg_poll(&r);
        assert(polls<1000 && r.token==123);
        if(!fail) {
            assert(result==1 && r.state==A733_VIDEO_JPEG_DONE && m.finish==2 && m.writes==661);
            f=fopen(argv[2],"wb");assert(f);
            assert(fwrite(m.trace,sizeof(m.trace[0]),m.writes,f)==m.writes);fclose(f);
        } else {
            assert(result==-1 && r.state==A733_VIDEO_JPEG_FAULT);
            if(fail<=3 || fail==8)assert(!m.writes);
            if(fail==2 || fail==8)assert(!m.prepared);
            if(fail==4 || fail==5)assert(m.triggered && !m.finish);
            if(fail==6)assert(!m.triggered && !m.finish);
            unsigned accesses=m.reads+m.writes;
            assert(a733_video_jpeg_poll(&r)==-1 && m.reads+m.writes==accesses);
        }
    }
    struct a733_video_jpeg_run r={0};struct model m={0};
    struct a733_video_jpeg_job bad=job;bad.chroma_iova=bad.luma_iova;
    assert(!a733_video_jpeg_begin(&r,&ops,&m,&bad,10000));
    bad=job;bad.input_iova=UINT64_C(0x100000000);
    assert(!a733_video_jpeg_begin(&r,&ops,&m,&bad,10000));
    assert(a733_video_jpeg_begin(&r,&ops,&m,&job,10000));
    assert(a733_video_jpeg_poll(&r)==0);m.now=0;
    assert(a733_video_jpeg_poll(&r)==-1 && r.token==123);
    free(data);
    puts("PASS: bounded JPEG executor model; success waits for drain/cache callback, failures retain ownership. No hardware execution.");
}
