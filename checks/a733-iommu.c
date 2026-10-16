/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "a733_iommu.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static _Alignas(16384) uint32_t arena[A733_IOMMU_TABLE_BYTES/4];
static uint32_t saved[A733_IOMMU_TABLE_BYTES/4];
static struct a733_iommu_span maps[] = {
    {0x10000000,0x48000000,4096,A733_IOMMU_READ},
    {0x103ff000,0xfffff000,307200,A733_IOMMU_WRITE},
    {0x10800000,0x180000000,307200,A733_IOMMU_WRITE}
};
struct model {
    uint32_t regs[0x20000/4], published[A733_IOMMU_TABLE_BYTES/4];
    uint64_t now,flush_start;
    unsigned writes,reads,publications,admissions,admit_pending,publish_pending;
    unsigned drop,late,late_read,deny,unresolved,undrained,regression,token_change;
};
static int now(void *p,uint64_t *v)
{
    struct model *m=p;*v=++m->now;
    if(m->regression && m->now>2)*v=0;
    return 1;
}
static int admit(void *p,const struct a733_iommu_tables *t,uint64_t d,
                 struct a733_iommu_admission *a)
{
    struct model *m=p;(void)t;(void)d;++m->admissions;
    *a=(struct a733_iommu_admission){.token=37,.excluded_masters=A733_IOMMU_REAL_MASTERS,
        .powered_masters=A733_IOMMU_REAL_MASTERS,.inherited_resolved=!m->unresolved,
        .transactions_drained=!m->undrained,.clocks_held=1,.recovery_qualified=1};
    if(m->token_change && m->admissions>1)a->token=38;
    if(m->deny)a->excluded_masters=2; /* CSI idle cannot authorize global writes. */
    if(m->admit_pending) { --m->admit_pending;return 0; }return 1;
}
static void offset_ok(uint32_t off)
{
    assert(!(off&3) && off<0x20000);
    unsigned local=off&0xffff;
    assert(!(local>=0xc00 && local<0x1000 && off<0x10000)); /* absent IDs4/5 */
}
static int read_reg(void *p,uint64_t token,uint32_t off,uint32_t *v,uint64_t d)
{
    struct model *m=p;assert(token==37);offset_ok(off);++m->reads;
    *v=m->regs[off/4];if(m->late_read)m->now=d;return 1;
}
static int write_reg(void *p,uint64_t token,uint32_t off,uint32_t v,uint64_t d)
{
    struct model *m=p;assert(token==37);offset_ok(off);assert(m->publications);
    assert(off!=0x10 && off!=0x10010); /* No reset, including on errors. */
    assert(off!=0x84 && off!=0x10084); /* Preserve faults. */
    ++m->writes;if(m->drop==m->writes)return 1;
    if(off==0x30 && v==3)m->flush_start=m->now;
    if(off==0x30 && v==0)assert(m->now-m->flush_start>=10);
    if((off==0x14 || off==0x10014) && v==1) {
        assert(m->regs[0x30/4]==0 && m->regs[0x10030/4]==0);
        assert(m->regs[0x604/4]==0);
    }
    m->regs[off/4]=v;if(m->late)m->now=d;return 1;
}
static int publish(void *p,uint64_t token,const void *v,size_t n,uint64_t d)
{
    struct model *m=p;(void)d;assert(token==37 && n==sizeof(arena));
    if(m->publish_pending) { --m->publish_pending;return 0; }
    memcpy(m->published,v,n);++m->publications;return 1;
}
static const struct a733_iommu_ops ops={now,admit,read_reg,write_reg,publish};
static int run(struct a733_iommu *s)
{
    for(unsigned n=0;n<300;++n) {
        struct model *m=s->context;
        unsigned accesses=m->reads+m->writes+m->publications+m->admissions;
        int r=a733_iommu_poll(s);
        assert(m->reads+m->writes+m->publications+m->admissions<=accesses+1);
        if(r)return r;
    }
    assert(!"poll failed to terminate");return -1;
}
/* Independent hardware-style two-level walk using published memory, without
 * consulting the driver's copied span metadata. Invalid pages remain guards. */
static int walk(const uint32_t *words,uint64_t table,uint64_t va,unsigned access,uint64_t *pa)
{
    uint32_t de=words[va>>22];if((de&3)!=1)return 0;
    uint64_t l2=(de&0xfffff000u)|((uint64_t)((de>>8)&3)<<32);
    assert(l2>=table+16384 && l2<table+sizeof(arena));
    uint32_t pe=words[(l2-table)/4+((va>>12)&1023)];
    if(!(pe&2) || ((access&1) && !(pe&4)) || ((access&2) && !(pe&8)))return 0;
    *pa=(pe&0xfffff000u)|((uint64_t)((pe>>8)&3)<<32)|(va&4095);return 1;
}
static void builder_checks(struct a733_iommu_tables *t)
{
    uint64_t pa=0;
    assert(a733_iommu_build(t,arena,0x140000000,maps,3)==1);
    assert(a733_iommu_validate(t)==1);
    for(unsigned j=0;j<3;++j) {
        for(uint64_t k=0;k<maps[j].bytes;k+=4096) {
            assert(walk(arena,t->physical,maps[j].iova+k,maps[j].access,&pa));
            assert(pa==maps[j].physical+k);
        }
        assert(walk(arena,t->physical,maps[j].iova+maps[j].bytes-1,maps[j].access,&pa));
        assert(pa==maps[j].physical+maps[j].bytes-1);
        assert(!walk(arena,t->physical,maps[j].iova+maps[j].bytes,maps[j].access,&pa));
        assert(!walk(arena,t->physical,maps[j].iova-1,maps[j].access,&pa));
        assert(!walk(arena,t->physical,maps[j].iova,3,&pa));
    }
    assert(a733_iommu_translate(t,maps[1].iova+4096,2,&pa)==1 && pa==0x100000000);
    assert(a733_iommu_translate(t,maps[1].iova,1,&pa)==-1);
    memcpy(saved,arena,sizeof(saved));struct a733_iommu_tables before=*t;
    struct a733_iommu_span bad[8];memcpy(bad,maps,sizeof(maps));
    bad[2].physical=maps[1].physical;
    assert(a733_iommu_build(t,arena,t->physical,bad,3)==-1);
    bad[2]=maps[2];bad[2].iova=maps[0].iova;
    assert(a733_iommu_build(t,arena,t->physical,bad,3)==-1);
    bad[2]=maps[2];bad[2].physical=t->physical;
    assert(a733_iommu_build(t,arena,t->physical,bad,3)==-1);
    bad[2]=maps[2];bad[2].bytes=UINT64_MAX;
    assert(a733_iommu_build(t,arena,t->physical,bad,3)==-1);
    bad[2]=maps[2];bad[2].iova=(UINT64_C(1)<<34)-4096;bad[2].bytes=8192;
    assert(a733_iommu_build(t,arena,t->physical,bad,3)==-1);
    bad[2]=maps[2];bad[2].physical=UINT64_C(1)<<34;
    assert(a733_iommu_build(t,arena,t->physical,bad,3)==-1);
    bad[2]=maps[2];bad[2].bytes=4095;
    assert(a733_iommu_build(t,arena,t->physical,bad,3)==-1);
    for(unsigned i=0;i<5;++i)bad[i]=(struct a733_iommu_span){0x400000u*(i+1),0x50000000u+4096*i,4096,1};
    assert(a733_iommu_build(t,arena,t->physical,bad,5)==-1);
    assert(a733_iommu_build((void *)arena,arena,t->physical,maps,3)==-1);
    assert(!memcmp(saved,arena,sizeof(saved)) && !memcmp(t,&before,sizeof(*t)));
    arena[0]=1;assert(a733_iommu_validate(t)==-1);arena[0]=0;
    assert(!a733_iommu_master(4) && !a733_iommu_master(5) && !a733_iommu_master(12));
    assert(a733_iommu_master(11)->micro_offset==0x10e00);
}
static void controller_checks(struct a733_iommu_tables *t,const char *dump)
{
    struct model *m=calloc(1,sizeof(*m));assert(m);
    struct a733_iommu s={0};m->admit_pending=1;m->publish_pending=1;
    assert(a733_iommu_init(&s,&ops,m,t)==1 && a733_iommu_open(&s,10000)==1);
    assert(run(&s)==1 && s.state==A733_IOMMU_ACTIVE && s.admission.token==37);
    assert(m->regs[0x14/4]==1 && m->regs[0x10014/4]==1);
    assert(!memcmp(arena,m->published,sizeof(arena)));
    uint64_t pa;assert(walk(m->published,t->physical,maps[1].iova+4096,2,&pa) && pa==0x100000000);
    if(dump) { FILE *f=fopen(dump,"wb");assert(f);assert(fwrite(m->regs,1,sizeof(m->regs),f)==sizeof(m->regs));assert(!fclose(f)); }
    unsigned writes=m->writes;
    assert(a733_iommu_scan(&s,20000)==1 && run(&s)==1 && s.fault.complete);
    m->regs[0x100b4/4]=4; /* Walk fault after the bank status sample. */
    assert(a733_iommu_scan(&s,25000)==1 && run(&s)==-1 && s.fault.complete);
    assert(s.fault.l2_status[1]==4 && s.admission.token==37);
    m->regs[0x88/4]=1u<<16;m->regs[0x638/4]=1;
    m->regs[0x63c/4]=0x12345000;m->regs[0x640/4]=2;m->regs[0x644/4]=0xab;
    assert(a733_iommu_scan(&s,30000)==1 && run(&s)==-1 && s.fault.complete);
    assert(s.fault.micro_low[1]==0x12345000 && s.fault.micro_high[1]==2 && s.fault.micro_data[1]==0xab);
    assert(m->writes==writes && s.admission.token==37);
    memset(m->regs,0,sizeof(m->regs));
    assert(a733_iommu_scan(&s,40000)==1 && run(&s)==-1); /* Fault remains terminal. */
    for(unsigned mode=0;mode<12;++mode) {
        memset(m,0,sizeof(*m));memset(&s,0,sizeof(s));
        switch(mode) {
        case 0:m->deny=1;break;
        case 1:m->unresolved=1;break;
        case 2:m->regs[0x10014/4]=1;break; /* Other bank inherited active. */
        case 3:m->regs[0x10438/4]=1;break; /* Other master fault. */
        case 4:m->drop=1;break;
        case 5:m->late=1;break;
        case 6:m->regression=1;break;
        case 7:m->admit_pending=1;m->token_change=1;break;
        case 8:m->publish_pending=500;break;
        case 9:m->late_read=1;break;
        case 10:m->admit_pending=1;break;
        case 11:m->undrained=1;break;
        }
        assert(a733_iommu_init(&s,&ops,m,t)==1 && a733_iommu_open(&s,mode==8?30:10000)==1);
        if(mode==10) { assert(a733_iommu_poll(&s)==0);arena[0]=1; }
        assert(run(&s)==-1 && s.state==A733_IOMMU_FAULT && s.admission.token==37);
        if(mode==10)arena[0]=0;
        if(mode<4 || mode>=6)assert(m->writes==0);
        writes=m->writes;assert(a733_iommu_poll(&s)==-1 && m->writes==writes);
    }
    free(m);
}
static void inherited_walk_checks(void)
{
    struct a733_iommu_tables t={0};
    assert(a733_iommu_build(&t,arena,0x140000000,maps,3)==1);
    struct model *m=calloc(1,sizeof(*m));assert(m);
    const unsigned offsets[]={0xb0,0xb4,0x100b0,0x100b4};
    for(unsigned i=0;i<4;++i) {
        memset(m,0,sizeof(*m));
        struct a733_iommu s={0};
        /* An isolated inherited walk error must refuse even when bank and
         * micro interrupt status are clear and admission reports full drain. */
        m->regs[offsets[i]/4]=1;
        assert(a733_iommu_init(&s,&ops,m,&t)==1 && a733_iommu_open(&s,10000)==1);
        assert(run(&s)==-1 && s.state==A733_IOMMU_FAULT && s.admission.token==37);
        assert(m->reads==5+i && !m->writes && !m->publications);
        assert(m->regs[offsets[i]/4]==1);
        assert(a733_iommu_poll(&s)==-1 && m->reads==5+i);
    }
    free(m);
    puts("A733 IOMMU: isolated inherited L1/L2 walk faults in both banks refuse before publication; token retained");
}
int main(int argc,char **argv)
{
    if(argc==2 && !strcmp(argv[1],"--inherited-walk-only")) {
        inherited_walk_checks();return 0;
    }
    struct a733_iommu_tables t={0};builder_checks(&t);
    controller_checks(&t,argc>1?argv[1]:NULL);
    puts("A733 IOMMU: page walks, guards, atomic refusal, global admission, publication, cold install and retained faults passed");
    return 0;
}
