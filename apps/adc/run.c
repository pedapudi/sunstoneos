#include "adc.h"
#include "boot.h"
#include "a733_storage_digest.h"
#include <string.h>
void adc_application_irq(void)
{
 UBaseType_t saved=taskENTER_CRITICAL_FROM_ISR();
 if(!adc.masked){adc.masked=1;
 if(!gicv3_spi_mask(&adc.irq))adc_cancel();}
 atomic_store_explicit(&adc.pending,1,memory_order_release);
 taskEXIT_CRITICAL_FROM_ISR(saved);
}
static int mask(void)
{
 taskENTER_CRITICAL();int ok=1;
 if(!adc.masked){adc.masked=1;ok=gicv3_spi_mask(&adc.irq);}
 taskEXIT_CRITICAL();
 if(!ok)adc_cancel();return ok;
}
static int rearm(void)
{
 int ok=1;taskENTER_CRITICAL();
 if(adc_live()&&!atomic_load_explicit(&adc.pending,memory_order_acquire)&&adc.masked){
  int r=gicv3_spi_rearm(&adc.irq);
  if(r==1){adc.masked=0;adc.rearm_wait=0;}
  else if(r<0||++adc.rearm_wait>=100)ok=0;
 }
 taskEXIT_CRITICAL();if(!ok)adc_cancel();return ok;
}
static int retain_failure(void)
{
 adc_cancel();(void)mask();
 /* The unwrapped adapter checks only actual owner/task/privilege. Never call
  * healthy platform/core code to manufacture a fault after cancellation. */
 if(adc.native.operations.current(&adc.native)!=1)return -1;
 if(adc.token.id&&!adc.clock_retained){
  if(a733_clock_retain(&providers.shared.clocks,&adc.token)!=A733_CLOCK_BUSY)adc.clock_retained=1;}
 if(adc.rail.id&&!adc.rail_retained&&a733_rail_retain(&providers.shared.rails,&adc.rail)!=A733_RAIL_BUSY)adc.rail_retained=1;
 return -1;
}
static int provider_ready(void)
{
 enum a733_pmic_status p=a733_pmic_live(&providers.shared.pmic,&adc.lifetime);
 if(p==A733_PMIC_BUSY||p==A733_PMIC_PENDING)return 0;
 if(p!=A733_PMIC_READY)return -1;
 enum a733_rail_result r=a733_rail_status(&providers.shared.rails,&adc.rail);
 return r==A733_RAIL_BUSY?0:r==A733_RAIL_READY?1:-1;
}
static int supply_valid(void)
{
 const struct a733_pmic_snapshot *s=&adc.supply;
 return s->rail==A733_RAIL_CLDO3&&!s->page_before&&s->chip==3&&!s->revision&&!s->page_after&&
 s->enabled==1&&s->decoded==1&&s->programmed_uv==adc.session.cldo3_uv&&(s->enable&32)&&(s->selector&31)==adc.session.cldo3_selector;
}
int adc_publish(void)
{
 uint32_t header[]={adc.supply.programmed_uv,adc.supply.selector&31,adc.session.version==2?2u:1u,adc.session.version==2?3u:1000u,2,adc.session.version==2?1u:15u,0,0};
 memcpy(adc.payload,header,sizeof(header));storage_digest_bytes(adc.payload,sizeof(adc.payload),adc.digest);
 unsigned empty=0;if(!adc_live()||!atomic_compare_exchange_strong_explicit(&adc.result,&empty,1,memory_order_release,memory_order_relaxed))return retain_failure();
 if(!adc_live())return retain_failure();return 1;
}
int adc_step(void)
{
 if(!adc_live())return retain_failure();
 if(atomic_load_explicit(&adc.result,memory_order_acquire)==1)return 1;
 enum a733_pmic_status p;enum a733_rail_result r;
 switch(adc.stage){
 case 0:
  if(!atomic_load_explicit(&providers.shared.pmic_ready,memory_order_acquire))return 0;
  p=a733_pmic_identity(&providers.shared.pmic,&adc.lifetime);
  if(p==A733_PMIC_BUSY)return 0;if(p!=A733_PMIC_READY)return retain_failure();adc.stage=1;return 0;
 case 1:
  r=a733_rail_acquire(&providers.shared.rails,adc.session.owner,A733_RAIL_BIT(A733_RAIL_CLDO3),0,adc.config.startup_deadline_us,&adc.rail);
  if(r==A733_RAIL_BUSY)return 0;if(r!=A733_RAIL_PENDING||!adc.rail.id)return retain_failure();adc.stage=2;return 0;
 case 2:
  r=a733_rail_poll(&providers.shared.rails,&adc.rail);
  if(r==A733_RAIL_PENDING||r==A733_RAIL_BUSY)return 0;if(r!=A733_RAIL_READY)return retain_failure();adc.stage=3;return 0;
 case 3:
  {int ready=provider_ready();if(ready<0)return retain_failure();if(!ready)return 0;}
  p=a733_pmic_submit(&providers.shared.pmic,adc.session.owner,&adc.rail,A733_RAIL_CLDO3,adc.config.startup_deadline_us,&adc.request);
  if(p==A733_PMIC_BUSY||(p==A733_PMIC_PENDING&&!adc.request.id))return 0;
  if(p!=A733_PMIC_PENDING||adc.request.owner!=adc.session.owner||adc.request.lifetime.provider!=adc.lifetime.provider||
   adc.request.lifetime.owner!=adc.lifetime.owner||adc.request.lifetime.identity!=adc.lifetime.identity){return retain_failure();}
  adc.stage=4;return 0;
 case 4:
  p=a733_pmic_result(&providers.shared.pmic,&adc.request,&adc.supply);
  if(p==A733_PMIC_BUSY||p==A733_PMIC_PENDING)return 0;
  if(p!=A733_PMIC_COMPLETE||!supply_valid())return retain_failure();adc.stage=5;return 0;
 case 5:{
  a733_clock_mask_t shared=A733_CLOCK_OSC|A733_CLOCK_REF|A733_CLOCK_FABRIC;
  a733_clock_mask_t exclusive=adc.session.version==2?A733_CLOCK_LRADC:A733_CLOCK_GPADC;
  if(adc.session.version==2)shared|=A733_CLOCK_RTC32K|A733_CLOCK_RC16M;
  else if(adc.session.configure_adc_clock)exclusive|=A733_CLOCK_GPADC24M;else shared|=A733_CLOCK_GPADC24M;
  if(adc.session.apb1>>24==3)shared|=A733_CLOCK_PERI0;
  enum a733_clock_result c=a733_clock_acquire(&providers.shared.clocks,adc.session.owner,shared,exclusive,adc.config.startup_deadline_us,&adc.token);
  if(c==A733_CLOCK_BUSY)return 0;if(c!=A733_CLOCK_OK)return retain_failure();adc.stage=6;return 0;}
 case 6:
  if(!(adc.session.version==2?a733_lradc_init(&adc.lradc,&adc.config,&adc.operations,&adc,&providers.shared.clocks,&adc.token):a733_adc_init(&adc.driver,&adc.config,&adc.operations,&adc,&providers.shared.clocks,&adc.token)))return retain_failure();adc.stage=7;return 0;
 case 7:{
  enum a733_adc_result v=adc.session.version==2?a733_lradc_poll(&adc.lradc):a733_adc_poll(&adc.driver);
  if(adc.session.version==2?adc.lradc.fault:adc.driver.fault)return retain_failure();if(v==A733_ADC_PENDING||v==A733_ADC_BUSY)return 0;
  if(v!=A733_ADC_READY)return retain_failure();adc.stage=8;return 0;}
 case 8:{
  unsigned notified=atomic_exchange_explicit(&adc.pending,0,memory_order_acq_rel);uint64_t n=a733_smp_counter()/24;
  if(notified||n>=adc.next_read){
   struct a733_adc_sample sample;enum a733_adc_result v=adc.session.version==2?a733_lradc_read(&adc.lradc,adc.config.startup_deadline_us,&sample):a733_adc_read(&adc.driver,adc.config.startup_deadline_us,&sample);
   if(adc.session.version==2?adc.lradc.fault:adc.driver.fault)return retain_failure();adc.next_read=n+25000;
   if(v==A733_ADC_SAMPLE){if(!adc_collect(&sample))return retain_failure();}
   else if(v!=A733_ADC_AGAIN&&v!=A733_ADC_BUSY)return retain_failure();
  }
  if(adc.sets==2){if(!mask())return retain_failure();adc.stage=9;return 0;}
  if(adc.session.version!=2&&!rearm())return retain_failure();return 0;}
 case 9:
  if(adc.session.version==2){enum a733_adc_result v=a733_lradc_stop(&adc.lradc,adc.config.startup_deadline_us);
   if(adc.lradc.fault)return retain_failure();if(v==A733_ADC_BUSY)return 0;if(v!=A733_ADC_STOPPED)return retain_failure();}
  if(adc.session.version==2){adc.stage=10;return 0;}
  /* GPADC preserves its original stage9 publication path. */
  /* fall through */
 case 10:{int ready=provider_ready();if(ready<0)return retain_failure();if(!ready)return 0;return adc_publish();}
 default:return retain_failure();
 }
}
void adc_task(void *context)
{(void)context;for(;;){adc_step();vTaskDelay(1);}}
