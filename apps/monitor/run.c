#include "monitor.h"
#include "boot.h"
#include <string.h>
static int mask(struct monitor_owner *o)
{
 taskENTER_CRITICAL();int ok=1;
 if(!o->masked){o->masked=1;ok=gicv3_spi_mask(&o->irq);}
 taskEXIT_CRITICAL();if(!ok)monitor_fail();return ok;
}
void monitor_application_irq(unsigned kind)
{
 if(kind>1){monitor_fail();return;}struct monitor_owner *o=&monitor.owners[kind];
 UBaseType_t saved=taskENTER_CRITICAL_FROM_ISR();
 if(!o->masked){o->masked=1;if(!gicv3_spi_mask(&o->irq))monitor_fail();}
 atomic_store_explicit(&o->pending,1,memory_order_release);
 taskEXIT_CRITICAL_FROM_ISR(saved);
}
static int rearm(struct monitor_owner *o)
{
 int ok=1;taskENTER_CRITICAL();unsigned state=atomic_load_explicit(&monitor.state,memory_order_acquire);
 if((state==MONITOR_BOOT||state==MONITOR_RUNNING)&&monitor_live()&&
 !atomic_load_explicit(&o->pending,memory_order_acquire)&&o->masked){
  int r=gicv3_spi_rearm(&o->irq);if(r==1){o->masked=0;o->rearm_wait=0;}
  else if(r<0||++o->rearm_wait>=100)ok=0;}
 taskEXIT_CRITICAL();if(!ok)monitor_fail();return ok;
}
static int failure(struct monitor_owner *o)
{
 monitor_fail();(void)mask(o);
 if(o->native.operations.current(&o->native)!=1)return -1;
 if(o->token.id&&!o->retained&&a733_clock_retain(&providers.shared.clocks,&o->token)!=A733_CLOCK_BUSY)o->retained=1;
 if(!o->kind){
  if(monitor.parent.id&&!monitor.parent_retained&&a733_clock_retain(&providers.shared.clocks,&monitor.parent)!=A733_CLOCK_BUSY)monitor.parent_retained=1;
  if(monitor.rail.id&&!monitor.rail_retained&&a733_rail_retain(&providers.shared.rails,&monitor.rail)!=A733_RAIL_BUSY)monitor.rail_retained=1;}
 return -1;
}
static int supply_health(void)
{
 enum a733_pmic_status p=a733_pmic_live(&providers.shared.pmic,&monitor.lifetime);
 if(p==A733_PMIC_BUSY||p==A733_PMIC_PENDING)return 0;if(p!=A733_PMIC_READY)return -1;
 enum a733_rail_result r=a733_rail_status(&providers.shared.rails,&monitor.rail);
 return r==A733_RAIL_BUSY?0:r==A733_RAIL_READY?1:-1;
}
static int supply(void)
{
 enum a733_pmic_status p;enum a733_rail_result r;uint64_t d=monitor.adc_config.startup_deadline_us;
 switch(monitor.supply_stage){
 case 0:
  if(!atomic_load_explicit(&providers.shared.pmic_ready,memory_order_acquire))return 0;
  p=a733_pmic_identity(&providers.shared.pmic,&monitor.lifetime);
  if(p==A733_PMIC_BUSY)return 0;if(p!=A733_PMIC_READY)return -1;monitor.supply_stage=1;return 0;
 case 1:
  r=a733_rail_acquire(&providers.shared.rails,monitor.session.owner,A733_RAIL_BIT(A733_RAIL_CLDO3),0,d,&monitor.rail);
  if(r==A733_RAIL_BUSY)return 0;if(r!=A733_RAIL_PENDING||!monitor.rail.id)return -1;monitor.supply_stage=2;return 0;
 case 2:
  r=a733_rail_poll(&providers.shared.rails,&monitor.rail);
  if(r==A733_RAIL_PENDING||r==A733_RAIL_BUSY)return 0;if(r!=A733_RAIL_READY)return -1;monitor.supply_stage=3;return 0;
 case 3:
  {int ready=supply_health();if(ready<=0)return ready;}
  p=a733_pmic_submit(&providers.shared.pmic,monitor.session.owner,&monitor.rail,A733_RAIL_CLDO3,d,&monitor.request);
  if(p==A733_PMIC_BUSY||(p==A733_PMIC_PENDING&&!monitor.request.id))return 0;
  if(p!=A733_PMIC_PENDING||monitor.request.owner!=monitor.session.owner||
   memcmp(&monitor.request.lifetime,&monitor.lifetime,sizeof(monitor.lifetime)))return -1;
  monitor.supply_stage=4;return 0;
 case 4:
  p=a733_pmic_result(&providers.shared.pmic,&monitor.request,&monitor.supply);
  if(p==A733_PMIC_BUSY||p==A733_PMIC_PENDING)return 0;
  if(p!=A733_PMIC_COMPLETE)return -1;
  {const struct a733_pmic_snapshot *s=&monitor.supply;
   if(s->rail!=A733_RAIL_CLDO3||s->page_before||s->chip!=3||s->revision||s->page_after||
    s->enabled!=1||s->decoded!=1||s->programmed_uv!=monitor.session.cldo3_uv||
    !(s->enable&32)||(s->selector&31)!=monitor.session.cldo3_selector)return -1;}
  monitor.latest.cldo3_uv=monitor.supply.programmed_uv;monitor.supply_stage=5;return 0;
 default:return supply_health();
 }
}
static int owner_step(struct monitor_owner *o)
{
 if(!o->stage){
  if(!o->kind){int r=supply();if(r<=0)return r;
   if(!atomic_load_explicit(&monitor.parent_ready,memory_order_acquire)){r=monitor_parent_step();if(r<=0)return r;}}
  else if(!atomic_load_explicit(&monitor.parent_ready,memory_order_acquire))return 0;
  o->stage=1;return 0;
 }
 if(!o->kind){int health=supply_health();if(health<=0)return health;}
 if(o->stage==1){
  a733_clock_mask_t shared=A733_CLOCK_REF|A733_CLOCK_OSC|A733_CLOCK_FABRIC|A733_CLOCK_GPADC24M;
  if(monitor.session.apb1>>24==3)shared|=A733_CLOCK_PERI0;
  enum a733_clock_result r=a733_clock_acquire(&providers.shared.clocks,o->kind?monitor.session.thermal_owner:monitor.session.owner,
   shared,o->kind?A733_CLOCK_THS:A733_CLOCK_GPADC,monitor.adc_config.startup_deadline_us,&o->token);
  if(r==A733_CLOCK_BUSY)return 0;if(r!=A733_CLOCK_OK)return -1;o->stage=2;return 0;}
 if(o->stage==2){
  int ok=o->kind?a733_thermal_init(&monitor.thermal,&monitor.thermal_config,&monitor.thermal_ops,o,&providers.shared.clocks,&o->token):
   a733_adc_init(&monitor.adc,&monitor.adc_config,&monitor.adc_ops,o,&providers.shared.clocks,&o->token);
  if(!ok)return -1;o->stage=3;return 0;}
 if(o->stage==3){
  int ready=0;
  if(o->kind){enum a733_thermal_result r=a733_thermal_poll(&monitor.thermal);
   if(monitor.thermal.fault)return -1;if(r==A733_THERMAL_READY)ready=1;else if(r!=A733_THERMAL_BUSY&&r!=A733_THERMAL_PENDING)return -1;}
  else {enum a733_adc_result r=a733_adc_poll(&monitor.adc);
   if(monitor.adc.fault)return -1;if(r==A733_ADC_READY)ready=1;else if(r!=A733_ADC_BUSY&&r!=A733_ADC_PENDING)return -1;}
  if(!ready)return 0;o->stage=4;
  taskENTER_CRITICAL();
  if(o->kind){const struct a733_thermal_calibration *c=&monitor.thermal.calibration;
   monitor.latest.factory_tenth_c=c->factory_tenth_c;
   memcpy(monitor.latest.factory_codes,c->factory_code,sizeof(c->factory_code));memcpy(monitor.latest.corrections,c->correction,sizeof(c->correction));}
  unsigned active=atomic_fetch_or_explicit(&monitor.active_mask,1u<<o->kind,memory_order_acq_rel)|(1u<<o->kind);
  if(active==3){unsigned expected=MONITOR_BOOT;
   (void)atomic_compare_exchange_strong_explicit(&monitor.state,&expected,MONITOR_RUNNING,memory_order_acq_rel,memory_order_acquire);}
  taskEXIT_CRITICAL();return 0;
 }
 unsigned pending=atomic_exchange_explicit(&o->pending,0,memory_order_acq_rel);uint64_t now=a733_smp_counter()/24;
 if(pending||now>=o->next_read){
  o->next_read=now+25000;
  if(o->kind){struct a733_thermal_sample s;enum a733_thermal_result r=a733_thermal_read(&monitor.thermal,monitor.thermal_config.startup_deadline_us,&s);
   if(monitor.thermal.fault)return -1;
   if(r==A733_THERMAL_SAMPLE){if(monitor_output_eligible()&&!monitor_collect_thermal(&s))return -1;}
   else if(r!=A733_THERMAL_AGAIN&&r!=A733_THERMAL_BUSY)return -1;}
  else {struct a733_adc_sample s;enum a733_adc_result r=a733_adc_read(&monitor.adc,monitor.adc_config.startup_deadline_us,&s);
   if(monitor.adc.fault)return -1;
   if(r==A733_ADC_SAMPLE){if(monitor_output_eligible()&&!monitor_collect_adc(&s))return -1;}
   else if(r!=A733_ADC_AGAIN&&r!=A733_ADC_BUSY)return -1;}}
 return rearm(o)?0:-1;
}
static int stop_owner(struct monitor_owner *o)
{
 if(a733_smp_counter()/24>=monitor.stop_deadline||!mask(o))return -1;
 if(o->stopped)return 1;
 int stopped=0;
 if(o->kind){enum a733_thermal_result r=a733_thermal_stop(&monitor.thermal,monitor.stop_deadline);
  if(monitor.thermal.fault)return -1;if(r==A733_THERMAL_STOPPED)stopped=1;else if(r!=A733_THERMAL_BUSY)return -1;}
 else {enum a733_adc_result r=a733_adc_stop(&monitor.adc,monitor.stop_deadline);
  if(monitor.adc.fault)return -1;if(r==A733_ADC_STOPPED)stopped=1;else if(r!=A733_ADC_BUSY)return -1;}
 if(!stopped)return 0;
 taskENTER_CRITICAL();o->stopped=1;
 if(monitor_live()&&a733_smp_counter()/24<monitor.stop_deadline&&monitor.owners[0].stopped&&monitor.owners[1].stopped&&
 atomic_load_explicit(&monitor.state,memory_order_acquire)==MONITOR_STOP_REQUEST)
  {unsigned expected=MONITOR_STOP_REQUEST;
   (void)atomic_compare_exchange_strong_explicit(&monitor.state,&expected,MONITOR_HALTED,memory_order_acq_rel,memory_order_acquire);}
 taskEXIT_CRITICAL();return 1;
}
int monitor_step(unsigned kind)
{
 if(kind>1)return -1;struct monitor_owner *o=&monitor.owners[kind];
 if(o->native.operations.current(&o->native)!=1){monitor_fail();return -1;}
 if(!monitor_live())return failure(o);
 taskENTER_CRITICAL();unsigned state=atomic_load_explicit(&monitor.state,memory_order_acquire);
 if(state==MONITOR_HALTED){taskEXIT_CRITICAL();return 1;}
 if(state==MONITOR_FAULT){taskEXIT_CRITICAL();return failure(o);}
 o->in_call=1;taskEXIT_CRITICAL();
 int r=state==MONITOR_STOP_REQUEST?stop_owner(o):owner_step(o);
 taskENTER_CRITICAL();o->in_call=0;taskEXIT_CRITICAL();
 if(r<0)return failure(o);
 if(!monitor_live())return failure(o);
 return r;
}
void monitor_task(void *v)
{struct monitor_owner *o=v;for(;;){(void)monitor_step(o->kind);vTaskDelay(1);}}
