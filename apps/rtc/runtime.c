#include "rtc.h"
#include "boot.h"
#include "a733_storage_digest.h"
#include "a733_mapping_check.h"
#include "a733_mmio.h"
#include <string.h>
static uint64_t deadline(void)
{return board.session.deadline_us>10000?board.session.deadline_us-10000:0;}
static int live(void)
{
 /* This exact lease is permanent: never released or changed by this owner.
  * The registry's documented atomic fault check avoids waiting on its lock
  * while holding the unrelated register mutex. External writers are excluded
  * by the sealed lifetime calendar/source authority. */
 return board_live(NULL)&&a733_smp_counter()/24<deadline()&&rtc.clock.id&&
  rtc.clock.owner==rtc.session.owner&&rtc.clock.shared==(A733_CLOCK_OSC|A733_CLOCK_RC16M|A733_CLOCK_FABRIC)&&
  !rtc.clock.exclusive&&!atomic_load_explicit(&providers.shared.clocks.failed,memory_order_acquire);
}
static int now(void *c,uint64_t *value)
{return c==&rtc&&rtc.runtime_active&&value&&live()&&
 a733_counter_clock_now(&providers.shared.counter,value)==1&&live();}
static const struct a733_mapping_rule device_rule={A733_MAPPING_WRITE,1,{{A733_MAIR_DEVICE_NGNRNE,A733_SH_OUTER}},0,1};
static int mapped(uintptr_t address)
{return a733_mapping_check(aarch64_device_translate,address,address,&device_rule);}
static int read_reg(void *c,uint32_t offset,uint32_t *value)
{
 if(c!=&rtc||!value||!rtc.runtime_active||offset>0x50||(offset&3)||!live()||
    a733_platform_lock_enter(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK,deadline())!=1)return 0;
 int result=0;uintptr_t address=0x07090000+offset;
 if(live()&&mapped(address)){
  *value=a733_mmio_read32_dsb_sy_both(address);result=live();
 }
 a733_platform_lock_leave(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK);return result;
}
static int write_reg(void *c,uint32_t offset,uint32_t value)
{
 if(c!=&rtc||!rtc.runtime_active||(offset!=0x10&&offset!=0x14)||!live()||
    a733_platform_lock_enter(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK,deadline())!=1)return 0;
 int result=0;uintptr_t address=0x07090000+offset;
 if(live()&&mapped(address)){
  /* The first attempt boundary is inside the mutex. A special return2 proves
   * no write was attempted; any other failed callback may have mutated RTC. */
  if(!rtc.runtime_attempted&&atomic_load_explicit(&rtc.cancel,memory_order_acquire))result=2;
  else{
   rtc.runtime_attempted=1;
   a733_mmio_write32_dsb_sy_after(address,value);result=live();
  }
 }
 /* Only the private calendar is uncertain on failure. Its exclusive lifetime
  * ownership remains retained; unrelated clients may keep using this mutex. */
 a733_platform_lock_leave(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK);return result;
}
static int pause(void *c,uint64_t until)
{
 if(c!=&rtc||!live())return 0;
 if(a733_smp_counter()/24<until)vTaskDelay(1);
 return live();
}
static int cancelled(void *c)
{return c==&rtc&&atomic_load_explicit(&rtc.cancel,memory_order_acquire);}
static int quiescent(void)
{
 if(!mapped(0x03400000))return 0;a733_mmio_dsb_sy();
 return !(a733_mmio_read32(0x03400000)&(1u<<31))&&
        !((a733_mmio_read32(0x0340011c)|a733_mmio_read32(0x0340021c)|
           a733_mmio_read32(0x0340031c))&16);
}
void rtc_runtime_task(void)
{
 /* Entered by the sole core0 RTC owner after its permanent clock acquisition.
  * No alarm is armed in this finite runtime profile. */
 if(rtc.session.version!=3||!live()||!gicv3_spi_mask(&rtc.spi))goto failed;
 while(!quiescent()){if(!live())goto failed;vTaskDelay(1);}
 rtc.runtime_active=1;
 struct a733_rtc_ops ops={now,read_reg,write_reg};
 if(!a733_rtc_set_calendar(&rtc.setting,&ops,&rtc,deadline(),pause,cancelled,&rtc.runtime_output.calendar))goto failed;
 rtc.runtime_active=0;
 if(!live())goto failed;
 storage_digest_bytes(&rtc.runtime_output,sizeof(rtc.runtime_output),rtc.runtime_digest);
 if(!live())goto failed;
 atomic_store_explicit(&rtc.result,1,memory_order_release);
 for(;;){if(!live())goto failed;vTaskDelay(1);}
failed:
 rtc.runtime_active=0;atomic_store_explicit(&rtc.result,2,memory_order_release);
 board_fail(NULL);for(;;)vTaskDelay(1);
}
int rtc_copy_runtime(struct rtc_runtime_output *out,uint8_t hash[32])
{
 if(!out||!hash)return 2;memset(out,0,sizeof(*out));memset(hash,0,32);
 unsigned result=atomic_load_explicit(&rtc.result,memory_order_acquire);
 if(result!=1)return (int)result;
 if(rtc.session.version!=3||!rtc.runtime_output.calendar.outcome||!live())return 2;
 *out=rtc.runtime_output;memcpy(hash,rtc.runtime_digest,32);
 if(!live()){memset(out,0,sizeof(*out));memset(hash,0,32);return 2;}
 return 1;
}
