#include "spi.h"
#include "boot.h"
#include "a733_storage_digest.h"
#include <string.h>
static int active(void)
{return spi_live()&&a733_smp_counter()/24<spi.deadline;}
static int retain_failure(void)
{
 spi_cancel();
 /* The unwrapped adapter checks only actual owner/task/privilege. Never call
  * healthy platform/core code to manufacture a fault after cancellation. */
 if(spi.native.operations.current(&spi.native)!=1)return -1;
 if(spi.platform.clock.id&&!spi.clock_retained){
  if(spi.platform.retained)spi.clock_retained=1;
  else if(a733_clock_retain(&providers.shared.clocks,&spi.platform.clock)!=A733_CLOCK_BUSY)spi.clock_retained=1;}
 if(spi.rail.id&&!spi.rail_retained&&a733_rail_retain(&providers.shared.rails,&spi.rail)!=A733_RAIL_BUSY)spi.rail_retained=1;
 return -1;
}
static int provider_ready(void)
{
 enum a733_pmic_status p=a733_pmic_live(&providers.shared.pmic,&spi.lifetime);
 if(p==A733_PMIC_BUSY||p==A733_PMIC_PENDING)return 0;
 if(p!=A733_PMIC_READY)return -1;
 enum a733_rail_result r=a733_rail_status(&providers.shared.rails,&spi.rail);
 return r==A733_RAIL_BUSY?0:r==A733_RAIL_READY?1:-1;
}
static int supply_valid(void)
{
 const struct a733_pmic_snapshot *s=&spi.supply;
 return s->rail==A733_RAIL_BLDO1&&!s->page_before&&s->chip==3&&!s->revision&&!s->page_after&&
 s->enabled==1&&s->decoded==1&&s->programmed_uv==1800000&&(s->enable&64)&&(s->selector&31)==13;
}
int spi_publish(void)
{
 memset(spi.payload,0,sizeof(spi.payload));uint64_t observed=a733_smp_counter()/24;
 memcpy(spi.payload,&observed,8);uint32_t values[]={spi.platform.spi.plan.actual_hz,spi.session.tx_bytes,spi.session.rx_bytes,A733_SPI_WRITE_READ,3,0};
 memcpy(spi.payload+8,values,sizeof(values));memcpy(spi.payload+32,spi.tx,spi.session.tx_bytes);memcpy(spi.payload+64,spi.rx,spi.session.rx_bytes);
 storage_digest_bytes(spi.payload,sizeof(spi.payload),spi.digest);
 unsigned empty=0;
 if(!active()||!atomic_compare_exchange_strong_explicit(&spi.result,&empty,1,memory_order_release,memory_order_relaxed))return retain_failure();
 if(!active())return retain_failure();return 1;
}
int spi_step(void)
{
 if(!spi_live())return retain_failure();
 /* A completed report is historical; the operation deadline bounds work,
  * while the independent board lifetime bounds later copied observation. */
 if(atomic_load_explicit(&spi.result,memory_order_acquire)==1)return 1;
 if(!active())return retain_failure();
 enum a733_spi_platform_result s;enum a733_pmic_status p;enum a733_rail_result r;
 switch(spi.stage){
 case 0:
  if(!atomic_load_explicit(&providers.shared.pmic_ready,memory_order_acquire))return 0;
  p=a733_pmic_identity(&providers.shared.pmic,&spi.lifetime);
  if(p==A733_PMIC_BUSY)return 0;if(p!=A733_PMIC_READY)return retain_failure();spi.stage=1;return 0;
 case 1:
  r=a733_rail_acquire(&providers.shared.rails,spi.session.owner,A733_RAIL_BIT(A733_RAIL_BLDO1),0,spi.deadline,&spi.rail);
  if(r==A733_RAIL_BUSY)return 0;if(r!=A733_RAIL_PENDING||!spi.rail.id)return retain_failure();spi.stage=2;return 0;
 case 2:
  r=a733_rail_poll(&providers.shared.rails,&spi.rail);
  if(r==A733_RAIL_PENDING||r==A733_RAIL_BUSY)return 0;if(r!=A733_RAIL_READY)return retain_failure();spi.stage=3;return 0;
 case 3:
  {int ready=provider_ready();if(ready<0)return retain_failure();if(!ready)return 0;}
  p=a733_pmic_submit(&providers.shared.pmic,spi.session.owner,&spi.rail,A733_RAIL_BLDO1,spi.deadline,&spi.request);
  if(p==A733_PMIC_BUSY||(p==A733_PMIC_PENDING&&!spi.request.id))return 0;
  if(p!=A733_PMIC_PENDING||spi.request.owner!=spi.session.owner||spi.request.lifetime.provider!=spi.lifetime.provider||
   spi.request.lifetime.owner!=spi.lifetime.owner||spi.request.lifetime.identity!=spi.lifetime.identity)return retain_failure();spi.stage=4;return 0;
 case 4:
  p=a733_pmic_result(&providers.shared.pmic,&spi.request,&spi.supply);
  if(p==A733_PMIC_BUSY||p==A733_PMIC_PENDING)return 0;
  if(p!=A733_PMIC_COMPLETE||!supply_valid())return retain_failure();spi.stage=5;return 0;
 case 5:
  {int ready=provider_ready();if(ready<0)return retain_failure();if(!ready)return 0;}
  s=a733_spi_platform_poll(&spi.platform,32);
  if(spi.platform.fault||spi.platform.spi.state==A733_SPI_FAULT)return retain_failure();
  if(s==A733_SPI_PLATFORM_PENDING||s==A733_SPI_PLATFORM_BUSY)return 0;
  if(s!=A733_SPI_PLATFORM_READY)return retain_failure();spi.stage=6;return 0;
 case 6:
  {int ready=provider_ready();if(ready<0)return retain_failure();if(!ready)return 0;}
  s=a733_spi_platform_submit(&spi.platform,&spi.transfer,spi.deadline);
  if(spi.platform.fault||spi.platform.spi.state==A733_SPI_FAULT)return retain_failure();
  if(s==A733_SPI_PLATFORM_BUSY)return 0;
  if(s!=A733_SPI_PLATFORM_PENDING)return retain_failure();spi.stage=7;return 0;
 case 7:
  s=a733_spi_platform_poll(&spi.platform,32);
  if(spi.platform.fault||spi.platform.spi.state==A733_SPI_FAULT)return retain_failure();
  if(s==A733_SPI_PLATFORM_PENDING||s==A733_SPI_PLATFORM_BUSY)return 0;
  if(s!=A733_SPI_PLATFORM_COMPLETE)return retain_failure();spi.stage=8;return 0;
 case 8:
  s=a733_spi_platform_reap(&spi.platform,spi.deadline);
  if(spi.platform.fault||spi.platform.spi.state==A733_SPI_FAULT)return retain_failure();
  if(s==A733_SPI_PLATFORM_BUSY)return 0;
  if(s!=A733_SPI_PLATFORM_READY)return retain_failure();spi.stage=9;return 0;
 case 9:
  {int ready=provider_ready();if(ready<0)return retain_failure();if(!ready)return 0;}
  return spi_publish();
 default:return retain_failure();
 }
}
void spi_task(void *context)
{(void)context;for(;;){spi_step();vTaskDelay(1);}}
