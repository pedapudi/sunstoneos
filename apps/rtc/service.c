#include "service-port.h"
#include "queue.h"
#include "a733_rtc_application.h"
#include <stdatomic.h>
#include <string.h>
struct request_slot {
 TaskHandle_t client;
 uint64_t id;
 struct rtc_request request;
 struct rtc_service_result result;
 unsigned state; /* 0 free,1 queued,2 active,3 terminal */
 atomic_uint cancel;
};
static struct {
 struct request_slot slots[2];
 StaticQueue_t queue_storage;
 uint8_t queue_bytes[2*sizeof(unsigned)];
 QueueHandle_t queue;
 uint64_t next;
 struct a733_rtc driver;
 struct rtc_service_output output;
 uint8_t digest[32];
 atomic_uint ready,poisoned,done,phase,alarm_active;
 unsigned active,attempted,alarm_client,alarm_finishing;
 struct rtc_service_result alarm_result;
 uint32_t seed_day,seed_hms,alarm_seconds;
} service;
static int client(void)
{
 TaskHandle_t caller=xTaskGetCurrentTaskHandle();
 for(unsigned i=0;i<2;++i)if(caller&&caller==service.slots[i].client)return (int)i;
 return -1;
}
static int valid(const struct rtc_request *r)
{
 if(!r)return 0;
 if(r->kind==RTC_REQUEST_READ)return !r->day&&!r->hms&&!r->seconds;
 if(r->kind==RTC_REQUEST_ALARM)return !r->day&&!r->hms&&r->seconds&&r->seconds<=service.alarm_seconds;
 struct a733_rtc_setting setting={A733_RTC_RUNTIME_CALENDAR,r->day,r->hms,0};
 return r->kind==RTC_REQUEST_SET&&!r->seconds&&a733_rtc_setting_valid(&setting);
}
int rtc_service_submit(const struct rtc_request *r,uint64_t *id)
{
 if(id)*id=0;
 int c=client();
 if(c<0||!id||!valid(r)||!rtc_service_live())return 2;
 int result=2;
 taskENTER_CRITICAL();
 if(atomic_load_explicit(&service.ready,memory_order_acquire)&&
    !atomic_load_explicit(&service.poisoned,memory_order_acquire)&&service.next!=UINT64_MAX){
  struct request_slot *s=&service.slots[c];result=0;
  if(!s->state){
   s->request=*r;s->id=++service.next;s->state=1;
   atomic_store_explicit(&s->cancel,0,memory_order_release);
   unsigned index=(unsigned)c;
   /* Zero-time enqueue cannot wait. Slot reservation and publication form one
    * transition; only a proven failed enqueue may roll the reservation back. */
   if(xQueueSend(service.queue,&index,0)==pdPASS){*id=s->id;result=1;}
   else{s->state=0;s->id=0;}
  }
 }
 taskEXIT_CRITICAL();return result;
}
int rtc_service_copy(uint64_t id,struct rtc_service_result *out)
{
 if(!out)return 2;memset(out,0,sizeof(*out));int c=client(),result=2;
 if(c<0||!id||!rtc_service_live())return 2;
 taskENTER_CRITICAL();struct request_slot *s=&service.slots[c];
 if(s->state&&s->id==id){result=0;if(s->state==3){*out=s->result;result=1;}}
 taskEXIT_CRITICAL();
 if(!rtc_service_live()){memset(out,0,sizeof(*out));return 2;}return result;
}
int rtc_service_release(uint64_t id)
{
 int c=client(),result=0;if(c<0||!id||!rtc_service_live())return 0;
 taskENTER_CRITICAL();struct request_slot *s=&service.slots[c];
 if(s->state==3&&s->id==id){s->state=0;s->id=0;result=1;}
 taskEXIT_CRITICAL();return result;
}
int rtc_service_cancel(uint64_t id)
{
 int c=client(),result=0;if(c<0||!id||!rtc_service_live())return 0;
 taskENTER_CRITICAL();struct request_slot *s=&service.slots[c];
 if((s->state==1||s->state==2)&&s->id==id){atomic_store_explicit(&s->cancel,1,memory_order_release);result=1;}
 taskEXIT_CRITICAL();return result;
}
int rtc_service_active_cancel(void)
{return service.active<2&&atomic_load_explicit(&service.slots[service.active].cancel,memory_order_acquire);}
int rtc_service_first_attempt(void){return !service.attempted;}
void rtc_service_note_attempt(void){service.attempted=1;}
static int cancelled(void *unused){(void)unused;return rtc_service_active_cancel();}
static int pause_until(void *unused,uint64_t until)
{
 (void)unused;uint64_t now;
 if(!rtc_service_now(NULL,&now))return 0;
 if(now<until)vTaskDelay(1);
 return rtc_service_live();
}
static void publish(unsigned c,struct rtc_service_result *result)
{
 if(!rtc_service_live())return;
 taskENTER_CRITICAL();
 if(atomic_load_explicit(&service.done,memory_order_acquire)!=3&&service.output.count<RTC_SERVICE_RECORDS){
  service.output.records[service.output.count++]=*result;
 }else if(atomic_load_explicit(&service.done,memory_order_acquire)!=3)++service.output.omitted;
 service.slots[c].result=*result;service.slots[c].state=3;
 taskEXIT_CRITICAL();
}
static void retain(unsigned c,struct rtc_service_result *result)
{
 atomic_store_explicit(&service.poisoned,1,memory_order_release);
 if(atomic_load_explicit(&service.done,memory_order_acquire)!=3)service.output.retained=1;result->outcome=RTC_REQUEST_RETAINED;publish(c,result);
}
static int idle(void)
{
 /* Reinitializing private state is permitted only after source/GIC quiescence.
  * open verifies disabled/unpending source/output and captures restored target. */
 if(!rtc_service_mask()||!rtc_service_quiet(1)||rtc_service_pending()||rtc_service_irq_fault())return 0;
 memset(&service.driver,0,sizeof(service.driver));
 struct a733_rtc_ops ops={rtc_service_now,rtc_service_read,rtc_service_write};
 return a733_rtc_open(&service.driver,&ops,NULL,rtc_service_deadline());
}
static int alarm_finish(void)
{
 unsigned c=service.alarm_client;service.active=c;
 struct request_slot *slot=&service.slots[c];
 if(!rtc_service_mask())return -1;
 if(!service.alarm_finishing){
 if(!rtc_service_quiet(0))return 0;
 if((rtc_service_pending()&&!a733_rtc_irq(&service.driver))||
    !a733_rtc_finish(&service.driver,atomic_load_explicit(&slot->cancel,memory_order_acquire)))return -1;
 /* IRQ notification is consumed only after mask/inactive and owned source
  * disable/W1C/restore. No new owned interrupt can now publish a notification. */
 rtc_service_armed(0);rtc_service_clear_pending();
 service.alarm_result=(struct rtc_service_result){.id=slot->id,.client=c,.kind=RTC_REQUEST_ALARM,
   .outcome=service.driver.snapshot.alarm_seen?RTC_REQUEST_OK:RTC_REQUEST_CANCELLED,
   .time=service.driver.snapshot.terminal,.alarm=service.driver.snapshot};
 service.alarm_finishing=1;
 }
 /* Source completion is irreversible. Poll only GIC propagation, never run
  * finish again on a FIRED/CANCELLED private driver. */
 if(!rtc_service_quiet(1))return 0;
 if(rtc_service_pending()||rtc_service_irq_fault()||!idle())return -1;
 atomic_store_explicit(&service.alarm_active,0,memory_order_release);
 publish(c,&service.alarm_result);service.alarm_finishing=0;service.active=2;return 1;
}
static int alarm_waiting(void)
{
 if(!atomic_load_explicit(&service.alarm_active,memory_order_acquire))return 0;
 return service.alarm_finishing||rtc_service_pending()||atomic_load_explicit(&service.slots[service.alarm_client].cancel,memory_order_acquire);
}
int rtc_service_prepare(TaskHandle_t clients[2],uint32_t day,uint32_t hms,unsigned seconds)
{
 struct a733_rtc_setting seed={A733_RTC_RUNTIME_CALENDAR,day,hms,0};
 if(!a733_rtc_setting_valid(&seed)||service.queue||!clients||!clients[0]||!clients[1]||clients[0]==clients[1]||day>=65535||!seconds||seconds>10)return 0;
 service.queue=xQueueCreateStatic(2,sizeof(unsigned),service.queue_bytes,&service.queue_storage);
 if(!service.queue)return 0;
 service.slots[0].client=clients[0];service.slots[1].client=clients[1];
 service.seed_day=day;service.seed_hms=hms;service.alarm_seconds=seconds;service.active=2;
 return 1;
}
void rtc_service_task(void)
{
 atomic_store_explicit(&service.ready,1,memory_order_release);
 /* The initial calendar may need the first client's repair. No alarm/read can
  * run until that request completes; other native clients wait for its phase. */
 while(rtc_service_live()){
  if(rtc_service_irq_fault())break;
  if(atomic_load_explicit(&service.poisoned,memory_order_acquire)){
   /* No further RTC accesses or successful operations after uncertainty. Wake
    * every queued/active client with its own retained generation before freeze. */
   (void)rtc_service_mask();
   for(unsigned i=0;i<2;++i)if(service.slots[i].state==1||service.slots[i].state==2){
    struct rtc_service_result r={.id=service.slots[i].id,.client=i,
       .kind=service.slots[i].request.kind,.outcome=RTC_REQUEST_RETAINED};publish(i,&r);
   }
   if(atomic_load_explicit(&service.done,memory_order_acquire)==3){for(;;){if(!rtc_service_live())rtc_service_fault();vTaskDelay(1);}}
   service.output.retained=1;service.output.complete=0;
   rtc_service_hash(&service.output,sizeof(service.output),service.digest);
   if(rtc_service_live()){atomic_store_explicit(&service.done,3,memory_order_release);rtc_service_terminal(&service.output);}
   for(;;){if(!rtc_service_live())rtc_service_fault();vTaskDelay(1);}
  }
  if(atomic_load_explicit(&service.done,memory_order_acquire)==2){
   service.output.complete=1;rtc_service_hash(&service.output,sizeof(service.output),service.digest);
   if(!rtc_service_live())break;
   atomic_store_explicit(&service.done,3,memory_order_release);rtc_service_terminal(&service.output);
  }
  if(alarm_waiting()){
   int finished=alarm_finish();
   if(finished<0){struct rtc_service_result r={.id=service.slots[service.alarm_client].id,
      .client=service.alarm_client,.kind=RTC_REQUEST_ALARM};retain(service.alarm_client,&r);}
   if(finished<=0){vTaskDelay(1);continue;}
  }
  unsigned c;
  if(xQueueReceive(service.queue,&c,1)!=pdPASS)continue;
  if(c>=2)break;
  taskENTER_CRITICAL();struct request_slot *slot=&service.slots[c];
  if(slot->state!=1){taskEXIT_CRITICAL();break;}slot->state=2;
  struct rtc_request request=slot->request;uint64_t id=slot->id;taskEXIT_CRITICAL();
  service.active=c;service.attempted=0;
  struct rtc_service_result result={.id=id,.client=c,.kind=request.kind};
  if(!rtc_service_live())break;
  if(rtc_service_active_cancel())result.outcome=RTC_REQUEST_CANCELLED;
  else if(atomic_load_explicit(&service.alarm_active,memory_order_acquire)&&request.kind!=RTC_REQUEST_READ)
   result.outcome=RTC_REQUEST_BUSY;
  else if(request.kind==RTC_REQUEST_SET){
   struct a733_rtc_setting setting={A733_RTC_RUNTIME_CALENDAR,request.day,request.hms,0};
   struct a733_rtc_ops ops={rtc_service_now,rtc_service_read,rtc_service_write};
   if(!rtc_service_mask()||!rtc_service_quiet(1)||rtc_service_pending()||
      !a733_rtc_set_calendar(&setting,&ops,NULL,rtc_service_deadline(),pause_until,cancelled,&result.calendar)){
    retain(c,&result);service.active=2;continue;
   }
   unsigned outcome=result.calendar.outcome;
   result.outcome=outcome==A733_RTC_CALENDAR_APPLIED?RTC_REQUEST_OK:
      outcome==A733_RTC_CALENDAR_CANCELLED?RTC_REQUEST_CANCELLED:
      outcome==A733_RTC_CALENDAR_APPLIED_LATE_CANCEL?RTC_REQUEST_LATE_CANCEL:RTC_REQUEST_RETAINED;
   if(result.outcome==RTC_REQUEST_RETAINED){retain(c,&result);service.active=2;continue;}
   if(result.outcome!=RTC_REQUEST_CANCELLED){
    if(!idle()||!a733_rtc_read(&service.driver,&result.time)){retain(c,&result);service.active=2;continue;}
   }
  }else{
   if(!atomic_load_explicit(&service.alarm_active,memory_order_acquire)&&!idle()){
    retain(c,&result);service.active=2;continue;
   }
   if(request.kind==RTC_REQUEST_READ){
    if(!a733_rtc_read(&service.driver,&result.time)){retain(c,&result);service.active=2;continue;}
    result.outcome=RTC_REQUEST_OK;
   }else{
    if(!a733_rtc_arm(&service.driver,request.seconds)){retain(c,&result);service.active=2;continue;}
    service.alarm_client=c;rtc_service_armed(1);
    atomic_store_explicit(&service.alarm_active,1,memory_order_release);
    int enabled=rtc_service_enable();
    if(enabled!=1){retain(c,&result);service.active=2;continue;}
    service.active=2;continue;
   }
  }
  publish(c,&result);service.active=2;
 }
 atomic_store_explicit(&service.poisoned,1,memory_order_release);rtc_service_fault();
 for(;;)vTaskDelay(1);
}
static int wait_phase(unsigned phase)
{
 while(atomic_load_explicit(&service.phase,memory_order_acquire)<phase){if(!rtc_service_live()||atomic_load_explicit(&service.poisoned,memory_order_acquire))return 0;vTaskDelay(1);}return 1;
}
static int complete(uint64_t id,unsigned outcome)
{
 struct rtc_service_result result;
 for(;;){int r=rtc_service_copy(id,&result);if(r==2)return 0;if(r==1)break;vTaskDelay(1);}
 if(result.outcome!=outcome||!rtc_service_release(id))return 0;
 /* A stale generation cannot retrieve, cancel, or release a later request. */
 return rtc_service_copy(id,&result)==2&&!rtc_service_cancel(id)&&!rtc_service_release(id);
}
static int request(unsigned kind,uint32_t day,uint32_t hms,unsigned seconds,unsigned outcome)
{
 struct rtc_request r={kind,day,hms,seconds};uint64_t id;
 return rtc_service_submit(&r,&id)==1&&complete(id,outcome);
}
void rtc_service_client(void *context)
{
 unsigned c=(unsigned)(uintptr_t)context;int ok=0;
 while(!atomic_load_explicit(&service.ready,memory_order_acquire))vTaskDelay(1);
 if(c==0){
  if(!request(RTC_REQUEST_SET,service.seed_day,service.seed_hms,0,RTC_REQUEST_OK))goto done;
  atomic_store_explicit(&service.phase,1,memory_order_release);
  if(!request(RTC_REQUEST_READ,0,0,0,RTC_REQUEST_OK)||
     !request(RTC_REQUEST_ALARM,0,0,service.alarm_seconds,RTC_REQUEST_OK)||
     !request(RTC_REQUEST_READ,0,0,0,RTC_REQUEST_OK))goto done;
  atomic_store_explicit(&service.phase,3,memory_order_release);ok=wait_phase(4);
 }else if(c==1){
  if(!wait_phase(1))goto done;
  while(!atomic_load_explicit(&service.alarm_active,memory_order_acquire)){if(!rtc_service_live()||atomic_load_explicit(&service.poisoned,memory_order_acquire))goto done;vTaskDelay(1);}
  if(!request(RTC_REQUEST_READ,0,0,0,RTC_REQUEST_OK)||
     !request(RTC_REQUEST_SET,service.seed_day+1,service.seed_hms,0,RTC_REQUEST_BUSY)||!wait_phase(3)||
     !request(RTC_REQUEST_SET,service.seed_day+1,service.seed_hms,0,RTC_REQUEST_OK)||
     !request(RTC_REQUEST_READ,0,0,0,RTC_REQUEST_OK))goto done;
  struct rtc_request r={.kind=RTC_REQUEST_ALARM,.seconds=service.alarm_seconds};uint64_t id;
  if(rtc_service_submit(&r,&id)!=1)goto done;
  while(!atomic_load_explicit(&service.alarm_active,memory_order_acquire)){if(!rtc_service_live()||atomic_load_explicit(&service.poisoned,memory_order_acquire))goto done;vTaskDelay(1);}
  if(!rtc_service_cancel(id)||!complete(id,RTC_REQUEST_CANCELLED)||
     !request(RTC_REQUEST_READ,0,0,0,RTC_REQUEST_OK))goto done;
  atomic_store_explicit(&service.phase,4,memory_order_release);ok=1;
 }
done:
 if(!ok)atomic_store_explicit(&service.poisoned,1,memory_order_release);
 taskENTER_CRITICAL();
 if(atomic_load_explicit(&service.done,memory_order_acquire)<2)atomic_fetch_add_explicit(&service.done,1,memory_order_acq_rel);
 taskEXIT_CRITICAL();
 for(;;)vTaskDelay(1);
}
int rtc_copy_service(struct rtc_service_output *out,uint8_t hash[32])
{
 if(!out||!hash)return 2;memset(out,0,sizeof(*out));memset(hash,0,32);
 if(!rtc_service_live())return 2;
 if(atomic_load_explicit(&service.done,memory_order_acquire)!=3)return 0;
 if(atomic_load_explicit(&service.poisoned,memory_order_acquire)&&!service.output.retained)return 2;
 *out=service.output;memcpy(hash,service.digest,32);
 if(!rtc_service_live()||(atomic_load_explicit(&service.poisoned,memory_order_acquire)&&!service.output.retained)){
  memset(out,0,sizeof(*out));memset(hash,0,32);return 2;
 }return 1;
}
