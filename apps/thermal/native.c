#include "thermal.h"
#include "boot.h"
#include "a733_thermal_native.h"
#include "aarch64_device_memory.h"
#include "a733_storage_digest.h"
#include <string.h>
struct thermal_application thermal;
static uint64_t deadline(void){return board.session.deadline_us-10000;}
static int live(void)
{return board_live(NULL)&&!atomic_load_explicit(&providers.shared.clocks.failed,memory_order_acquire)&&!atomic_load_explicit(&thermal.failed,memory_order_acquire)&&a733_smp_counter()/24<deadline();}
static int owner(void)
{return thermal.handle&&xTaskGetCurrentTaskHandle()==thermal.handle&&live();}
static int now(void *v,uint64_t *n)
{return v==&thermal?a733_counter_clock_now(&providers.shared.counter,n):0;}
static int enter(void *v,uint64_t d)
{return v==&thermal&&owner()?a733_platform_lock_enter(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK,d):-1;}
static void leave(void *v)
{if(v==&thermal)a733_platform_lock_leave(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK);}
static void retain(void *v)
{if(v==&thermal)(void)a733_platform_lock_retain(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK);}
static int device(uintptr_t a)
{struct aarch64_translation t;return aarch64_device_translate(a,1,&t)&&t.physical==a&&t.attributes==0&&t.shareability==2;}
static int mappings(void)
{
 static const uintptr_t a[]={0x02002000,0x02002518,0x02002fc0,0x02002fe4,0x02522000,0x03006244,0x03006248,0x0300624c,0x07090160,0x03400000,0x03400108};
 for(unsigned i=0;i<sizeof(a)/sizeof(*a);++i)if(!device(a[i]))return 0;return 1;
}
static int read_register(void *v,uintptr_t a,uint32_t *out)
{return v==&thermal&&owner()&&thermal.driver.held&&!atomic_load_explicit(&providers.shared.clocks.failed,memory_order_acquire)&&a733_thermal_native_read(NULL,a,out);}
static int write_register(void *v,uintptr_t a,uint32_t value)
{return v==&thermal&&owner()&&thermal.driver.held&&!atomic_load_explicit(&providers.shared.clocks.failed,memory_order_acquire)&&a733_thermal_native_write(NULL,a,value);}
static int authorize(void *v,const struct a733_thermal_config *c,const struct a733_clock_token *token)
{
 return v==&thermal&&owner()&&mappings()&&c->owner==thermal.config.owner&&c->handoff==thermal.config.handoff&&
 c->startup_deadline_us==thermal.config.startup_deadline_us&&c->apb1==thermal.config.apb1&&c->apb1_hz==thermal.config.apb1_hz&&
 c->configure_adc_clock==thermal.config.configure_adc_clock&&c->alarm_mask==31&&c->critical_mask==31&&
 !memcmp(c->limits,thermal.config.limits,sizeof(c->limits))&&token->id==thermal.token.id&&token->owner==thermal.token.owner&&
 token->shared==thermal.token.shared&&token->exclusive==thermal.token.exclusive&&a733_clock_token_status(&providers.shared.clocks,&thermal.token)==A733_CLOCK_OK;
}
void thermal_cancel(void)
{atomic_store_explicit(&thermal.failed,1,memory_order_release);atomic_store_explicit(&thermal.result,2,memory_order_release);}
/* IRQ callbacks share the SMP kernel critical section with task rearm. They
 * never inspect driver fields, touch THS status or enter the platform mutex. */
void thermal_application_irq(void)
{
 UBaseType_t saved=taskENTER_CRITICAL_FROM_ISR();
 if(!thermal.masked){thermal.masked=1;
 if(!gicv3_spi_mask(&thermal.irq))thermal_cancel();}
 atomic_store_explicit(&thermal.pending,1,memory_order_release);
 taskEXIT_CRITICAL_FROM_ISR(saved);
}
static int mask(void)
{
 taskENTER_CRITICAL();int ok=1;
 if(!thermal.masked){thermal.masked=1;ok=gicv3_spi_mask(&thermal.irq);}
 taskEXIT_CRITICAL();
 if(!ok)thermal_cancel();return ok;
}
static int rearm(void)
{
 int ok=1;taskENTER_CRITICAL();
 if(live()&&!atomic_load_explicit(&thermal.pending,memory_order_acquire)&&thermal.masked){
  int r=gicv3_spi_rearm(&thermal.irq);
  if(r==1){thermal.masked=0;thermal.rearm_wait=0;}
  else if(r<0||++thermal.rearm_wait>=100)ok=0;
 }
 taskEXIT_CRITICAL();if(!ok)thermal_cancel();return ok;
}
int thermal_construct(void)
{
 if(!board_live(NULL)||board.session.deadline_us<=10000||!mappings())return 0;
 thermal.irq=(struct gicv3_spi){0x03400000,A733_THERMAL_INTID};thermal.masked=1;
 atomic_init(&thermal.failed,0);atomic_init(&thermal.pending,0);atomic_init(&thermal.result,0);
 thermal.config=(struct a733_thermal_config){.owner=thermal.session.owner,.handoff=thermal.session.authority,
 .startup_deadline_us=deadline(),.apb1=thermal.session.apb1,.apb1_hz=thermal.session.apb1_hz,
 .configure_adc_clock=thermal.session.configure_adc_clock,.alarm_mask=31,.critical_mask=31};
 for(unsigned i=0;i<5;++i)thermal.config.limits[i]=(struct a733_thermal_limits){thermal.session.limits[i][0],thermal.session.limits[i][1],thermal.session.limits[i][2]};
 a733_clock_mask_t shared=A733_CLOCK_FABRIC|A733_CLOCK_REF|A733_CLOCK_OSC;
 a733_clock_mask_t exclusive=A733_CLOCK_THS;
 if(thermal.config.configure_adc_clock)exclusive|=A733_CLOCK_GPADC24M;else shared|=A733_CLOCK_GPADC24M;
 if(thermal.config.apb1>>24==3)shared|=A733_CLOCK_PERI0;
 if(a733_clock_acquire(&providers.shared.clocks,thermal.session.owner,shared,exclusive,deadline(),&thermal.token)!=A733_CLOCK_OK)return 0;
 struct a733_thermal_ops ops={now,authorize,enter,leave,retain,read_register,write_register};
 storage_digest_bytes(&thermal.session,sizeof(thermal.session),thermal.session_digest);
 return a733_thermal_init(&thermal.driver,&thermal.config,&ops,&thermal,&providers.shared.clocks,&thermal.token);
}
int thermal_output_ready(void)
{return live()&&atomic_load_explicit(&thermal.result,memory_order_acquire)==1;}
int thermal_step(void)
{
 if(!owner()){thermal_cancel();if(!thermal.retaining)thermal.retaining=1;}
 if(thermal.retaining){(void)mask();if(thermal.retaining==1&&a733_clock_retain(&providers.shared.clocks,&thermal.token)!=A733_CLOCK_BUSY)thermal.retaining=2;return -1;}
 if(atomic_load_explicit(&thermal.result,memory_order_acquire)==1)return 1;
 if(!thermal.active){
  enum a733_thermal_result r=a733_thermal_poll(&thermal.driver);
  if(thermal.driver.fault){thermal_cancel();thermal.retaining=1;(void)mask();return -1;}
  if(r==A733_THERMAL_READY){thermal.active=1;memcpy(thermal.payload,&thermal.driver.calibration,sizeof(thermal.driver.calibration));}
  else if(r!=A733_THERMAL_PENDING&&r!=A733_THERMAL_BUSY){thermal_cancel();thermal.retaining=1;return -1;}
 }
 if(thermal.active){
  unsigned notified=atomic_exchange_explicit(&thermal.pending,0,memory_order_acq_rel);
  uint64_t n=a733_smp_counter()/24;
  if(notified||n>=thermal.next_read){
   struct a733_thermal_sample sample;
   enum a733_thermal_result r=a733_thermal_read(&thermal.driver,deadline(),&sample);
   if(thermal.driver.fault){thermal_cancel();thermal.retaining=1;(void)mask();return -1;}
   thermal.next_read=n+25000;
   if(r==A733_THERMAL_SAMPLE){if(!thermal_collect(&sample)){thermal_cancel();thermal.retaining=1;return -1;}}
   else if(r!=A733_THERMAL_AGAIN&&r!=A733_THERMAL_BUSY){thermal_cancel();thermal.retaining=1;return -1;}
  }
  if(thermal.sets==2){
   if(!mask()||!live()){thermal_cancel();thermal.retaining=1;return -1;}
   storage_digest_bytes(thermal.payload,sizeof(thermal.payload),thermal.digest);
   if(!live()){thermal_cancel();thermal.retaining=1;return -1;}
   atomic_store_explicit(&thermal.result,1,memory_order_release);return 1;
  }
  if(!rearm()){thermal.retaining=1;return -1;}
 }
 return 0;
}
void thermal_task(void *unused)
{(void)unused;for(;;){(void)thermal_step();vTaskDelay(1);}}
