#include "rtc.h"
#include "boot.h"
static int live(void)
{return board.session.deadline_us>10000&&board_live(NULL)&&
 a733_smp_counter()/24<board.session.deadline_us-10000;}
int rtc_enable_irq(void)
{
 /* Owner task affinity is core0 only, matching the sealed GIC228 route.
  * Local IRQ masking prevents the handler's disable from racing enable readback.
  * No platform mutex or blocking operation occurs inside this critical section. */
 taskENTER_CRITICAL();
 int result=live()?gicv3_spi_rearm(&rtc.spi):-1;
 taskEXIT_CRITICAL();return result;
}
void rtc_application_irq(void)
{
 /* RTC MMIO belongs only to the owner task under the register mutex. */
 if(!gicv3_spi_mask(&rtc.spi)||!atomic_load_explicit(&rtc.armed,memory_order_acquire)||!live()){
  atomic_store_explicit(&rtc.driver.irq_fault,1,memory_order_release);board_fail(NULL);return;
 }
 atomic_store_explicit(&rtc.pending,1,memory_order_release);
}
