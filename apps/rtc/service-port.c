#include "rtc.h"
#include "service-port.h"
#include "boot.h"
#include "a733_storage_digest.h"
#include "a733_mapping_check.h"
#include "a733_mmio.h"
#include <string.h>
uint64_t rtc_service_deadline(void)
{return board.session.deadline_us>10000?board.session.deadline_us-10000:0;}
int rtc_service_live(void)
{
 /* This exact lease is permanent: never released or changed by this owner.
  * The registry's documented atomic fault check avoids waiting on its lock
  * while holding the unrelated register mutex. External writers are excluded
  * by the sealed lifetime calendar/source authority. */
 return board_live(NULL)&&a733_smp_counter()/24<rtc_service_deadline()&&rtc.clock.id&&
  rtc.clock.owner==rtc.session.owner&&rtc.clock.shared==(A733_CLOCK_OSC|A733_CLOCK_RC16M|A733_CLOCK_FABRIC)&&
  !rtc.clock.exclusive&&!atomic_load_explicit(&providers.shared.clocks.failed,memory_order_acquire);
}
int rtc_service_now(void *c,uint64_t *value)
{return !c&&value&&rtc_service_live()&&
 a733_counter_clock_now(&providers.shared.counter,value)==1&&rtc_service_live();}
static const struct a733_mapping_rule device_rule={A733_MAPPING_WRITE,1,{{A733_MAIR_DEVICE_NGNRNE,A733_SH_OUTER}},0,1};
static int mapped(uintptr_t address)
{return a733_mapping_check(aarch64_device_translate,address,address,&device_rule);}
int rtc_service_read(void *c,uint32_t offset,uint32_t *value)
{
 if(c||!value||offset>0x50||(offset&3)||!rtc_service_live()||
    a733_platform_lock_enter(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK,rtc_service_deadline())!=1)return 0;
 int result=0;uintptr_t address=0x07090000+offset;
 if(rtc_service_live()&&mapped(address)){
  *value=a733_mmio_read32_dsb_sy_both(address);result=rtc_service_live();
 }
 a733_platform_lock_leave(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK);return result;
}
int rtc_service_write(void *c,uint32_t offset,uint32_t value)
{
 if(c||(offset!=0x10&&offset!=0x14&&offset!=0x20&&offset!=0x24&&offset!=0x28&&offset!=0x2c&&offset!=0x30)||(offset==0x30&&value!=1)||!rtc_service_live()||
    a733_platform_lock_enter(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK,rtc_service_deadline())!=1)return 0;
 int result=0;uintptr_t address=0x07090000+offset;
 if(rtc_service_live()&&mapped(address)){
  /* The first attempt boundary is inside the mutex. A special return2 proves
   * no write was attempted; any other failed callback may have mutated RTC. */
  if(offset==0x14&&rtc_service_first_attempt()&&rtc_service_active_cancel())result=2;
  else{
   if(offset==0x14)rtc_service_note_attempt();
   a733_mmio_write32_dsb_sy_after(address,value);result=rtc_service_live();
  }
 }
 /* Only the private RTC state is uncertain on failure. Its exclusive lifetime
  * ownership remains retained; unrelated clients may keep using this mutex. */
 a733_platform_lock_leave(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK);return result;
}
int rtc_service_mask(void){return rtc_service_live()&&gicv3_spi_mask(&rtc.spi);}
int rtc_service_quiet(unsigned pending)
{
 if(!rtc_service_live()||!mapped(0x03400000))return 0;
 a733_mmio_dsb_sy();
 return !(a733_mmio_read32(0x03400000)&(1u<<31))&&
        !((a733_mmio_read32(0x0340011c)|a733_mmio_read32(0x0340031c))&16)&&
        (!pending||!(a733_mmio_read32(0x0340021c)&16));
}
int rtc_service_enable(void){return rtc_enable_irq();}
int rtc_service_pending(void){return atomic_load_explicit(&rtc.pending,memory_order_acquire);}
int rtc_service_irq_fault(void){return atomic_load_explicit(&rtc.driver.irq_fault,memory_order_acquire);}
void rtc_service_armed(unsigned value){atomic_store_explicit(&rtc.armed,value,memory_order_release);}
void rtc_service_clear_pending(void){atomic_store_explicit(&rtc.pending,0,memory_order_release);}
void rtc_service_fault(void){board_fail(NULL);}
void rtc_service_hash(const void *p,unsigned bytes,uint8_t hash[32]){storage_digest_bytes(p,bytes,hash);}
void rtc_service_terminal(const struct rtc_service_output *out){(void)out;}
