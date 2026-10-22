#include "a733_rtc_application.h"
#include "a733_mmio.h"
#include "boot.h"
/* This provider exists only during the sealed primary EL3 hook. The authenticated
 * clock authority excludes all inherited32k consumers/writers, not just tasks. */
static const struct a733_smp_contract *initializing;
static int allowed(void)
{
 uint64_t el,mpidr;__asm__ volatile("mrs %0, CurrentEL\n mrs %1, mpidr_el1":"=r"(el),"=r"(mpidr));
 return initializing&&el==12&&!(mpidr&UINT64_C(0xff00ffffff))&&
        a733_smp_counter()<initializing->startup_deadline;
}
static int now(void *c,uint64_t *value)
{if(c!=initializing||!value||!allowed())return 0;*value=a733_smp_counter()/24;return allowed();}
static int read_reg(void *c,uint32_t offset,uint32_t *value)
{
 if(c!=initializing||!value||!allowed()||offset>0x50||(offset&3))return 0;
 *value=a733_mmio_read32_dsb_sy_both(0x07090000+offset);return allowed();
}
static int write_reg(void *c,uint32_t offset,uint32_t value)
{
 if(c!=initializing||!allowed()||(offset!=0&&offset!=4&&offset!=0x10&&offset!=0x14)||
    (offset==0&&(value>>16)!=0x16aa)||(offset==4&&value!=2))return 0;
 a733_mmio_write32_dsb_sy_after(0x07090000+offset,value);return allowed();
}
int rtc_boot_initialize(const struct a733_rtc_setting *setting,struct a733_rtc_initialization *out,
 const struct a733_smp_contract *contract)
{
 if(initializing||!contract)return 0;initializing=contract;
 struct a733_rtc_ops ops={now,read_reg,write_reg};
 return a733_rtc_initialize(setting,&ops,(void *)contract,contract->startup_deadline/24,out);
}
