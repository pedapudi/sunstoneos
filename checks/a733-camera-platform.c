/* SPDX-License-Identifier: GPL-2.0-only */
#include "a733_camera_platform.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct model {
    uint32_t regs[0x1900/4],trace[16],proof;
    unsigned operations,reads,writes,admissions,pending,delay,cal_reads;
    unsigned fail_operation,drop_write,late_operation,regress_operation;
    unsigned change_token,stuck,corrupt_control,corrupt_final;
    uint64_t now;
    enum a733_camera_origin origin;
};
static int now(void *p,uint64_t *n){struct model *m=p;*n=m->now++;return 1;}
static int operation(struct model *m){
    ++m->operations;
    if(m->late_operation==m->operations)m->now=10000;
    if(m->regress_operation==m->operations)m->now=0;
    return m->fail_operation!=m->operations;
}
static int admit(void *p,uint64_t d,struct a733_camera_admission *a){
    struct model *m=p;assert(d==10000);++m->admissions;
    a->token=(m->change_token && m->admissions>1)?43:42;
    a->proof=m->proof;a->origin=m->origin;
    if(!operation(m))return -1;
    return m->admissions>m->pending;
}
static int read_reg(void *p,uint64_t token,uint16_t off,uint32_t *v,uint64_t d){
    struct model *m=p;assert(token==42 && d==10000 && !(off&3));
    assert(off==0 || off==4 || off==0x100 || off==0x200 || off==0x300 ||
           off==0x1000 || off==0x1400 || off==0x1800);
    ++m->reads;if(!operation(m))return -1;
    if(off==4 && (m->regs[1]&2) && (m->regs[0]&15)==15){
        ++m->cal_reads;
        if(!m->stuck && m->cal_reads>m->delay){
            m->regs[1]=(m->regs[1]&0x1f7)|8|(((m->regs[1]>>4)&31)<<16);
        }
        if(m->corrupt_control)m->regs[1]^=1;
    }
    if(off==0 && m->corrupt_final && m->cal_reads)m->regs[0]^=0x100;
    *v=m->regs[off/4];return 1;
}
static int write_reg(void *p,uint64_t token,uint16_t off,uint32_t v,uint64_t d){
    struct model *m=p;assert(token==42 && d==10000 && (off==0 || off==4));
    ++m->writes;assert(m->writes<=6);
    m->trace[m->writes-1]=v;
    if(off==4){assert(!(v&~0x1f7u));assert(!(v&1));assert((v&4)==(m->regs[1]&4));}
    else assert((v&~0x67001f0fu)==(m->regs[0]&~0x67001f0fu));
    if(!operation(m))return -1;
    if(m->drop_write!=m->writes){
        if(off==4)m->regs[1]=(m->regs[1]&~0x1f7u)|v;
        else m->regs[0]=v;
    }
    return 1;
}
static const struct a733_camera_platform_ops ops={now,admit,read_reg,write_reg};
static void prepare(struct model *m,struct a733_camera_platform *s){
    memset(m,0,sizeof(*m));memset(s,0,sizeof(*s));
    m->now=1;m->proof=A733_CAMERA_REQUIRED_PROOF;m->origin=A733_CAMERA_COLD_RESET;
    m->regs[0]=0x360500;m->regs[1]=0xa5;
    for(unsigned i=1;i<=3;i++)m->regs[i*0x100/4]=0x40000;
    assert(a733_camera_platform_init(s,&ops,m)==1);
    assert(a733_camera_platform_open(s,10000)==1);
}
static int run(struct model *m,struct a733_camera_platform *s){
    int r=0;
    for(unsigned i=0;i<11000 && !r;i++){
        unsigned n=m->operations;r=a733_camera_platform_poll(s);assert(m->operations-n<=1);
    }
    assert(r);
    unsigned n=m->operations;uint64_t time=m->now;
    assert(a733_camera_platform_poll(s)==r);
    assert(m->operations==n && m->now==time && s->admission.token==42);
    assert(a733_camera_platform_open(s,20000)==-1);
    return r;
}
static void assert_fault(struct model *m,struct a733_camera_platform *s){
    assert(run(m,s)==-1);assert(s->state==A733_CAMERA_PLATFORM_FAULT);
}
int main(int argc,char **argv){
    assert(argc==2);struct model m;struct a733_camera_platform s;
    prepare(&m,&s);assert(run(&m,&s)==1);
    assert(m.writes==6 && m.regs[0]==0x360b0f && m.regs[1]==0x10010e);
    assert(s.observed[0]==0x360500 && s.observed[1]==0xa5);
    assert(s.calibration==0x10010e && s.admission.origin==A733_CAMERA_COLD_RESET);
    unsigned normal_operations=m.operations;
    /* Final TOP plus the four power/reset writes compare with unmodified
     * source setters. Calibration has its separate manual-defined model.
     */
    uint32_t source_image[5]={m.regs[0],m.trace[2],m.trace[3],m.trace[4],m.trace[5]};
    FILE *f=fopen(argv[1],"wb");assert(f);
    assert(fwrite(source_image,sizeof(source_image),1,f)==1);assert(!fclose(f));
    prepare(&m,&s);m.pending=3;m.delay=5;m.origin=A733_CAMERA_RESOLVED_RESET_HANDOFF;
    assert(run(&m,&s)==1 && m.admissions==4 && m.cal_reads==6);
    /* Preserve automatic-mode RESETN, even when clear; software ignores it. */
    prepare(&m,&s);m.regs[1]=0x1f00a1;assert(run(&m,&s)==1);assert(m.regs[1]==0x10010a);
    for(unsigned bit=0;bit<10;bit++){
        prepare(&m,&s);m.proof&=~(1u<<bit);assert_fault(&m,&s);assert(!m.reads && !m.writes);
    }
    prepare(&m,&s);m.proof|=0x400;assert_fault(&m,&s);assert(!m.reads);
    prepare(&m,&s);m.origin=A733_CAMERA_ORIGIN_UNKNOWN;assert_fault(&m,&s);assert(!m.reads);
    prepare(&m,&s);m.pending=1;m.change_token=1;assert_fault(&m,&s);assert(!m.reads);
    const uint16_t active_offsets[]={0,0x100,0x200,0x300,0x1000,0x1400,0x1800};
    for(unsigned i=0;i<sizeof(active_offsets)/sizeof(active_offsets[0]);i++){
        prepare(&m,&s);m.regs[active_offsets[i]/4]|=1;assert_fault(&m,&s);assert(!m.writes);
    }
    const uint32_t top_busy[]={2,4,8,0x1000,0x1000000,0x20000000};
    for(unsigned i=0;i<sizeof(top_busy)/sizeof(top_busy[0]);i++){
        prepare(&m,&s);m.regs[0]|=top_busy[i];assert_fault(&m,&s);assert(!m.writes);
    }
    for(unsigned i=1;i<=3;i++){
        prepare(&m,&s);m.regs[i*0x100/4]|=0x040001f0;assert_fault(&m,&s);assert(!m.writes);
    }
    /* Every callback boundary fails once, including admission and final read.
     * This sweep covers partial writes without rerunning lower-level drivers.
     */
    for(unsigned i=1;i<=normal_operations;i++){
        prepare(&m,&s);m.fail_operation=i;assert_fault(&m,&s);
        prepare(&m,&s);m.late_operation=i;assert_fault(&m,&s);
        prepare(&m,&s);m.regress_operation=i;assert_fault(&m,&s);
    }
    for(unsigned i=1;i<=6;i++){
        prepare(&m,&s);m.drop_write=i;assert_fault(&m,&s);
    }
    prepare(&m,&s);m.stuck=1;assert_fault(&m,&s);assert(m.writes==6);
    prepare(&m,&s);m.corrupt_control=1;assert_fault(&m,&s);
    prepare(&m,&s);m.corrupt_final=1;assert_fault(&m,&s);
    memset(&s,0,sizeof(s));assert(a733_camera_platform_init(&s,0,&m)==-1);
    assert(a733_camera_platform_init(&s,&ops,&s)==-1);
    struct a733_camera_platform_ops bad=ops;bad.write=0;
    assert(a733_camera_platform_init(&s,&bad,&m)==-1);
    assert(a733_camera_platform_init(&s,&ops,&m)==1);
    assert(a733_camera_platform_open(&s,0)==-1);
    assert(a733_camera_platform_poll(&s)==-1);
    assert(a733_camera_platform_init(&s,&ops,&m)==-1);
    puts("PASS: cold analog ownership, manual calibration semantics and bounded faults");
    return 0;
}
