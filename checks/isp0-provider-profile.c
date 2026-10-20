/* Validate only the selected ISP0 media/PMIC profile, reusing accepted fixture. */
#define main media_previous_cases
#include "a733-media-clock.c"
#undef main
#include "a733_pmic.h"
int main(void)
{
 struct model m;setup(&m,A733_MEDIA_VI|A733_MEDIA_IOMMU);
 struct a733_gpu_resource_lease lease=m.driver.pck;
 memset(&m.driver,0,sizeof(m.driver));
 m.ccu[0x500/4]=0;m.ccu[0x580/4]=m.ccu[0x588/4]=0x80000000;
 m.ccu[0x58c/4]=m.ccu[0x5b4/4]=0x10007;
 const struct a733_media_clock_config c={.owner=5,.handoff=99,.deadline_us=1000000,
  .groups=A733_MEDIA_VI|A733_MEDIA_IOMMU,.camera_mclk_mask=0,
  .fabric={0,0x80000000,0x80000000},.rate_hz={24000000,24000000,24000000}};
 const struct a733_media_clock_ops ops={now,auth,enter,leave,retain,read32,write32};
 assert(a733_media_clock_init(&m.driver,&c,&ops,&m,&m.registry,&m.manager,2,&lease));
 assert(run(&m)==A733_MEDIA_PREPARED);
 assert(m.ccu[0x1840/4]==0x85000000&&m.ccu[0x1860/4]==0x84000000);
 assert(!(m.ccu[0x1804/4]&(1u<<31))&&!(m.ccu[0x1808/4]&(1u<<31)));
 struct a733_pmic_snapshot s={0};assert(a733_pmic_decode(A733_RAIL_DCDC2,2,30,&s));
 assert(s.enabled&&s.decoded&&s.programmed_uv==800000);
 puts("Selected ISP0 all24MHz fabric,324MHz CSI/ISP,no sensor MCLK,DCDC2 800mV profile passed");
 return 0;
}
