/* Same service tasks/queue and RTC driver bodies, real pinned FreeRTOS POSIX
 * scheduler. Only native MMIO/provider/GIC boundaries are modeled. */
#include "service-port.h"
#include "semphr.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "a733_storage_digest.h"
static uint32_t reg[32];
static uint64_t epoch_tick,busy_hms,busy_day,gic_until;
static unsigned irq_pending,irq_armed,spi_enabled,lost,writes,source_restores,waited_gic,failed,finished;
static const char *variant;
static TaskHandle_t owner,clients[2];
static StaticTask_t tasks[4];static StackType_t stacks[4][16384];
static StaticSemaphore_t mutex_storage;static SemaphoreHandle_t mutex;
static struct rtc_service_output saved;
static atomic_uint store_seen,client0_id,clients_done;
static uint64_t stamp(void){return (uint64_t)xTaskGetTickCount()*1000+1000;}
int rtc_service_live(void){return !lost&&stamp()<10000000;}
uint64_t rtc_service_deadline(void){return 10000000;}
static void update(void)
{
 uint64_t elapsed=((uint64_t)xTaskGetTickCount()-epoch_tick)/1000;
 if(elapsed){
  uint32_t seconds=(reg[5]>>16)*3600+((reg[5]>>8)&63)*60+(reg[5]&63)+(uint32_t)elapsed;
  reg[4]+=seconds/86400;seconds%=86400;reg[5]=(seconds/3600)<<16|((seconds/60)%60)<<8|seconds%60;
  epoch_tick+=elapsed*1000;
 }
 uint64_t current=(uint64_t)reg[4]*86400+(reg[5]>>16)*3600+((reg[5]>>8)&63)*60+(reg[5]&63);
 uint64_t target=(uint64_t)(reg[8]&65535)*86400+((reg[9]>>16)&31)*3600+((reg[9]>>8)&63)*60+(reg[9]&63);
 if((reg[10]&reg[11]&1)&&current>=target)reg[12]|=1;
 if(irq_armed&&spi_enabled&&(reg[12]&1)){spi_enabled=0;irq_pending=1;}
}
int rtc_service_now(void *c,uint64_t *v){assert(!c);*v=stamp();return rtc_service_live();}
static void enter(void)
{
 assert(xTaskGetCurrentTaskHandle()==owner&&rtc_service_live());
 assert(xSemaphoreTake(mutex,1)==pdTRUE);update();
}
static void leave(void){assert(xSemaphoreGive(mutex)==pdTRUE);}
int rtc_service_read(void *c,uint32_t offset,uint32_t *v)
{
 assert(!c&&offset<=0x50&&!(offset&3));enter();
 if(offset==0){reg[0]&=~0x180u;if(stamp()<busy_hms)reg[0]|=256;if(stamp()<busy_day)reg[0]|=128;}
 *v=reg[offset/4];leave();return rtc_service_live();
}
int rtc_service_write(void *c,uint32_t offset,uint32_t v)
{
 assert(!c&&(offset==0x10||offset==0x14||offset==0x20||offset==0x24||offset==0x28||offset==0x2c||offset==0x30));enter();
 if(offset==0x14&&rtc_service_first_attempt()&&rtc_service_active_cancel()){leave();return 2;}
 ++writes;
 if(offset==0x30){assert(v==1);reg[12]&=~1u;}
 else reg[offset/4]=v;
 if(offset==0x14){rtc_service_note_attempt();epoch_tick=xTaskGetTickCount();busy_hms=stamp()+5000;atomic_store(&store_seen,1);}
 if(offset==0x10)busy_day=stamp()+5000;
 if(offset==0x20&&v==0xabcd)++source_restores;
 if(!strcmp(variant,"partial")&&offset==0x10){leave();return 0;}
 if(!strcmp(variant,"loss")&&offset==0x14){lost=1;leave();return 0;}
 leave();return rtc_service_live();
}
int rtc_service_quiet(unsigned pending)
{
 assert(xTaskGetCurrentTaskHandle()==owner);
 if(spi_enabled)return 0;
 if(pending&&stamp()<gic_until){++waited_gic;return 0;}
 return !pending||!irq_pending;
}
int rtc_service_mask(void){spi_enabled=0;return rtc_service_live();}
int rtc_service_enable(void){assert(irq_armed&&!irq_pending);spi_enabled=1;update();return 1;}
int rtc_service_pending(void){return irq_pending;}
int rtc_service_irq_fault(void){return 0;}
void rtc_service_armed(unsigned v){irq_armed=v;}
void rtc_service_clear_pending(void)
{
 irq_pending=0;if(!strcmp(variant,"gic-delay"))gic_until=stamp()+5000;
}
void rtc_service_fault(void)
{
 assert(!rtc_service_live());failed=1;finished=1;vTaskEndScheduler();
}
void rtc_service_hash(const void *p,unsigned n,uint8_t hash[32]){storage_digest_bytes(p,n,hash);}
void rtc_service_terminal(const struct rtc_service_output *out)
{
 assert(xTaskGetCurrentTaskHandle()==owner);saved=*out;
 if(!strcmp(variant,"partial")){assert(out->retained&&!out->complete&&out->count&&out->records[0].outcome==RTC_REQUEST_RETAINED);}
 else{assert(out->complete&&!out->retained&&out->count==10&&source_restores==2);if(!strcmp(variant,"gic-delay"))assert(waited_gic);}
 assert(!spi_enabled&&reg[0]==0x401d&&reg[1]==1&&reg[20]==0&&reg[16]==0xdeadbeef);
 if(out->complete)assert(!(reg[10]&1)&&!(reg[11]&1)&&!(reg[12]&1)&&reg[8]==0xabcd&&reg[9]==0x40000000);
 finished=1;vTaskEndScheduler();
}
static void run_owner(void *p){(void)p;rtc_service_task();abort();}
static struct rtc_service_result wait_result(uint64_t id)
{
 struct rtc_service_result out;
 for(;;){int r=rtc_service_copy(id,&out);assert(r!=2);if(r==1)return out;vTaskDelay(1);}
}
static void custom_client(void *p)
{
 unsigned c=(unsigned)(uintptr_t)p;
 if(!c){
  struct rtc_request r={RTC_REQUEST_SET,20744,0x123438,0};uint64_t id;
  assert(rtc_service_submit(&r,&id)==1);atomic_store(&client0_id,(unsigned)id);
  while(!atomic_load(&store_seen))vTaskDelay(1);
  assert(rtc_service_cancel(id));
  struct rtc_service_result result=wait_result(id);assert(result.outcome==RTC_REQUEST_LATE_CANCEL&&result.calendar.operation.writes==2);
  assert(rtc_service_release(id));r=(struct rtc_request){.kind=RTC_REQUEST_READ};uint64_t next;
  assert(rtc_service_submit(&r,&next)==1&&next>id);
  assert(!rtc_service_cancel(id)&&!rtc_service_release(id)&&rtc_service_copy(id,&result)==2);
  result=wait_result(next);assert(result.outcome==RTC_REQUEST_OK&&rtc_service_release(next));
 }else{
  while(!atomic_load(&store_seen))vTaskDelay(1);
  uint64_t other=atomic_load(&client0_id);struct rtc_service_result output;
  /* IDs never substitute for a registered current-task identity. */
  assert(!rtc_service_cancel(other)&&!rtc_service_release(other)&&rtc_service_copy(other,&output)==2);
  struct rtc_request r={.kind=RTC_REQUEST_READ};uint64_t id;
  assert(rtc_service_submit(&r,&id)==1&&rtc_service_cancel(id));
  output=wait_result(id);assert(output.outcome==RTC_REQUEST_CANCELLED&&rtc_service_release(id));
 }
 atomic_fetch_add(&clients_done,1);for(;;)vTaskDelay(1);
}
static void monitor(void *p)
{
 (void)p;struct rtc_request r={.kind=RTC_REQUEST_READ};uint64_t id=99;
 assert(rtc_service_submit(&r,&id)==2&&!id);
 for(;;){
  if(xSemaphoreGetMutexHolder(mutex))assert(eTaskGetState(owner)!=eBlocked);update();
  if(!strcmp(variant,"cancel-queue")&&atomic_load(&clients_done)==2){finished=1;assert(writes==2);vTaskEndScheduler();}
  assert(stamp()<9000000);vTaskDelay(1);
 }
}
int main(int argc,char **argv)
{
 assert(argc==3);variant=argv[1];
 reg[0]=0x401d;reg[1]=1;reg[8]=0xabcd;reg[9]=0x40000000;reg[16]=0xdeadbeef;
 mutex=xSemaphoreCreateMutexStatic(&mutex_storage);assert(mutex);
 int custom=!strcmp(variant,"cancel-queue");
 clients[0]=xTaskCreateStatic(custom?custom_client:rtc_service_client,"client0",16384,NULL,3,stacks[0],&tasks[0]);
 clients[1]=xTaskCreateStatic(custom?custom_client:rtc_service_client,"client1",16384,(void *)1,3,stacks[1],&tasks[1]);
 assert(rtc_service_prepare(clients,20744,0x123438,1));
 owner=xTaskCreateStatic(run_owner,"rtc-owner",16384,NULL,4,stacks[2],&tasks[2]);
 assert(xTaskCreateStatic(monitor,"monitor",16384,NULL,5,stacks[3],&tasks[3]));
 vTaskStartScheduler();assert(finished);
 if(!strcmp(variant,"loss"))assert(failed&&lost&&writes==1);
 FILE *f=fopen(argv[2],"wb");assert(f&&fwrite(&saved,1,sizeof(saved),f)==sizeof(saved)&&!fclose(f));
 printf("PASS %s: real FreeRTOS owner/queue/clients; %u writes, %u delayed GIC polls\n",variant,writes,waited_gic);return 0;
}
