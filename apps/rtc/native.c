#include "rtc.h"
#include "boot.h"
#include "a733_storage_digest.h"
#include "aarch64_device_memory.h"
#include <string.h>
struct rtc_application rtc;
static uint64_t deadline(void){return board.session.deadline_us>10000?board.session.deadline_us-10000:0;}
static int now(void *c,uint64_t *out)
{return c==&rtc&&board_live(NULL)&&a733_counter_clock_now(&providers.shared.counter,out)==1;}
static int device(uintptr_t p)
{
 struct aarch64_translation t;
 return aarch64_device_translate(p,1,&t)&&t.physical==p&&t.attributes==0&&t.shareability==2;
}
static int read_reg(void *c,uint32_t offset,uint32_t *v)
{
 if(c!=&rtc||!v||offset>0x50||(offset&3)||!board_live(NULL))return 0;
 uintptr_t p=0x07090000+offset;
 if(!device(p))return 0;
 __asm__ volatile("dsb sy":::"memory");*v=*(volatile uint32_t *)p;
 __asm__ volatile("dsb sy":::"memory");return board_live(NULL);
}
static int write_reg(void *c,uint32_t offset,uint32_t value)
{
 if(c!=&rtc||!board_live(NULL)||
    (offset!=0x20&&offset!=0x24&&offset!=0x28&&offset!=0x2c&&offset!=0x30)||
    (offset==0x30&&value!=1))return 0;
 uintptr_t p=0x07090000+offset;if(!device(p))return 0;
 *(volatile uint32_t *)p=value;__asm__ volatile("dsb sy":::"memory");return board_live(NULL);
}
/* Caller has already masked228. Active must clear after the FreeRTOS IRQ
 * wrapper's EOI before task-side RTC access. Never clear inherited GIC pending. */
static int quiet(unsigned pending)
{
 if(!device(0x03400000)||!device(0x0340011c))return 0;
 __asm__ volatile("dsb sy":::"memory");
 if((*(volatile uint32_t *)0x03400000&(1u<<31))||
    (*(volatile uint32_t *)0x0340011c&16)||
    (*(volatile uint32_t *)0x0340031c&16))return 0;
 return !pending||!(*(volatile uint32_t *)0x0340021c&16);
}
static _Noreturn void retained(void)
{
 (void)gicv3_spi_mask(&rtc.spi);
 atomic_store_explicit(&rtc.armed,0,memory_order_release);
 atomic_store_explicit(&rtc.result,2,memory_order_release);
 board_fail(NULL);for(;;)vTaskDelay(1);
}
void rtc_task(void *unused)
{
 (void)unused;
 a733_clock_mask_t stable=A733_CLOCK_OSC|A733_CLOCK_RC16M|A733_CLOCK_FABRIC;
 for(;;){
  if(!board_live(NULL))retained();
  enum a733_clock_result r=a733_clock_acquire(&providers.shared.clocks,rtc.session.owner,
      stable,0,deadline(),&rtc.clock);
  if(r==A733_CLOCK_OK)break;if(r!=A733_CLOCK_BUSY)retained();vTaskDelay(1);
 }
 if(rtc.session.version==4){rtc_service_task();retained();}
 if(rtc.session.version==3){rtc_runtime_task();retained();}
 if(a733_platform_lock_enter(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK,deadline())!=1)retained();
 struct a733_rtc_ops ops={now,read_reg,write_reg};
 if(!gicv3_spi_mask(&rtc.spi)||!quiet(1)||
    !a733_rtc_open(&rtc.driver,&ops,&rtc,deadline())||
    !a733_rtc_arm(&rtc.driver,rtc.session.alarm_seconds)){
  (void)a733_platform_lock_retain(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK);retained();
 }
 a733_platform_lock_leave(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK);
 atomic_store_explicit(&rtc.armed,1,memory_order_release);
 int enabled;
 do {if(!board_live(NULL))retained();enabled=rtc_enable_irq();if(!enabled)vTaskDelay(1);}while(!enabled);
 if(enabled!=1)retained();
 while(!atomic_load_explicit(&rtc.pending,memory_order_acquire)&&
       !atomic_load_explicit(&rtc.cancel,memory_order_acquire)){
  if(!board_live(NULL)||atomic_load_explicit(&rtc.driver.irq_fault,memory_order_acquire))retained();
  vTaskDelay(1);
 }
 if(!gicv3_spi_mask(&rtc.spi))retained();
 while(!quiet(0)){if(!board_live(NULL))retained();vTaskDelay(1);}
 atomic_store_explicit(&rtc.armed,0,memory_order_release);
 if(a733_platform_lock_enter(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK,deadline())!=1)retained();
 if((atomic_load_explicit(&rtc.pending,memory_order_acquire)&&!a733_rtc_irq(&rtc.driver))||
    !a733_rtc_finish(&rtc.driver,atomic_load_explicit(&rtc.cancel,memory_order_acquire))){
  (void)a733_platform_lock_retain(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK);retained();
 }
 a733_platform_lock_leave(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK);
 while(!quiet(1)){if(!board_live(NULL))retained();vTaskDelay(1);}
 if(!board_live(NULL)||a733_clock_token_status(&providers.shared.clocks,&rtc.clock)!=A733_CLOCK_OK)retained();
 rtc.output=rtc.driver.snapshot;
 storage_digest_bytes(&rtc.output,sizeof(rtc.output),rtc.digest);
 if(rtc.session.version==2){
  rtc.initialized_output.calendar=rtc.output;
  storage_digest_bytes(&rtc.initialized_output,sizeof(rtc.initialized_output),rtc.initialized_digest);
 }
 if(!board_live(NULL)||a733_smp_counter()/24>=deadline())retained();
 atomic_store_explicit(&rtc.result,1,memory_order_release);
 for(;;){if(!board_live(NULL))retained();vTaskDelay(1);}
}
int rtc_copy(struct a733_rtc_snapshot *out,uint8_t hash[32])
{
 if(!out||!hash)return 2;
 memset(out,0,sizeof(*out));memset(hash,0,32);
 unsigned r=atomic_load_explicit(&rtc.result,memory_order_acquire);
 if(r!=1)return (int)r;
 if(!board_live(NULL)||a733_smp_counter()/24>=deadline())return 2;
 *out=rtc.output;memcpy(hash,rtc.digest,32);
 if(!board_live(NULL)||a733_smp_counter()/24>=deadline()){
  memset(out,0,sizeof(*out));memset(hash,0,32);return 2;
 }
 return 1;
}

int rtc_copy_initialized(struct rtc_initialized_output *out,uint8_t hash[32])
{
 if(!out||!hash)return 2;
 memset(out,0,sizeof(*out));memset(hash,0,32);
 unsigned result=atomic_load_explicit(&rtc.result,memory_order_acquire);
 if(result!=1)return (int)result;
 if(rtc.session.version!=2||rtc.initialized_output.initialization.stage!=6||
    !board_live(NULL)||a733_smp_counter()/24>=deadline())return 2;
 *out=rtc.initialized_output;memcpy(hash,rtc.initialized_digest,32);
 if(!board_live(NULL)||a733_smp_counter()/24>=deadline()){
  memset(out,0,sizeof(*out));memset(hash,0,32);return 2;
 }
 return 1;
}
