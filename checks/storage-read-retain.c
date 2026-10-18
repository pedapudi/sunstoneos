/* Reuse existing provider models; execute only the new metadata API cases. */
#include "storage-platform-model.h"
static int forbidden_now(void *v,uint64_t *out){(void)v;(void)out;assert(!"retain read time");return 0;}
int main(void)
{
 storage_setup(SUNXI_MMC_SD);
 m.platform.ops.now_us=forbidden_now;
 m.power.worker=1;
 assert(a733_mmc_platform_retain(&m.platform)==A733_MMC_INVALID&&!m.platform.fault);
 m.power.worker=0;
 atomic_store(&m.platform.busy,1);
 assert(a733_mmc_platform_retain(&m.platform)==A733_MMC_BUSY&&!m.platform.fault);
 atomic_store(&m.platform.busy,0);
 m.platform.update=(void *)1;
 assert(a733_mmc_platform_retain(&m.platform)==A733_MMC_INVALID&&!m.platform.fault);
 m.platform.update=NULL;
 assert(a733_clock_acquire(&m.power.clocks,42,m.platform.shared,A733_CLOCK_SDC0,1000000,&m.platform.clock)==A733_CLOCK_OK);
 assert(a733_rail_acquire(&m.power.rails,42,A733_RAIL_BIT(A733_RAIL_DCDC1),0,1000000,&m.platform.rail)==A733_RAIL_PENDING);
 uint8_t borrowed[512]={7};m.platform.core.borrowed=borrowed;m.platform.core.borrowed_bytes=512;
 m.power.now=20000000; /* Structural ownership remains valid after all deadlines. */
 atomic_store(&m.power.clocks.busy,1);atomic_store(&m.power.rails.busy,1);
 unsigned accesses=m.accesses;
 assert(a733_mmc_platform_retain(&m.platform)==A733_MMC_BUSY&&m.platform.fault);
 assert(m.accesses==accesses&&!m.platform.clock_retained&&!m.platform.rail_retained);
 atomic_store(&m.power.clocks.busy,0);atomic_store(&m.power.rails.busy,0);
 assert(a733_mmc_platform_retain(&m.platform)==A733_MMC_RETAINED);
 assert(m.platform.clock_retained&&m.platform.rail_retained&&m.accesses==accesses);
 assert(m.platform.core.borrowed==borrowed&&borrowed[0]==7&&m.platform.pin.id==0);
 assert(a733_mmc_platform_retain(&m.platform)==A733_MMC_RETAINED&&m.accesses==accesses);
 puts("PASS: current owner, API busy, update rejection, expired metadata retention and manager BUSY retry; no MMIO/time/buffer reuse");
}
