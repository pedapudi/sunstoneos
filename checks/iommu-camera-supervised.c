/* New CSI supervised admission only; no register read/write or installation. */
#include "a733_iommu.h"
#include <assert.h>
#include <stdio.h>
static uint32_t words[A733_IOMMU_TABLE_BYTES/4] __attribute__((aligned(16384)));
static unsigned manual,missing_drain,calls;
static int now(void *p,uint64_t *v){(void)p;*v=10;return 1;}
static int admit(void *p,const struct a733_iommu_tables *t,uint64_t d,struct a733_iommu_admission *a)
{
 (void)p;(void)t;(void)d;
 *a=(struct a733_iommu_admission){.token=1,.excluded_masters=A733_IOMMU_REAL_MASTERS,
  .powered_masters=A733_IOMMU_REAL_MASTERS,.inherited_resolved=1,.transactions_drained=!missing_drain,
  .clocks_held=1,.recovery_qualified=!manual,.retained_power_removal=manual};return 1;
}
static int read_reg(void *p,uint64_t t,uint32_t o,uint32_t *v,uint64_t d)
{(void)p;(void)t;(void)o;(void)v;(void)d;++calls;return -1;}
static int write_reg(void *p,uint64_t t,uint32_t o,uint32_t v,uint64_t d)
{(void)p;(void)t;(void)o;(void)v;(void)d;++calls;return -1;}
static int publish(void *p,uint64_t t,const void *c,size_t n,uint64_t d)
{(void)p;(void)t;(void)c;(void)n;(void)d;++calls;return -1;}
int main(void)
{
 struct a733_iommu_tables tables={0};
 struct a733_iommu_span span={0x48008000,0x48008000,4096,A733_IOMMU_WRITE};
 assert(a733_iommu_build(&tables,words,0x48000000,&span,1)==1);
 struct a733_iommu_ops ops={now,admit,read_reg,write_reg,publish};
 manual=1;
 struct a733_iommu old={0};assert(a733_iommu_init(&old,&ops,NULL,&tables)==1);
 assert(a733_iommu_open(&old,100)==1&&a733_iommu_poll(&old)==-1&&!calls);
 for(unsigned variant=0;variant<3;++variant){
  struct a733_iommu current={0};manual=variant!=1;missing_drain=variant==2;
  assert(a733_iommu_init_camera_supervised(&current,&ops,NULL,&tables)==1);
  assert(current.translated_masters==2&&current.accessible_masters==A733_IOMMU_REAL_MASTERS);
  assert(a733_iommu_open(&current,100)==1);
  int r=a733_iommu_poll(&current);
  assert(variant?(r==-1&&current.state==A733_IOMMU_FAULT):(r==0&&current.state==A733_IOMMU_CHECK));
  assert(!calls);
 }
 puts("PASS CSI supervised IOMMU admission and strict default/drain refusal before MMIO");return 0;
}
