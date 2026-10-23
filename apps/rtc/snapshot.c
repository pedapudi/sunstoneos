#include "a733_rtc_application.h"
#include "a733_rtc_boot.h"
#include "a733_cpu_coherence.h"
#include "a733_storage_digest.h"
#include "a733_mmio.h"
#include "boot.h"
#include <string.h>
static struct {
 struct a733_rtc_session_set session;
 struct a733_rtc_initialization initialization;
 struct a733_rtc_boot boot;
 unsigned attempted,ready;
} admitted;
static const struct a733_smp_contract *boot_contract;
static int access_ok(void)
{
 uint64_t el,mpidr;
 __asm__ volatile("mrs %0, CurrentEL\n mrs %1, mpidr_el1":"=r"(el),"=r"(mpidr));
 return el==12&&!(mpidr&UINT64_C(0xff00ffffff))&&boot_contract&&
        a733_smp_counter()<boot_contract->startup_deadline;
}
static int read32(void *c,uintptr_t p,uint32_t *v)
{
 if(c!=&admitted||!v||!access_ok())return 0;
 *v=a733_mmio_read32_dsb_sy_both(p);return access_ok();
}
static int write32(void *c,uintptr_t p,uint32_t v)
{
 if(c!=&admitted||!access_ok())return 0;
 a733_mmio_write32_dsb_sy_after(p,v);return access_ok();
}
static int route64(void *c,uintptr_t p,uint64_t v)
{
 if(c!=&admitted||!access_ok())return 0;
 a733_mmio_write64_dsb_sy_after(p,v);return access_ok();
}
int rtc_session_prepare(const struct a733_smp_contract *contract)
{
 if(!contract||admitted.attempted)return 0;
 admitted.attempted=1;
 const uint64_t bytes=contract->reserved[1];
 if(contract->magic!=A733_RTC_CONTRACT_MAGIC||
    contract->reserved[0]!=A733_BOARD_SESSION_ADDRESS||
    (bytes!=sizeof(struct a733_rtc_session)&&
     bytes!=sizeof(admitted.session)))return 0;
 const volatile uint8_t *source=(const void *)A733_BOARD_SESSION_ADDRESS;
 for(unsigned i=0;i<bytes;++i)((uint8_t *)&admitted.session)[i]=source[i];
 uint8_t hash[32];storage_digest_bytes(&admitted.session,(size_t)bytes,hash);
 if(memcmp(hash,&contract->reserved[2],32)||admitted.session.base.bytes!=bytes||
    !a733_rtc_session_valid(&admitted.session.base))return 0;
 boot_contract=contract;
 struct a733_rtc_boot_ops ops={read32,write32,route64};
 if(!access_ok()||!a733_rtc_boot_bus(&admitted.boot,&ops,&admitted))return 0;
 if(admitted.session.base.version>=2){
  extern int rtc_boot_initialize(const struct a733_rtc_setting *,struct a733_rtc_initialization *,
      const struct a733_smp_contract *);
  if(!rtc_boot_initialize(&admitted.session.setting,&admitted.initialization,contract))return 0;
 }
 /* Ready remains private until all early ownership and publication succeeds. */
 admitted.ready=1;
 if(a733_cpu_coherence_publish((uintptr_t)&admitted,sizeof(admitted),contract->startup_deadline)==1)return 1;
 admitted.ready=0;return 0;
}
int rtc_boot_route(void)
{
 if(!admitted.ready||!access_ok()||!a733_rtc_boot_route(&admitted.boot,0)){admitted.ready=0;return 0;}
 if(a733_cpu_coherence_publish((uintptr_t)&admitted,sizeof(admitted),boot_contract->startup_deadline)==1)return 1;
 admitted.ready=0;return 0;
}
const struct a733_rtc_session *rtc_admitted_session(void)
{return admitted.ready&&admitted.boot.state==2?&admitted.session.base:NULL;}

const struct a733_rtc_setting *rtc_admitted_setting(void)
{return rtc_admitted_session()&&admitted.session.base.version>=2?&admitted.session.setting:NULL;}
const struct a733_rtc_initialization *rtc_admitted_initialization(void)
{return rtc_admitted_setting()?&admitted.initialization:NULL;}
