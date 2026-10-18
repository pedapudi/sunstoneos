#include "a733_iommu.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static _Alignas(16384) uint32_t table[8192];
struct model {uint32_t regs[32768];uint64_t now;unsigned accesses,drained;};
static int now(void *v,uint64_t *n){*n=++((struct model *)v)->now;return 1;}
static int admit(void *v,const struct a733_iommu_tables *t,uint64_t d,struct a733_iommu_admission *a)
{(void)t;(void)d;*a=(struct a733_iommu_admission){.token=9,.excluded_masters=A733_IOMMU_REAL_MASTERS,.powered_masters=0xc0,.inherited_resolved=1,.transactions_drained=((struct model *)v)->drained,.clocks_held=1,.retained_power_removal=1};return 1;}
static void offset(uint32_t off)
{assert(off<0x20000&&!(off&3));unsigned local=off&0xffff;if(local>=0x400)assert(off>=0x10400&&off<0x10800);}
static int rd(void *v,uint64_t t,uint32_t off,uint32_t *value,uint64_t d)
{(void)d;assert(t==9);offset(off);struct model*m=v;++m->accesses;*value=m->regs[off/4];return 1;}
static int wr(void *v,uint64_t t,uint32_t off,uint32_t value,uint64_t d)
{(void)d;assert(t==9);offset(off);struct model*m=v;++m->accesses;m->regs[off/4]=value;return 1;}
static int publish(void *v,uint64_t t,const void *p,size_t n,uint64_t d)
{(void)v;(void)d;assert(t==9&&p==table&&n==32768);return 1;}
static int poll(struct a733_iommu *i)
{int r=0;for(unsigned n=0;!r&&n<400;++n)r=a733_iommu_poll(i);assert(r);return r;}
int main(int argc,char **argv)
{
    (void)argv;assert(argc==1||argc==2);
    struct a733_iommu_tables tables;
    const struct a733_iommu_span spans[]={{0x10001000,0x41c01000,4096,1},{0x10005000,0x41c05000,4096,2},{0x10009000,0x41c09000,4096,2}};
    assert(a733_iommu_build(&tables,table,0x41c10000,spans,3)==1);
    const struct a733_iommu_ops ops={now,admit,rd,wr,publish};
    if(argc==1)for(unsigned drained=0;drained<2;++drained){
        struct model m={.drained=drained};struct a733_iommu i={0};
        assert(a733_iommu_init_decoder(&i,&ops,&m,&tables)==1&&a733_iommu_open(&i,10000)==1);
        assert(poll(&i)==(drained?1:-1));
        if(!drained){assert(!m.accesses);continue;}
        assert(m.regs[0x10404/4]==0&&m.regs[0x10604/4]==0);
        assert(a733_iommu_scan(&i,20000)==1&&poll(&i)==1&&i.fault.scanned_masters==0xc0);
        m.regs[0x10638/4]=1;assert(a733_iommu_scan(&i,30000)==1&&poll(&i)==-1&&i.fault.micro_status[5]==1);
        assert(i.admission.token==9);
    }
    uint64_t pa;assert(a733_iommu_translate(&tables,0x10001000,1,&pa)==1&&pa==0x41c01000);
    assert(a733_iommu_translate(&tables,0x10001000,2,&pa)==-1&&a733_iommu_translate(&tables,0x10000000,1,&pa)==-1);
    puts("JPEG decoder IOMMU: only6/7 micro MMIO, full prior drain, guards and retained fault passed");return 0;
}
