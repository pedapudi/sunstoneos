#include "monitor.h"
#include "boot.h"
#include <string.h>
int monitor_live(void)
{return board_live(NULL)&&a733_smp_counter()/24<board.session.deadline_us-10000&&
 !atomic_load_explicit(&providers.shared.clocks.failed,memory_order_acquire)&&
 !atomic_load_explicit(&providers.shared.locks.retained,memory_order_acquire)&&
 atomic_load_explicit(&monitor.state,memory_order_acquire)!=MONITOR_FAULT;}
void monitor_fail(void)
{atomic_store_explicit(&monitor.state,MONITOR_FAULT,memory_order_release);}
int monitor_output_eligible(void)
{return monitor_live()&&atomic_load_explicit(&monitor.state,memory_order_acquire)==MONITOR_RUNNING;}
static enum monitor_status status(unsigned state)
{return state==MONITOR_BOOT?MONITOR_STARTING:state==MONITOR_RUNNING?MONITOR_AGAIN:
 state==MONITOR_STOP_REQUEST?MONITOR_STOPPING:state==MONITOR_HALTED?MONITOR_STOPPED:MONITOR_FAILED;}
enum monitor_status monitor_request_stop(void)
{
 if(!monitor_live()){monitor_fail();return MONITOR_FAILED;}
 taskENTER_CRITICAL();unsigned s=atomic_load_explicit(&monitor.state,memory_order_acquire);
 if(s==MONITOR_RUNNING){uint64_t n=a733_smp_counter()/24,limit=board.session.deadline_us-10000;
  if(n>=limit){monitor_fail();s=MONITOR_FAULT;}else {
  monitor.stop_deadline=limit-n>1000000?n+1000000:limit;
  unsigned expected=MONITOR_RUNNING;
  if(atomic_compare_exchange_strong_explicit(&monitor.state,&expected,MONITOR_STOP_REQUEST,memory_order_acq_rel,memory_order_acquire))s=MONITOR_STOP_REQUEST;else s=expected;}}
 else if(s==MONITOR_BOOT){monitor_fail();s=MONITOR_FAULT;}
 taskEXIT_CRITICAL();return status(s);
}
enum monitor_status monitor_read_snapshot(struct monitor_snapshot *out,uint64_t after)
{
 uintptr_t p=(uintptr_t)out,m=(uintptr_t)&monitor;
 if(!out||sizeof(*out)>UINTPTR_MAX-p||!(p+sizeof(*out)<=m||m+sizeof(monitor)<=p))return MONITOR_FAILED;
 if(!monitor_live()){monitor_fail();return MONITOR_FAILED;}
 taskENTER_CRITICAL();unsigned s=atomic_load_explicit(&monitor.state,memory_order_acquire);
 if(!monitor_live()){monitor_fail();s=MONITOR_FAULT;}
 enum monitor_status r=status(s);
 if(s==MONITOR_RUNNING&&monitor.latest.thermal_sets&&monitor.latest.adc_sets&&monitor.latest.generation>after){
  struct monitor_snapshot copy=monitor.latest;
  if(monitor_output_eligible()){*out=copy;r=MONITOR_SNAPSHOT;}else r=MONITOR_FAILED;}
 taskEXIT_CRITICAL();
 return r;
}
static int publish(unsigned kind)
{
 struct monitor_owner *o=&monitor.owners[kind];
 taskENTER_CRITICAL();unsigned state=atomic_load_explicit(&monitor.state,memory_order_acquire);
 int ok=state==MONITOR_RUNNING||state==MONITOR_BOOT;
 if(ok){struct monitor_snapshot *s=&monitor.latest;
  if(s->generation==UINT64_MAX||(kind?s->thermal_sets:s->adc_sets)==UINT64_MAX)ok=0;
  else {++s->generation;s->observed_us=a733_smp_counter()/24;s->state=MONITOR_RUNNING;
   if(kind){++s->thermal_sets;s->thermal_sequence=o->last_sequence;
    s->alarm|=o->alarm;s->alarm_off|=o->alarm_off;s->critical|=o->critical;
    memcpy(s->thermal_times,o->times,sizeof(s->thermal_times));memcpy(s->thermal_codes,o->codes,sizeof(s->thermal_codes));
    memcpy(s->thermal_millicelsius,o->temperatures,sizeof(s->thermal_millicelsius));}
   else {++s->adc_sets;s->adc_sequence=o->last_sequence;
    memcpy(s->adc_times,o->times,sizeof(s->adc_times));memcpy(s->adc_codes,o->codes,sizeof(s->adc_codes));}}}
 if(state==MONITOR_STOP_REQUEST)ok=1; /* Discard the admitted read after stop. */
 taskEXIT_CRITICAL();return ok;
}
int monitor_collect_adc(const struct a733_adc_sample *s)
{
 struct monitor_owner *o=&monitor.owners[0];
 if(!s||!s->valid||(s->valid&~15u)||s->sequence<=o->last_sequence)return 0;
 o->last_sequence=s->sequence;
 for(unsigned i=0;i<4;++i)if(s->valid&(1u<<i)){
  if(s->code[i]>4095||!s->observed_us[i]||s->observed_us[i]<=o->times[i])return 0;
  o->times[i]=s->observed_us[i];o->codes[i]=s->code[i];o->valid|=1u<<i;}
 if(o->valid==15){o->valid=0;return publish(0);}return 1;
}
int monitor_collect_thermal(const struct a733_thermal_sample *s)
{
 struct monitor_owner *o=&monitor.owners[1];
 if(!s||(s->valid&~31u)||((s->alarm|s->alarm_off|s->critical)&~31u)||
 !(s->valid|s->alarm|s->alarm_off|s->critical)||s->sequence<=o->last_sequence||!s->observed_us)return 0;
 o->last_sequence=s->sequence;
 for(unsigned i=0;i<5;++i)if(s->valid&(1u<<i)){
  if(!s->code[i]||s->code[i]>4095||s->observed_us<=o->times[i])return 0;
  o->times[i]=s->observed_us;o->codes[i]=s->code[i];o->temperatures[i]=s->millicelsius[i];o->valid|=1u<<i;}
 o->alarm|=s->alarm;o->alarm_off|=s->alarm_off;o->critical|=s->critical;
 if(o->valid==31){o->valid=0;return publish(1);}return 1;
}
