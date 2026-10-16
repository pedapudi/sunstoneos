/* SPDX-License-Identifier: BSD-3-Clause */
#include "a733_cpu_coherence.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static _Alignas(4096) uint64_t l1[512],l2[512],e1[512],e2[512];
struct model { uint64_t reg[A733_COH_REGISTERS],now;unsigned calls,writes,fail,late,drop,regress; };
static int now(void *p,uint64_t *v)
{ struct model *m=p;*v=++m->now;if(m->regress && m->calls)*v=0;return 1; }
static int readreg(void *p,unsigned r,uint64_t *v)
{
    struct model *m=p;assert(r<A733_COH_REGISTERS);++m->calls;
    if(m->fail==m->calls)return -1;
    *v=m->reg[r];if(m->late==m->calls)m->now=10000;return 1;
}
static int writereg(void *p,unsigned r,uint64_t v)
{
    struct model *m=p;assert(r>=A733_COH_ACTLR && r<A733_COH_REGISTERS);++m->calls;++m->writes;
    if(m->fail==m->calls)return -1;
    if(!m->drop)m->reg[r]=v;
    if(m->late==m->calls)m->now=10000;
    return 1;
}
static const struct a733_cpu_coherence_ops ops={now,readreg,writereg};
static struct model model(unsigned core,unsigned rev)
{
    struct model m={0};m.reg[A733_COH_CURRENT_EL]=12;m.reg[A733_COH_DAIF]=0x3c0;
    m.reg[A733_COH_MIDR]=(core<6?0x410fd050u:0x410fd0b0u)|((uint64_t)(rev>>4)<<20)|(rev&15);
    m.reg[A733_COH_MPIDR]=0x81000000|(core<<8);m.reg[A733_COH_CLIDR]=0x124;
    m.reg[A733_COH_MMFR0]=2;m.reg[A733_COH_CLUSTERIDR]=0x30;
    for(unsigned i=A733_COH_ACTLR;i<=A733_COH_CLUSTERACTLR;++i)m.reg[i]=UINT64_C(1)<<40;
    return m;
}
static int run(struct a733_cpu_coherence *s)
{
    for(unsigned i=0;i<200;++i) {
        struct model *m=s->context;unsigned c=m->calls;
        int r=a733_cpu_coherence_poll(s);assert(m->calls<=c+1);if(r)return r;
    }
    assert(0);return 0;
}
static void tables(void)
{
    assert(a733_cpu_coherence_tables(l1,l2,0x42001000,1)==1);
    assert(a733_cpu_coherence_tables(e1,e2,0x42003000,3)==1);
    assert(l1[1]==0x42001003 && e1[1]==0x42003003);
    for(unsigned i=2;i<512;++i)assert(!l1[i] && !e1[i]);
    for(unsigned i=0;i<512;++i) {
        if(i>=64) { assert(!l2[i] && !e2[i]);continue; }
        uint64_t a=UINT64_C(0x40000000)+((uint64_t)i<<21),v=l2[i],e=e2[i];
        assert((v&UINT64_C(0xffffffe00000))==a && (e&UINT64_C(0xffffffe00000))==a);
        assert(((v>>8)&3)==3 && ((e>>8)&3)==3);
        assert(((v>>2)&7)==(i==16?2:1) && ((e>>2)&7)==((v>>2)&7));
        assert((e&(UINT64_C(1)<<5)) && (e&(UINT64_C(1)<<6)) && !(e&(UINT64_C(1)<<53)));
        if(i==8)assert(((v>>6)&3)==3 && !(v>>53) && !(e>>53));
        else assert((v>>53)==3 && (e>>53)==2 && ((v>>6)&3)==(i==16?0:1));
    }
    uint64_t save=l2[0];assert(a733_cpu_coherence_tables(l1,l1,0x42001000,1)==-1);
    assert(a733_cpu_coherence_tables(l1,l2,0x42200000,1)==-1);
    assert(a733_cpu_coherence_tables(l1,l2,0x42001000,2)==-1 && l2[0]==save);
}
int main(int argc,char **argv)
{
    if(argc==2 && !strcmp(argv[1],"--parange-only")) {
        for(unsigned core=0;core<8;++core) {
            struct a733_cpu_coherence_identity id={.midr=core<6?0x412fd050:0x414fd0b1,
                .mpidr=0x81000000|(core<<8),.clidr=0x124,.clusteridr=0x30,.mmfr0=1};
            struct a733_cpu_coherence_plan p={0},saved=p;
            assert(a733_cpu_coherence_plan(&p,&id,core)==-1 && !memcmp(&p,&saved,sizeof(p)));
            id.mmfr0=2;assert(a733_cpu_coherence_plan(&p,&id,core)==1);
        }
        puts("PASS 36-bit physical-address refusal and 40-bit admission on all eight core identities");return 0;
    }
    tables();
    const unsigned revs[][7]={{0,1,0x10,0x20},{0,0x10,0x20,0x30,0x31,0x40,0x41}};
    for(unsigned core=0;core<8;++core)for(unsigned i=0;i<(core<6?4:7);++i) {
        struct model m=model(core,revs[core>=6][i]);struct a733_cpu_coherence s={0};
        assert(a733_cpu_coherence_init(&s,&ops,&m,core,9,10000)==1 && run(&s)==1);
        assert(s.state==A733_COH_PREPARED && s.token==9 && s.plan.core==core);
        unsigned calls=m.calls;assert(a733_cpu_coherence_poll(&s)==1 && m.calls==calls);
        for(unsigned r=A733_COH_ACTLR;r<=A733_COH_CLUSTERACTLR;++r)assert(m.reg[r]&(UINT64_C(1)<<40));
    }
    struct model good=model(6,0);struct a733_cpu_coherence s={0};
    assert(a733_cpu_coherence_init(&s,&ops,&good,6,9,10000)==1 && run(&s)==1);
    /* Every native register boundary can fail or return after the deadline. */
    for(unsigned n=1;n<=good.calls;++n)for(unsigned late=0;late<2;++late) {
        struct model m=model(6,0);memset(&s,0,sizeof(s));
        if(late)m.late=n;else m.fail=n;
        assert(a733_cpu_coherence_init(&s,&ops,&m,6,9,10000)==1 && run(&s)==-1);
        assert(s.state==A733_COH_RETAINED && s.token==9 && m.calls==n);
        assert(a733_cpu_coherence_poll(&s)==-1 && m.calls==n);
    }
    for(unsigned n=0;n<7;++n) {
        struct model m=model(0,0x20);memset(&s,0,sizeof(s));
        switch(n) { case 0:m.reg[A733_COH_MIDR]|=3;break;case 1:m.reg[A733_COH_MPIDR]|=1;break;
        case 2:m.reg[A733_COH_DAIF]=0;break;case 3:m.reg[A733_COH_SCTLR3]=4;break;
        case 4:m.reg[A733_COH_MMFR0]=0;break;case 5:m.reg[A733_COH_CLUSTERIDR]=0x90;break;
        default:m.regress=1;break; }
        assert(a733_cpu_coherence_init(&s,&ops,&m,0,9,10000)==1 && run(&s)==-1 && !m.writes);
    }
    for(unsigned core=0;core<2;++core) {
        struct model m=model(core,0x20);memset(&s,0,sizeof(s));m.reg[A733_COH_CLUSTERIDR]=0;
        m.reg[A733_COH_CLUSTERCFR]=1<<11;
        assert(a733_cpu_coherence_init(&s,&ops,&m,core,9,10000)==1);
        if(core)assert(run(&s)==-1 && !m.writes); /* Secondary cannot repair shared DSU. */
        else assert(run(&s)==1 && (m.reg[A733_COH_CLUSTERACTLR]&0x18000)==0x18000);
    }
    struct model m=model(6,0);m.drop=1;memset(&s,0,sizeof(s));
    assert(a733_cpu_coherence_init(&s,&ops,&m,6,9,10000)==1 && run(&s)==-1 && s.error==4);
    puts("PASS revision admission, matched EL1/EL3 mappings, preserved controls, bounded failures and retained ownership");
}
